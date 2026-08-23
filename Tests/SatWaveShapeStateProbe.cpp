#include "../Source/PluginProcessor.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

float boundedBezier(float t, float curvature) noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);
    curvature = juce::jlimit(-1.0f, 1.0f, curvature);
    float control1 = 1.0f / 3.0f;
    float control2 = 2.0f / 3.0f;
    if (curvature >= 0.0f)
    {
        control1 += curvature * (2.0f / 3.0f);
        control2 += curvature * (1.0f / 3.0f);
    }
    else
    {
        const auto magnitude = -curvature;
        control1 *= 1.0f - magnitude;
        control2 *= 1.0f - magnitude;
    }
    const auto inverse = 1.0f - t;
    return 3.0f * inverse * inverse * t * control1
         + 3.0f * inverse * t * t * control2 + t * t * t;
}

float evaluate(const TR::Curves::Curve& curve, float x)
{
    std::size_t segment = 0;
    while (segment + 2 < curve.points.size() && x > curve.points[segment + 1].x)
        ++segment;
    const auto& left = curve.points[segment];
    const auto& right = curve.points[segment + 1];
    const auto span = right.x - left.x;
    const auto local = span > 0.0f ? (x - left.x) / span : 0.0f;
    return left.y + (right.y - left.y) * boundedBezier(local, left.curvature);
}

void setPlain(juce::AudioProcessorValueTreeState& state, const char* id, float value)
{
    auto* parameter = state.getParameter(id);
    require(parameter != nullptr, "required SAT parameter is missing");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float rawValue(juce::AudioProcessorValueTreeState& state, const char* id)
{
    const auto* value = state.getRawParameterValue(id);
    require(value != nullptr, "required SAT raw parameter is missing");
    return value->load(std::memory_order_relaxed);
}

juce::MemoryBlock withoutWaveShapeState(SATTRAudioProcessor& processor)
{
    juce::MemoryBlock current;
    processor.getStateInformation(current);
    auto xml = juce::AudioProcessor::getXmlFromBinary(current.getData(),
                                                       static_cast<int>(current.getSize()));
    require(xml != nullptr, "could not decode SAT state XML");
    auto tree = juce::ValueTree::fromXml(*xml);
    const auto waveShape = tree.getChildWithName(SATTR::WaveShape::stateTreeType());
    require(waveShape.isValid(), "canonical state did not contain WAVESHAPER_STATE");
    tree.removeChild(waveShape, nullptr);
    auto legacyXml = tree.createXml();
    require(legacyXml != nullptr, "could not encode legacy SAT state XML");
    juce::MemoryBlock legacy;
    juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacy);
    return legacy;
}

void verifyMirrorAndRestoration()
{
    auto state = SATTR::WaveShape::makeDefaultState();
    auto& loader = state.loaders[0];
    auto& slot = loader.slots[0];
    slot.unipolar.points = {
        { 0.0f, 0.0f, 0.65f },
        { 0.2f, 0.62f, -0.4f },
        { 0.73f, 0.38f, 0.2f },
        { 1.0f, 1.0f, 0.0f }
    };
    const auto original = slot.unipolar;

    require(SATTR::WaveShape::setPolarityMode(loader, SATTR::WaveShape::PolarityMode::bipolar),
            "could not enable bipolar mode");
    require(slot.bipolarInitialised, "bipolar curve was not initialised");
    require(slot.bipolar.points.size() == original.points.size() * 2 - 1,
            "bipolar mirror has the wrong point count");
    require(slot.unipolar == original, "enabling bipolar changed the unipolar source");

    for (int sample = 0; sample <= 1000; ++sample)
    {
        const float x = static_cast<float>(sample) / 1000.0f;
        const float error = std::abs(evaluate(slot.bipolar, -x) + evaluate(original, x));
        require(error <= 2.0e-6f, "bipolar curve is not the exact X/Y mirror");
    }

    slot.bipolar.points[1].y += 0.05f;
    const auto editedBipolar = slot.bipolar;
    SATTR::WaveShape::setPolarityMode(loader, SATTR::WaveShape::PolarityMode::unipolar);
    require(slot.unipolar == original, "returning to unipolar did not restore the source curve");
    SATTR::WaveShape::setPolarityMode(loader, SATTR::WaveShape::PolarityMode::bipolar);
    require(slot.bipolar == editedBipolar, "returning to bipolar destroyed its independent edit");

    auto invalidLoader = SATTR::WaveShape::makeDefaultState().loaders[0];
    invalidLoader.slots[0].unipolar.points[1].x = 0.0f;
    const auto invalidBefore = invalidLoader;
    require(! SATTR::WaveShape::setPolarityMode(
                invalidLoader, SATTR::WaveShape::PolarityMode::bipolar),
            "invalid unipolar source was mirrored");
    require(invalidLoader == invalidBefore, "failed bipolar initialisation mutated loader state");
}

void verifyStateCodec()
{
    auto state = SATTR::WaveShape::makeDefaultState();
    state.loaders[0].enabled = true;
    SATTR::WaveShape::setPolarityMode(state.loaders[2], SATTR::WaveShape::PolarityMode::bipolar);
    state.loaders[2].slots[1].unipolar.points = {
        { 0.0f, 0.0f, 0.0f }, { 0.4f, 0.8f, -0.25f }, { 1.0f, 0.2f, 0.0f }
    };

    SATTR::WaveShape::ValidationReport report;
    const auto encoded = SATTR::WaveShape::encodeState(state, &report);
    require(encoded.has_value() && report.ok(), "valid WaveShape state did not encode");
    const auto decoded = SATTR::WaveShape::decodeState(*encoded);
    require(decoded.ok && decoded.state == state, "WaveShape state did not round-trip exactly");

    juce::ValueTree oldParent("SATTRState");
    const auto missing = SATTR::WaveShape::readStateFromParent(oldParent);
    require(missing.ok && missing.migratedFromMissingState,
            "missing WaveShape state was not treated as a legacy preset");
    require(missing.state == SATTR::WaveShape::makeDefaultState(),
            "legacy preset did not receive disabled default WaveShape state");

    auto malformed = *encoded;
    malformed.setProperty("schema", 999, nullptr);
    require(! SATTR::WaveShape::decodeState(malformed).ok,
            "unsupported WaveShape schema was accepted");

    auto invalid = state;
    invalid.loaders[0].slots[0].unipolar.points[1].x = 0.0f;
    require(! SATTR::WaveShape::validate(invalid).ok(), "invalid curve was accepted");
    require(! SATTR::WaveShape::encodeState(invalid).has_value(),
            "invalid curve was serialised");

    auto uninitialisedBipolar = SATTR::WaveShape::makeDefaultState();
    uninitialisedBipolar.loaders[0].polarity = SATTR::WaveShape::PolarityMode::bipolar;
    require(! SATTR::WaveShape::validate(uninitialisedBipolar).ok(),
            "active bipolar state without mirrored curves was accepted");
}

void verifyProcessorCompatibility()
{
    auto source = std::make_unique<SATTRAudioProcessor>();
    auto& sourceParameters = source->getValueTreeState();
    setPlain(sourceParameters, SATTRAudioProcessor::kParamSatTypeA, 6.0f);
    setPlain(sourceParameters, SATTRAudioProcessor::kParamSatTypeB, 7.0f);
    setPlain(sourceParameters, SATTRAudioProcessor::kParamSatTypeC, 8.0f);
    require(sourceParameters.getParameter("waveshape_mode_a") == nullptr,
            "WaveShape structural mode leaked into host automation parameters");

    const auto legacy = withoutWaveShapeState(*source);
    source.reset();
    auto restoredLegacy = std::make_unique<SATTRAudioProcessor>();
    restoredLegacy->setStateInformation(legacy.getData(), static_cast<int>(legacy.getSize()));
    auto& restoredParameters = restoredLegacy->getValueTreeState();
    require(rawValue(restoredParameters, SATTRAudioProcessor::kParamSatTypeA) == 6.0f
                && rawValue(restoredParameters, SATTRAudioProcessor::kParamSatTypeB) == 7.0f
                && rawValue(restoredParameters, SATTRAudioProcessor::kParamSatTypeC) == 8.0f,
            "legacy sat_type values changed while migrating state");
    require(restoredLegacy->waveShapeState() == SATTR::WaveShape::makeDefaultState(),
            "legacy preset unexpectedly enabled WaveShape");

    require(restoredLegacy->setWaveShapeEnabled(0, true), "could not enable WaveShape loader A");
    require(restoredLegacy->setWaveShapePolarityMode(0, SATTR::WaveShape::PolarityMode::bipolar),
            "could not set WaveShape polarity");
    require(rawValue(restoredParameters, SATTRAudioProcessor::kParamSatTypeA) == 6.0f,
            "WaveShape extension changed the historical sat_type value");

    juce::MemoryBlock current;
    restoredLegacy->getStateInformation(current);
    const auto expectedWaveShape = restoredLegacy->waveShapeState();
    restoredLegacy.reset();
    auto restoredCurrent = std::make_unique<SATTRAudioProcessor>();
    restoredCurrent->setStateInformation(current.getData(), static_cast<int>(current.getSize()));
    require(restoredCurrent->waveShapeState() == expectedWaveShape,
            "processor WaveShape state did not survive preset round-trip");
    require(rawValue(restoredCurrent->getValueTreeState(), SATTRAudioProcessor::kParamSatTypeA) == 6.0f,
            "current preset round-trip changed sat_type");
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        verifyMirrorAndRestoration();
        verifyStateCodec();
        verifyProcessorCompatibility();
        std::cout << "SAT WaveShape phase 1 state probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SAT WaveShape phase 1 state probe failed: " << error.what() << '\n';
        return 1;
    }
}

#include "SatBackendBindings.h"

#include "SatUiDefinition.h"
#include "../PluginProcessor.h"
#include "../Modulation/SatModulationConfig.h"
#include "../WaveShape/SatWaveShapeCore.h"
#include "../../../TR-Shared/Modulation/Integration/TRModulationPresetCodec.h"

#include <cmath>
#include <cctype>
#include <limits>

namespace SATTR::UIV2
{
namespace L = TR::LoaderUIV2;
namespace S = TR::SimpleUIV2;

namespace
{
constexpr const char* selectedTaskKey = "uiV2SelectedTask";
constexpr const char* surfaceKey = "uiV2Surface";
constexpr const char* selectedScopeKey = "uiV2SelectedScope";
constexpr const char* waveShapePresetStateId = "sat_waveshape_state";

bool waveShapeFromMusicalState(const S::MusicalState& state,
                               SATTR::WaveShape::State& destination) noexcept
{
    const auto found = state.textValues.find(waveShapePresetStateId);
    if (found == state.textValues.end())
    {
        destination = SATTR::WaveShape::makeDefaultState();
        return true;
    }
    const auto xml = juce::XmlDocument::parse(juce::String(found->second));
    if (xml == nullptr) return false;
    const auto decoded = SATTR::WaveShape::decodeState(juce::ValueTree::fromXml(*xml));
    if (!decoded.ok) return false;
    destination = decoded.state;
    return true;
}

void writeWaveShapeText(S::MusicalState& state, const SATTR::WaveShape::State& waveShape)
{
    SATTR::WaveShape::ValidationReport report;
    if (const auto tree = SATTR::WaveShape::encodeState(waveShape, &report))
        if (const auto xml = tree->createXml())
            state.textValues[waveShapePresetStateId] =
                xml->toString(juce::XmlElement::TextFormat().singleLine()).toStdString();
}

const char* enableId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamEnableA,
                                  SATTRAudioProcessor::kParamEnableB,
                                  SATTRAudioProcessor::kParamEnableC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* typeId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatTypeA,
                                  SATTRAudioProcessor::kParamSatTypeB,
                                  SATTRAudioProcessor::kParamSatTypeC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* driveId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatDriveA,
                                  SATTRAudioProcessor::kParamSatDriveB,
                                  SATTRAudioProcessor::kParamSatDriveC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* characterId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatCharA,
                                  SATTRAudioProcessor::kParamSatCharB,
                                  SATTRAudioProcessor::kParamSatCharC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* modelControlId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatTypeCtrlA,
                                  SATTRAudioProcessor::kParamSatTypeCtrlB,
                                  SATTRAudioProcessor::kParamSatTypeCtrlC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* biasId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatBiasA,
                                  SATTRAudioProcessor::kParamSatBiasB,
                                  SATTRAudioProcessor::kParamSatBiasC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* dynamicsId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatSagA,
                                  SATTRAudioProcessor::kParamSatSagB,
                                  SATTRAudioProcessor::kParamSatSagC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* detailId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamDetailA,
                                  SATTRAudioProcessor::kParamDetailB,
                                  SATTRAudioProcessor::kParamDetailC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* instabilityId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamInstabilityA,
                                  SATTRAudioProcessor::kParamInstabilityB,
                                  SATTRAudioProcessor::kParamInstabilityC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* seriesId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSeriesA,
                                  SATTRAudioProcessor::kParamSeriesB,
                                  SATTRAudioProcessor::kParamSeriesC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* rawId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamSatRawA,
                                  SATTRAudioProcessor::kParamSatRawB,
                                  SATTRAudioProcessor::kParamSatRawC };
    return ids[juce::jlimit(0, 2, index)];
}

const char* mixId(int index)
{
    constexpr const char* ids[] { SATTRAudioProcessor::kParamMixA,
                                  SATTRAudioProcessor::kParamMixB,
                                  SATTRAudioProcessor::kParamMixC };
    return ids[juce::jlimit(0, 2, index)];
}

float parameterValue(const juce::AudioProcessorValueTreeState& state, const char* id)
{
    if (const auto* value = state.getRawParameterValue(id)) return value->load();
    return 0.0f;
}

void setParameter(juce::AudioProcessorValueTreeState& state, const char* id, float plainValue)
{
    if (auto* parameter = state.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}
}

SatBackendBindings::SatBackendBindings(SATTRAudioProcessor& value) : processor(value) {}
SatBackendBindings::~SatBackendBindings() = default;

SATTR::WaveShape::State SatBackendBindings::waveShapeState() const
{
    return processor.waveShapeState();
}

bool SatBackendBindings::setWaveShapeState(const SATTR::WaveShape::State& state)
{
    return processor.setWaveShapeState(state);
}

bool SatBackendBindings::setWaveShapeEnabled(int loaderIndex, bool enabled)
{
    return processor.setWaveShapeEnabled(loaderIndex, enabled);
}

bool SatBackendBindings::isWaveShapeEnabled(int loaderIndex) const noexcept
{
    return processor.isWaveShapeEnabled(loaderIndex);
}

juce::AudioProcessorValueTreeState& SatBackendBindings::parameters() const noexcept
{
    return processor.getValueTreeState();
}

S::ParameterSnapshot SatBackendBindings::parameterSnapshot() const
{
    S::ParameterSnapshot result;
    updateParameterSnapshot(result);
    return result;
}

void SatBackendBindings::updateParameterSnapshot(S::ParameterSnapshot& destination) const
{
    destination.clear();
    for (const auto& item : definition().parameters)
        if (item.domain == S::StateDomain::musicalParameter)
            if (auto* value = parameters().getRawParameterValue(item.parameterId))
                destination[item.parameterId] = value->load();
    const auto musical = readMusicalState();
    for (const auto& item : musical.values) destination[item.first] = item.second;
    for (int index = 0; index < 3; ++index)
        destination["waveshape_enabled_" + std::string(1, static_cast<char>('a' + index))]
            = processor.isWaveShapeEnabled(index) ? 1.0 : 0.0;
}

std::optional<juce::String> SatBackendBindings::formatControlValue(
    std::string_view controlId, double value) const
{
    const auto id = juce::String(controlId.data(), controlId.size()).toLowerCase();
    const auto has = [&id](const char* token) { return id.contains(token); };
    const auto isGain = has("global-input") || has("global-output")
                        || id.startsWith("input-") || id.startsWith("output-")
                        || has("sidechain-gain") || has("exp-sc-gain");
    if (isGain)
        return value <= SATTRAudioProcessor::kGainFloorDb + 0.01
                   ? juce::String("-INF")
                   : juce::String(value, 1) + " dB";

    if (has("limit-threshold") || has("exp-threshold") || has("exp-knee")
        || id.startsWith("tilt_"))
        return juce::String(value, 1) + " dB";

    if (has("dry-level"))
    {
        if (value <= 0.0001) return juce::String("-INF");
        return juce::String(20.0 * std::log10(value), 1) + " dB";
    }

    const bool percent01 = has("macro-drive") || has("macro-character")
                           || has("macro-type") || has("macro-nam-size")
                           || has("macro-mix") || has("global-mix")
                           || has("waveshape-morph")
                           || has("dynamics-") || has("detail-")
                           || has("instability-") || has("frequency-")
                           || has("position-") || has("bias-")
                           || has("resonance-") || has("sidechain-smooth");
    if (percent01)
    {
        const auto display = std::abs(value) < 0.0005 ? 0.0 : value;
        return juce::String(display * 100.0, 1) + "%";
    }

    if (has("chaos-amount") || has("chaos-filter-amount"))
        return juce::String(value, 1) + "%";
    if (has("chaos-speed") || has("chaos-filter-speed"))
        return juce::String(value, value >= 10.0 ? 1 : 2) + " Hz";

    if (has("series-")) return juce::String(juce::roundToInt(value)) + "x";
    if (has("offset-"))
        return value >= 1000.0 ? juce::String(value / 1000.0, 2) + " s"
                               : juce::String(value, value >= 100.0 ? 1 : 2) + " ms";

    if (has("pan-"))
    {
        const int percent = juce::roundToInt((value - 0.5) * 200.0);
        if (percent == 0) return juce::String("C");
        return percent < 0 ? "L" + juce::String(-percent) : "R" + juce::String(percent);
    }

    if (has("slope"))
    {
        constexpr int labels[] { 6, 12, 24 };
        return juce::String(labels[juce::jlimit(0, 2, juce::roundToInt(value))]) + " dB/oct";
    }
    const bool frequency = !has("-on") && (has("hp-freq") || has("lp-freq")
                           || has("sidechain-hp-") || has("sidechain-lp-")
                           || has("exp-sc-hp-") || has("exp-sc-lp-")
                           || id.startsWith("hp_freq_") || id.startsWith("lp_freq_"));
    if (frequency)
        return value >= 1000.0 ? juce::String(value / 1000.0, 2) + " kHz"
                               : juce::String(value, value >= 100.0 ? 1 : 2) + " Hz";

    if (has("exp-attack") || has("exp-release"))
        return juce::String(value, value >= 100.0 ? 1 : 2) + " ms";
    if (has("exp-ratio")) return "1:" + juce::String(value, 1);
    return std::nullopt;
}

std::optional<double> SatBackendBindings::parseControlValue(
    std::string_view controlId, const juce::String& text) const
{
    const auto id = juce::String(controlId.data(), controlId.size()).toLowerCase();
    const auto input = text.trim().toUpperCase().replaceCharacter(',', '.');
    const auto number = input.retainCharacters("0123456789-+.").getDoubleValue();
    const auto has = [&id](const char* token) { return id.contains(token); };

    const auto isGain = has("global-input") || has("global-output")
                        || id.startsWith("input-") || id.startsWith("output-")
                        || has("sidechain-gain") || has("exp-sc-gain")
                        || has("limit-threshold") || has("exp-threshold")
                        || has("exp-knee") || id.startsWith("tilt_");
    if (isGain) return input.contains("INF") ? SATTRAudioProcessor::kGainFloorDb : number;
    if (has("dry-level"))
        return input.contains("INF") ? 0.0 : std::pow(10.0, number / 20.0);

    const bool percent01 = has("macro-drive") || has("macro-character")
                           || has("macro-type") || has("macro-nam-size")
                           || has("macro-mix") || has("global-mix")
                           || has("waveshape-morph")
                           || has("dynamics-") || has("detail-")
                           || has("instability-") || has("frequency-")
                           || has("position-") || has("bias-")
                           || has("resonance-") || has("sidechain-smooth");
    if (percent01) return number / 100.0;
    if (has("chaos-amount") || has("chaos-filter-amount")) return number;
    if (has("chaos-speed") || has("chaos-filter-speed")) return number;
    if (has("series-")) return static_cast<double>(juce::roundToInt(number));
    if (has("offset-")) return input.endsWith("S") && !input.endsWith("MS") ? number * 1000.0 : number;
    if (has("pan-"))
    {
        if (input == "C" || input == "CENTER" || input == "CENTRE") return 0.5;
        if (input.startsWith("L")) return 0.5 - juce::jlimit(0.0, 100.0, number) / 200.0;
        if (input.startsWith("R")) return 0.5 + juce::jlimit(0.0, 100.0, number) / 200.0;
        return number;
    }
    if (has("slope"))
    {
        if (number >= 18.0) return 2.0;
        if (number >= 9.0) return 1.0;
        return 0.0;
    }
    const bool frequency = !has("-on") && (has("hp-freq") || has("lp-freq")
                           || has("sidechain-hp-") || has("sidechain-lp-")
                           || has("exp-sc-hp-") || has("exp-sc-lp-")
                           || id.startsWith("hp_freq_") || id.startsWith("lp_freq_"));
    if (frequency) return input.contains("K") ? number * 1000.0 : number;
    if (has("exp-attack") || has("exp-release") || has("exp-ratio")) return number;
    return std::nullopt;
}

bool SatBackendBindings::invokeContextualAction(std::string_view actionId)
{
    const auto id = std::string(actionId);
    for (int index = 0; index < 3; ++index)
    {
        const auto suffix = std::string(1, static_cast<char>('a' + index));
        if (id == "browse-nam-" + suffix) { browseNam(index); return true; }
        if (id == "clear-nam-" + suffix) { clearNam(index); return true; }
        if (id == "select-waveshape-" + suffix)
            return processor.setWaveShapeEnabled(index, true);
        if (id == "select-legacy-model-" + suffix)
            return processor.setWaveShapeEnabled(index, false);
        const auto legacyPrefix = "select-legacy-model-" + suffix + "-";
        if (id.rfind(legacyPrefix, 0) == 0)
        {
            const auto rawValue = juce::String(id.substr(legacyPrefix.size())).getIntValue();
            auto* parameter = parameters().getParameter(typeId(index));
            if (parameter == nullptr || rawValue < 0 || rawValue > 8) return false;
            if (!processor.setWaveShapeEnabled(index, false)) return false;
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(rawValue)));
            parameter->endChangeGesture();
            return true;
        }
    }
    if (id == "trigger-align") { triggerAlign(); return true; }
    return false;
}

std::optional<int> SatBackendBindings::contextualChoiceIndex(std::string_view controlId) const
{
    const auto id = std::string(controlId);
    if (id.rfind("model-", 0) != 0 || id.size() != 7) return std::nullopt;
    const int loader = static_cast<int>(std::toupper(static_cast<unsigned char>(id.back())) - 'A');
    if (!juce::isPositiveAndBelow(loader, 3)) return std::nullopt;
    if (processor.isWaveShapeEnabled(loader)) return 8;
    const auto* raw = parameters().getRawParameterValue(typeId(loader));
    const int legacy = raw != nullptr ? juce::roundToInt(raw->load()) : 0;
    constexpr std::array<int, 9> legacyToVisible { 0, 1, 2, 3, 4, 5, 7, 9, 6 };
    return legacyToVisible[static_cast<std::size_t>(juce::jlimit(0, 8, legacy))];
}

int SatBackendBindings::selectedLoaderIndex() const noexcept
{
    const auto scope = readLoaderUiInstanceState().selectedScope;
    return scope == L::ScopeId::loaderA ? 0 : (scope == L::ScopeId::loaderB ? 1 :
           (scope == L::ScopeId::loaderC ? 2 : -1));
}

float SatBackendBindings::inputMeterPeak() const noexcept
{
    return processor.getGlobalInputMeterPeak();
}

float SatBackendBindings::outputMeterPeak() const noexcept
{
    return processor.getGlobalOutputMeterPeak();
}

S::MusicalState SatBackendBindings::readMusicalState() const
{
    S::MusicalState state;
    for (int index = 0; index < 3; ++index)
    {
        const auto suffix = std::string(1, static_cast<char>('A' + index));
        state.textValues["namPath" + suffix] = processor.getNamModelPathForLoader(index).toStdString();
        state.textValues["instabilitySeed" + suffix] = juce::String(
            static_cast<juce::int64>(processor.instabilitySeeds()[static_cast<std::size_t>(index)])).toStdString();
        state.values["uiDryAlignAnchor" + suffix] = processor.getDryAlignAnchorForLoader(index);
    }
    state.values["uiDryAlignMode"] = processor.isDryAlignModeEnabled() ? 1.0 : 0.0;
    state.values["uiSafeClipMode"] = processor.isSafeClipModeEnabled() ? 1.0 : 0.0;
    TR::Modulation::Integration::writePresetState(state, processor.modulationState());
    writeWaveShapeText(state, processor.waveShapeState());
    return state;
}

S::MusicalState SatBackendBindings::defaultMusicalState() const
{
    S::MusicalState state;
    state.textValues = { { "namPathA", "" }, { "namPathB", "" }, { "namPathC", "" } };
    const auto seeds = processor.instabilitySeeds();
    for (int index = 0; index < 3; ++index)
        state.textValues["instabilitySeed" + std::string(1, static_cast<char>('A' + index))]
            = juce::String(static_cast<juce::int64>(seeds[static_cast<std::size_t>(index)])).toStdString();
    state.values = { { "uiDryAlignMode", 0.0 }, { "uiSafeClipMode", 0.0 },
                     { "uiDryAlignAnchorA", 0.0 }, { "uiDryAlignAnchorB", 0.0 },
                     { "uiDryAlignAnchorC", 0.0 } };
    TR::Modulation::Integration::writePresetState(state, TR::Modulation::makeDefaultState());
    writeWaveShapeText(state, SATTR::WaveShape::makeDefaultState());
    return state;
}

bool SatBackendBindings::validateMusicalState(const S::MusicalState& state) const noexcept
{
    for (const auto& item : state.values)
    {
        const bool known = item.first == "uiDryAlignMode" || item.first == "uiSafeClipMode"
                           || item.first == "uiDryAlignAnchorA" || item.first == "uiDryAlignAnchorB"
                           || item.first == "uiDryAlignAnchorC"
                           || (item.first == TR::Modulation::Integration::presetStateId
                               && item.second == 0.0);
        if (!known || !std::isfinite(item.second)) return false;
    }
    for (const auto& item : state.textValues)
        if (item.first != "namPathA" && item.first != "namPathB" && item.first != "namPathC"
            && item.first != "instabilitySeedA" && item.first != "instabilitySeedB"
            && item.first != "instabilitySeedC"
            && item.first != TR::Modulation::Integration::presetStateId
            && item.first != waveShapePresetStateId)
            return false;
    TR::Modulation::State modulation;
    SATTR::WaveShape::State waveShape;
    return TR::Modulation::Integration::readPresetState(state, modulation)
        && waveShapeFromMusicalState(state, waveShape);
}

void SatBackendBindings::writeMusicalState(const S::MusicalState& state)
{
    if (!validateMusicalState(state)) return;
    for (int index = 0; index < 3; ++index)
    {
        const auto suffix = std::string(1, static_cast<char>('A' + index));
        if (const auto found = state.textValues.find("namPath" + suffix); found != state.textValues.end())
        {
            const juce::String requested(found->second);
            if (processor.getNamModelPathForLoader(index) != requested)
            {
                if (requested.isEmpty())
                    processor.clearNamModelForLoader(index);
                else
                {
                    juce::String error;
                    processor.loadNamModelForLoader(index, requested, error);
                }
            }
        }
        if (const auto found = state.values.find("uiDryAlignAnchor" + suffix); found != state.values.end())
            processor.setDryAlignAnchorForLoader(index, static_cast<float>(found->second));
    }
    auto seeds = processor.instabilitySeeds();
    for (int index = 0; index < 3; ++index)
    {
        const auto suffix = std::string(1, static_cast<char>('A' + index));
        if (const auto found = state.textValues.find("instabilitySeed" + suffix);
            found != state.textValues.end())
        {
            const auto parsed = juce::String(found->second).getLargeIntValue();
            if (parsed > 0 && parsed <= static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max()))
                seeds[static_cast<std::size_t>(index)] = static_cast<std::uint32_t>(parsed);
        }
    }
    processor.setInstabilitySeeds(seeds);
    if (const auto found = state.values.find("uiDryAlignMode"); found != state.values.end())
        processor.setDryAlignModeEnabled(found->second >= 0.5);
    if (const auto found = state.values.find("uiSafeClipMode"); found != state.values.end())
        processor.setSafeClipModeEnabled(found->second >= 0.5);
    TR::Modulation::State modulation;
    if (TR::Modulation::Integration::readPresetState(state, modulation))
        processor.setModulationState(modulation);
    SATTR::WaveShape::State waveShape;
    if (waveShapeFromMusicalState(state, waveShape))
        processor.setWaveShapeState(waveShape);
}

L::LoaderUiInstanceState SatBackendBindings::readLoaderUiInstanceState() const
{
    const auto& tree = parameters().state;
    L::LoaderUiInstanceState state;
    state.selectedTask = static_cast<S::TaskId>(juce::jlimit(0, 3,
        static_cast<int>(tree.getProperty(selectedTaskKey, 0))));
    state.selectedScope = L::scopeAt(static_cast<std::size_t>(juce::jlimit(0, 3,
        static_cast<int>(tree.getProperty(selectedScopeKey, 0)))));
    state.surface = static_cast<S::UiSurface>(juce::jlimit(
        0, 2, static_cast<int>(tree.getProperty(surfaceKey, 0))));
    return state;
}

void SatBackendBindings::writeLoaderUiInstanceState(const L::LoaderUiInstanceState& state)
{
    auto& tree = parameters().state;
    tree.setProperty(selectedTaskKey, static_cast<int>(state.selectedTask), nullptr);
    tree.setProperty(selectedScopeKey, static_cast<int>(L::scopeIndex(state.selectedScope)), nullptr);
    tree.setProperty(surfaceKey, static_cast<int>(state.surface), nullptr);
}

std::array<L::ScopeStatus, 4> SatBackendBindings::scopeStatuses() const
{
    std::array<L::ScopeStatus, 4> result;
    for (int index = 0; index < 3; ++index)
    {
        auto& status = result[static_cast<std::size_t>(index)];
        status.scope = L::scopeAt(static_cast<std::size_t>(index));
        const bool enabled = parameters().getRawParameterValue(enableId(index))->load() > 0.5f;
        const int model = juce::roundToInt(parameters().getRawParameterValue(typeId(index))->load());
        const bool waveShapeMode = processor.isWaveShapeEnabled(index);
        const bool namMode = !waveShapeMode && model == 7;
        const auto path = processor.getNamModelPathForLoader(index);
        if (namMode && path.isEmpty()) { status.state = L::ScopeState::empty; status.primary = "EMPTY"; }
        else if (namMode && !juce::File(path).existsAsFile())
        {
            status.state = L::ScopeState::error;
            status.primary = "MISSING";
            status.secondary = juce::File(path).getFileName().toStdString();
        }
        else if (!enabled) { status.state = L::ScopeState::bypassed; status.primary = "BYPASSED"; }
        else
        {
            status.state = L::ScopeState::ready;
            if (waveShapeMode)
                status.primary = "WAVE SHAPE";
            else if (auto* parameter = parameters().getParameter(typeId(index)))
                status.primary = parameter->getCurrentValueAsText().toStdString();
            if (namMode) status.secondary = juce::File(path).getFileNameWithoutExtension().toStdString();
        }
    }
    auto& global = result[3];
    global.scope = L::ScopeId::global;
    global.state = L::ScopeState::ready;
    global.primary = "GLOBAL";
    if (auto* route = parameters().getParameter(SATTRAudioProcessor::kParamRoute))
        global.secondary = route->getCurrentValueAsText().toStdString();
    return result;
}

L::LoaderSignatureSnapshot SatBackendBindings::signatureSnapshot(L::ScopeId scope) const
{
    L::LoaderSignatureSnapshot result;
    result.scope = scope;
    const auto statuses = scopeStatuses();
    result.state = statuses[L::scopeIndex(scope)].state;
    result.activity = scope == L::ScopeId::global
                          ? processor.getGlobalOutputMeterPeak()
                          : processor.getLoaderOutputMeterPeak(static_cast<int>(L::scopeIndex(scope)));
    if (scope == L::ScopeId::global)
    {
        result.kind = L::SignatureKind::routingTopology;
        result.primary = "GLOBAL / ROUTING";
        if (auto* route = parameters().getParameter(SATTRAudioProcessor::kParamRoute))
        {
            result.topologyIndex = juce::jlimit(0, 5, juce::roundToInt(
                route->convertFrom0to1(route->getValue())));
            result.secondary = route->getCurrentValueAsText().toStdString();
        }
        for (int index = 0; index < 3; ++index)
            result.loaderActive[static_cast<std::size_t>(index)] =
                statuses[static_cast<std::size_t>(index)].state == L::ScopeState::ready;
        return result;
    }
    const int index = static_cast<int>(L::scopeIndex(scope));
    result.kind = L::SignatureKind::dynamicTransfer;
    juce::String modelName;
    if (processor.isWaveShapeEnabled(index))
        modelName = "WAVE SHAPE";
    else if (auto* parameter = parameters().getParameter(typeId(index)))
        modelName = parameter->getCurrentValueAsText();
    result.primary = std::string(L::scopeName(scope)) + " / " + modelName.toStdString();
    result.secondary = statuses[static_cast<std::size_t>(index)].secondary;
    updateTransferCache(index);
    const auto& cache = transferCaches[static_cast<std::size_t>(index)];
    result.revision = cache.revision;
    result.primaryTrace = cache.trace;
    result.primaryTraceSize = cache.size;
    if (processor.isWaveShapeEnabled(index))
    {
        constexpr int referencePoints = 128;
        const auto fillReference = [&] (float morph, auto& destination,
                                        std::size_t& destinationSize)
        {
            std::array<float, referencePoints> output {};
            if (!processor.makeWaveShapeTransferSnapshotForMorph(
                    index, morph, output.data(), referencePoints)) return;
            destinationSize = referencePoints;
            for (int point = 0; point < referencePoints; ++point)
            {
                destination[static_cast<std::size_t>(point)] = {
                    static_cast<float>(point) / static_cast<float>(referencePoints - 1),
                    juce::jlimit(-1.0f, 1.0f,
                                 output[static_cast<std::size_t>(point)] / 1.15f)
                };
            }
        };
        fillReference(0.0f, result.referenceTraceA, result.referenceTraceASize);
        fillReference(1.0f, result.referenceTraceB, result.referenceTraceBSize);
    }
    return result;
}

void SatBackendBindings::updateTransferCache(int loaderIndex) const
{
    auto& cache = transferCaches[static_cast<std::size_t>(juce::jlimit(0, 2, loaderIndex))];
    const auto& state = parameters();
    const auto structuralRevision = processor.waveShapeRevision();
    if (processor.isWaveShapeEnabled(loaderIndex))
    {
        constexpr int tracePoints = 128;
        const char* morphIds[] = { SATTRAudioProcessor::kParamWaveShapeMorphA,
                                   SATTRAudioProcessor::kParamWaveShapeMorphB,
                                   SATTRAudioProcessor::kParamWaveShapeMorphC };
        const char* biasIds[] = { SATTRAudioProcessor::kParamWaveShapeBiasA,
                                  SATTRAudioProcessor::kParamWaveShapeBiasB,
                                  SATTRAudioProcessor::kParamWaveShapeBiasC };
        const std::array<float, 2> controls {
            parameterValue(state, morphIds[loaderIndex]),
            parameterValue(state, biasIds[loaderIndex])
        };
        if (cache.valid && cache.structuralRevision == structuralRevision
            && cache.key[0] == controls[0] && cache.key[1] == controls[1])
            return;

        std::array<float, tracePoints> output {};
        if (! processor.makeWaveShapeTransferSnapshot(loaderIndex, output.data(), tracePoints))
            return;
        cache.valid = true;
        cache.structuralRevision = structuralRevision;
        cache.key[0] = controls[0];
        cache.key[1] = controls[1];
        ++cache.revision;
        cache.size = tracePoints;
        for (int point = 0; point < tracePoints; ++point)
        {
            const auto x = static_cast<float>(point) / static_cast<float>(tracePoints - 1);
            cache.trace[static_cast<std::size_t>(point)] =
                { x, juce::jlimit(-1.0f, 1.0f, output[static_cast<std::size_t>(point)] / 1.15f) };
        }
        return;
    }
    const std::array<float, 11> key {
        parameterValue(state, typeId(loaderIndex)),
        parameterValue(state, driveId(loaderIndex)),
        parameterValue(state, characterId(loaderIndex)),
        parameterValue(state, modelControlId(loaderIndex)),
        parameterValue(state, biasId(loaderIndex)),
        parameterValue(state, dynamicsId(loaderIndex)),
        parameterValue(state, detailId(loaderIndex)),
        parameterValue(state, instabilityId(loaderIndex)),
        parameterValue(state, seriesId(loaderIndex)),
        parameterValue(state, rawId(loaderIndex)),
        parameterValue(state, mixId(loaderIndex))
    };
    if (cache.valid && cache.structuralRevision == structuralRevision && cache.key == key) return;

    cache.key = key;
    cache.valid = true;
    cache.structuralRevision = structuralRevision;
    ++cache.revision;
    cache.size = 0;

    const auto model = static_cast<SatEngine::Model>(juce::jlimit(
        0, static_cast<int>(SatEngine::Model::NumModels) - 1, juce::roundToInt(key[0])));
    if (model == SatEngine::Model::NAM) return;

    constexpr int cyclePoints = 384;
    constexpr int tracePoints = 128;
    constexpr int warmupCycles = 8;
    constexpr int sweepSamples = cyclePoints * (warmupCycles + 1);
    constexpr float inputRange = 1.15f;
    std::array<float, sweepSamples> sweep {};
    for (int sample = 0; sample < sweepSamples; ++sample)
        sweep[static_cast<std::size_t>(sample)] = inputRange * std::sin(
            juce::MathConstants<float>::twoPi * static_cast<float>(sample)
            / static_cast<float>(cyclePoints));

    if (transferPreviewState == nullptr)
        transferPreviewState = std::make_unique<SatEngine::State>();
    auto& engineState = *transferPreviewState;
    engineState.reset();
    engineState.lastModel = model;
    const bool raw = key[9] >= 0.5f;
    const float mix = key[10];
    const int series = juce::jlimit(1, SatEngine::kMaxSeries, juce::roundToInt(key[8]));
    engineState.deferFullKlonPostEq = model == SatEngine::Model::OverdriveB && !raw;
    engineState.deferOverdriveAPostEq = model == SatEngine::Model::OverdriveA && !raw;

    SatEngine::processBlock(engineState, sweep.data(), sweep.data(), sweepSamples,
                            model, key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                            48000.0f, series, false, raw, nullptr, 1);
    if (engineState.deferOverdriveAPostEq)
        SatEngine::processDeferredOverdriveAPostEq(engineState, sweep.data(), sweep.data(),
                                                   sweepSamples, key[1], 0.0f, series, 48000.0f, raw);
    if (engineState.deferFullKlonPostEq)
        SatEngine::processDeferredFullKlonPostEq(engineState, sweep.data(), sweep.data(),
                                                 sweepSamples, key[1], 1.0f, series, 48000.0f, raw);

    std::array<float, cyclePoints> wetCycle {};
    for (int point = 0; point < cyclePoints; ++point)
        wetCycle[static_cast<std::size_t>(point)] =
            sweep[static_cast<std::size_t>(warmupCycles * cyclePoints + point)];

    // The complete model path contains filters and memory. Align its periodic
    // response before pairing x/y samples; otherwise preview phase rotation can
    // turn a non-inverting waveshaper into a visually descending transfer.
    int alignmentLag = 0;
    double bestCorrelation = -std::numeric_limits<double>::infinity();
    for (int lag = 0; lag < cyclePoints; ++lag)
    {
        double correlation = 0.0;
        for (int point = 0; point < cyclePoints; ++point)
        {
            const float input = std::sin(juce::MathConstants<float>::twoPi
                                         * static_cast<float>(point)
                                         / static_cast<float>(cyclePoints));
            correlation += static_cast<double>(input)
                           * wetCycle[static_cast<std::size_t>((point + lag) % cyclePoints)];
        }
        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            alignmentLag = lag;
        }
    }

    std::array<float, cyclePoints> outputs {};
    float maximum = 0.0f;
    for (int point = 0; point < cyclePoints; ++point)
    {
        const float dry = inputRange * std::sin(
            juce::MathConstants<float>::twoPi * static_cast<float>(point)
            / static_cast<float>(cyclePoints));
        const float wet = wetCycle[static_cast<std::size_t>((point + alignmentLag) % cyclePoints)];
        const float output = dry + (wet - dry) * juce::jlimit(0.0f, 1.0f, mix);
        outputs[static_cast<std::size_t>(point)] = output;
        maximum = juce::jmax(maximum, std::abs(output));
    }
    const float visualGain = maximum > 0.0001f && maximum < inputRange * 0.72f
                                 ? juce::jmin(6.0f, inputRange * 0.82f / maximum)
                                 : 1.0f;

    const auto outputAtPhase = [&outputs](float phase)
    {
        phase -= std::floor(phase);
        const float position = phase * static_cast<float>(cyclePoints);
        const int first = static_cast<int>(std::floor(position)) % cyclePoints;
        const int second = (first + 1) % cyclePoints;
        return juce::jmap(position - std::floor(position),
                          outputs[static_cast<std::size_t>(first)],
                          outputs[static_cast<std::size_t>(second)]);
    };
    for (int point = 0; point < tracePoints; ++point)
    {
        const float x = juce::jmap(static_cast<float>(point)
                                       / static_cast<float>(tracePoints - 1),
                                   -1.0f, 1.0f);
        const float firstAngle = std::asin(x);
        const float secondAngle = juce::MathConstants<float>::pi - firstAngle;
        const float firstPhase = firstAngle >= 0.0f
                                     ? firstAngle / juce::MathConstants<float>::twoPi
                                     : 1.0f + firstAngle / juce::MathConstants<float>::twoPi;
        const float secondPhase = secondAngle / juce::MathConstants<float>::twoPi;
        const float averagedOutput = 0.5f * (outputAtPhase(firstPhase)
                                             + outputAtPhase(secondPhase));
        cache.trace[static_cast<std::size_t>(point)] =
            { juce::jmap(x, -1.0f, 1.0f, 0.0f, 1.0f),
              juce::jlimit(-1.0f, 1.0f,
                           averagedOutput * visualGain / inputRange) };
    }
    cache.size = tracePoints;
}

bool SatBackendBindings::canAcceptAssetDrop(L::ScopeId scope,
                                            const juce::StringArray& files) const
{
    if (scope == L::ScopeId::global || files.size() != 1) return false;
    const juce::File file(files[0]);
    return file.existsAsFile() && file.getFileExtension().equalsIgnoreCase(".nam");
}

bool SatBackendBindings::loadDroppedAsset(L::ScopeId scope, const juce::StringArray& files)
{
    if (!canAcceptAssetDrop(scope, files)) return false;
    const int index = static_cast<int>(L::scopeIndex(scope));
    juce::String error;
    return processor.loadNamModelForLoader(index, files[0], error);
}

void SatBackendBindings::browseNam(int loaderIndex)
{
    auto& chooser = namChoosers[static_cast<std::size_t>(loaderIndex)];
    const auto currentPath = processor.getNamModelPathForLoader(loaderIndex);
    const auto start = currentPath.isNotEmpty() ? juce::File(currentPath).getParentDirectory() : juce::File();
    chooser = std::make_unique<juce::FileChooser>("Load NAM model", start, "*.nam");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, loaderIndex](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file.existsAsFile())
            {
                juce::String error;
                processor.loadNamModelForLoader(loaderIndex, file.getFullPathName(), error);
            }
            namChoosers[static_cast<std::size_t>(loaderIndex)].reset();
        });
}

void SatBackendBindings::clearNam(int loaderIndex)
{
    processor.clearNamModelForLoader(loaderIndex);
}

void SatBackendBindings::triggerAlign()
{
    setParameter(parameters(), SATTRAudioProcessor::kParamAlign, 1.0f);
}
}
void SATTR::UIV2::SatBackendBindings::setMacroName(int index, const juce::String& name)
{
    if (!juce::isPositiveAndBelow(index, TR::Modulation::macroCount)) return;
    auto state = processor.modulationState();
    state.macros[static_cast<std::size_t>(index)].name = name;
    processor.setModulationState(state);
}

TR::Modulation::State SATTR::UIV2::SatBackendBindings::modulationState() const { return processor.modulationState(); }
std::uint64_t SATTR::UIV2::SatBackendBindings::modulationStateGeneration() const noexcept { return processor.modulationStateGeneration(); }
std::array<float, TR::Modulation::macroCount> SATTR::UIV2::SatBackendBindings::modulationMacroValues() const noexcept { return processor.modulationMacroValues(); }
void SATTR::UIV2::SatBackendBindings::setModulationMacroValue(int macro, float value) { processor.setModulationMacroValue(macro, value); }
bool SATTR::UIV2::SatBackendBindings::setModulationState(const TR::Modulation::State& state) { return processor.setModulationState(state); }
TR::Modulation::UI::SourceCapabilities SATTR::UIV2::SatBackendBindings::modulationSourceCapabilities() const noexcept { return { true }; }
std::vector<TR::Modulation::UI::MotionRecipeOption> SATTR::UIV2::SatBackendBindings::modulationRecipeOptions() const
{
    return { { "instability-a", "INSTABILITY A" }, { "instability-b", "INSTABILITY B" },
             { "instability-c", "INSTABILITY C" }, { "instability-all", "INSTABILITY ALL" } };
}
bool SATTR::UIV2::SatBackendBindings::installModulationRecipe(const juce::String& id, int macro)
{
    const std::uint32_t mask = id == "instability-a" ? 0x1u : id == "instability-b" ? 0x2u
        : id == "instability-c" ? 0x4u : id == "instability-all" ? 0x7u : 0u;
    if (mask == 0) return false;
    constexpr std::array<const char*, 3> parameterIds {
        SATTRAudioProcessor::kParamInstabilityA, SATTRAudioProcessor::kParamInstabilityB,
        SATTRAudioProcessor::kParamInstabilityC };
    std::array<juce::RangedAudioParameter*, 3> parameters {};
    float nativeAmount = 0.0f;
    for (int loader = 0; loader < 3; ++loader)
        if ((mask & (1u << loader)) != 0)
        {
            parameters[static_cast<std::size_t>(loader)] =
                processor.getValueTreeState().getParameter(parameterIds[static_cast<std::size_t>(loader)]);
            if (parameters[static_cast<std::size_t>(loader)] == nullptr) return false;
            nativeAmount = juce::jmax(nativeAmount,
                parameters[static_cast<std::size_t>(loader)]->getValue());
        }
    const auto candidate = TR::SatModulation::makeInstabilityParityRecipe(
        processor.modulationState(), processor.instabilitySeeds(), macro, false, mask);
    if (!processor.setModulationState(candidate)) return false;
    processor.setModulationMacroValue(macro - 1, nativeAmount);
    for (int loader = 0; loader < 3; ++loader)
        if (auto* parameter = parameters[static_cast<std::size_t>(loader)])
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(0.0f));
            parameter->endChangeGesture();
        }
    return true;
}
TR::Modulation::Runtime::TelemetrySnapshot SATTR::UIV2::SatBackendBindings::modulationTelemetry() const noexcept { return processor.modulationTelemetry(); }

bool SATTR::UIV2::SatBackendBindings::refreshModulationDestinationOptions(
    std::vector<TR::Modulation::UI::DestinationOption>& options)
{
    bool changed = false;
    for (auto& option : options)
    {
        const auto id = option.id.toStdString();
        const auto hasSuffix = [&id](const char* suffix)
        {
            const auto length = std::char_traits<char>::length(suffix);
            return id.size() >= length && id.compare(id.size() - length, length, suffix) == 0;
        };
        int loader = -1;
        bool requiresLegacyDrive = false;
        bool requiresWaveShape = false;
        if (id.size() > 7 && id.rfind("loader:", 0) == 0 && hasSuffix(":drive"))
        {
            loader = static_cast<int>(id[7] - 'a');
            requiresLegacyDrive = true;
        }
        else if (id.size() > 10 && id.rfind("waveshape:", 0) == 0)
        {
            loader = static_cast<int>(id[10] - 'a');
            requiresWaveShape = true;
        }
        if (!juce::isPositiveAndBelow(loader, 3)) continue;
        const bool waveShape = isWaveShapeEnabled(loader);
        const bool available = requiresWaveShape ? waveShape : (requiresLegacyDrive ? !waveShape : true);
        const juce::String reason = available ? juce::String {}
            : (requiresWaveShape ? juce::String("WAVE SHAPE ONLY") : juce::String("LEGACY MODEL ONLY"));
        changed = changed || option.available != available || option.unavailableReason != reason;
        option.available = available;
        option.unavailableReason = reason;
    }
    return changed;
}

TR::Modulation::UI::SidechainWorkspaceCallbacks SATTR::UIV2::SatBackendBindings::sidechainWorkspaceCallbacks()
{
    static constexpr std::array<const char*, 3> ids {
        SATTRAudioProcessor::kParamSidechainA,
        SATTRAudioProcessor::kParamSidechainB,
        SATTRAudioProcessor::kParamSidechainC
    };
    return {
        true, {}, {}, {}, "Enable sidechain processing for the selected loader",
        { "A", "B", "C" },
        [this] { return sidechainContextIndex; },
        [this](int index) { sidechainContextIndex = juce::jlimit(0, 2, index); },
        [this](int index)
        {
            const auto* value = parameters().getRawParameterValue(ids[static_cast<std::size_t>(juce::jlimit(0, 2, index))]);
            return value != nullptr && value->load(std::memory_order_relaxed) >= 0.5f;
        },
        [this](int index, bool enabled)
        {
            if (auto* parameter = parameters().getParameter(ids[static_cast<std::size_t>(juce::jlimit(0, 2, index))]))
                parameter->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
        },
        [](int index) { return std::string("sidechain-") + static_cast<char>('A' + juce::jlimit(0, 2, index)); }
    };
}

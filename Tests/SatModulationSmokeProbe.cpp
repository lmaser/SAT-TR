#include "../Source/PluginProcessor.h"
#include "../Source/Modulation/SatModulationConfig.h"
#include "../Source/UIV2/SatBackendBindings.h"
#include "../Source/UIV2/SatUiDefinition.h"
#include "../../TR-Shared/Modulation/Tests/TRNativeSidechainBaseline.h"
#include "../../TR-Shared/Modulation/Tests/TRModulationJourneyAssertions.h"
#include "../../TR-Shared/Modulation/Tests/TRJitterMotionEvidence.h"
#include "../../TR-Shared/Modulation/Tests/TRMotionRecipeUiAssertions.h"
#include "../../TR-Shared/SimpleUIV2/Preset/TRPresetManager.h"
#include "../../TR-Shared/Testing/TRPluginCpuBenchmark.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

struct SatNativeSidechainTestAccess
{
	static void useNative(SATTRAudioProcessor& processor, bool enabled)
	{
		processor.useNativeSidechainForTests_ = enabled;
	}
    static void extract(const SATTRAudioProcessor& processor, float* values)
    {
        for (int loader = 0; loader < 3; ++loader)
        {
            values[loader] = processor.sidechainEnv_[loader];
            values[loader + 3] = processor.sidechainDriveAmount_[loader];
        }
    }

    static void extractShared(const SATTRAudioProcessor& processor, int sample, float* values)
	{
		for (int loader = 0; loader < 3; ++loader)
		{
			const auto view = processor.sharedSidechainControls_[(size_t) loader];
			const auto value = view.valid() && sample < view.sampleCount ? view.samples[sample] : 0.0f;
			values[loader] = value;
			values[loader + 3] = value;
		}
    }

    static std::array<float, 3> nativeInstability(const SATTRAudioProcessor& processor,
                                                   int loader) noexcept
    {
        const SATTRAudioProcessor::LoaderState* states[] {
            &processor.stateA, &processor.stateB, &processor.stateC };
        const auto& state = states[juce::jlimit(0, 2, loader)]->satState;
        const auto amount = state.sInstability;
        const auto t = juce::jlimit(0.0f, 1.0f, (amount - 0.15f) / 0.85f);
        const auto dynamicWeight = 0.30f + 0.12f * t * t * (3.0f - 2.0f * t);
        const auto staticWeight = 1.0f - dynamicWeight;
        const auto combined = [&](const SatEngine::DriftOsc& drift)
        {
            return (drift.staticTol * staticWeight + drift.dynamic * dynamicWeight) * amount;
        };
        return { combined(state.instability.gainDrift) * 0.08f * (1.0f + amount),
                 combined(state.instability.inputDrift),
                 combined(state.instability.shapeDrift) * 0.02f * (1.0f + amount) };
    }

    static std::array<float, 3> matrixInstability(const SATTRAudioProcessor& processor,
                                                   int loader, int sample) noexcept
    {
        const auto base = TR::SatModulation::loaderAInstabilityGain + loader * 3;
        return { processor.modulation.effectiveNativeAtSample(base, sample, 0.0f),
                 processor.modulation.effectiveNativeAtSample(base + 1, sample, 0.0f),
                 processor.modulation.effectiveNativeAtSample(base + 2, sample, 0.0f) };
    }
};

namespace
{
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

juce::Component* findById(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findById(*child, id)) return found;
    return nullptr;
}

void process(SATTRAudioProcessor& processor, bool noteOn)
{
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> audio(2, blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto value = 0.05f * std::sin(0.01f * static_cast<float>(sample));
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, value);
    }
    juce::MidiBuffer midi;
    if (noteOn) midi.addEvent(juce::MidiMessage::noteOn(1, 127, static_cast<juce::uint8>(127)), 16);
    processor.processBlock(audio, midi);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            require(std::isfinite(audio.getSample(channel, sample)), "SAT produced non-finite audio");
}

void verifySidechainSource(SATTRAudioProcessor& processor)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    require(processor.isBusesLayoutSupported(layout) && processor.setBusesLayout(layout),
            "SAT stereo sidechain layout was rejected");
    processor.prepareToPlay(48000.0, 512);

    auto state = TR::Modulation::makeDefaultState();
    constexpr std::array<const char*, 3> preDriveDestinations {
        "sidechain:a:pre-drive", "sidechain:b:pre-drive", "sidechain:c:pre-drive"
    };
    for (int source = 1; source <= 3; ++source)
    {
        auto& profile = state.analysisSources[source];
        profile.feature = TR::Modulation::AnalysisFeature::hybridEnvelope;
        profile.detector.smooth = 0.0f;
        profile.detector.timingMode = TR::Modulation::EnvelopeTimingMode::independent;
        profile.detector.attackMs = 0.0f;
        profile.detector.releaseMs = 0.0f;
        profile.detector.hybrid.rmsWeight = 4.0f;
        profile.detector.hybrid.peakWeight = 0.75f;
        require(TR::Modulation::appendRoute(state, { 0, 0, true,
            source == 1 ? TR::Modulation::SourceId::sidechainEnvelope()
                        : TR::Modulation::SourceId::sidechainAnalysis(source),
            TR::Modulation::Polarity::unipolar, 1.0f,
            preDriveDestinations[static_cast<std::size_t>(source - 1)],
            TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
            TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "SAT shared Sidechain source route rejected");
    }
    require(processor.setModulationState(state), "SAT sidechain modulation state rejected");

    juce::AudioBuffer<float> audio(4, 512);
    audio.clear();
    for (int channel = 2; channel < 4; ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            audio.setSample(channel, sample, 0.75f);
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block)
        processor.processBlock(audio, midi);
    for (int source = 1; source <= 3; ++source)
    {
        const auto id = preDriveDestinations[static_cast<std::size_t>(source - 1)];
        float base = 0.0f, effective = 0.0f;
        require(processor.modulationDestinationValues(id, base, effective)
                    && effective > base + 0.1f,
                "SAT shared Sidechain source did not reach its destination");
        const auto telemetry = processor.modulationTelemetry();
        require(telemetry.sources[source].available
                    && telemetry.sources[source].signalState
                        == TR::Modulation::Runtime::SourceSignalState::active,
                "SAT shared Sidechain source did not become active");
    }
    float mapped[6] {};
    SatNativeSidechainTestAccess::extract(processor, mapped);
    require(mapped[3] > 0.05f && mapped[4] > 0.05f && mapped[5] > 0.05f,
            "SAT MATRIX-only Sidechain did not reach all three +24 dB pre-drive laws");
    for (const auto* id : { SATTRAudioProcessor::kParamSidechainA,
                            SATTRAudioProcessor::kParamSidechainB,
                            SATTRAudioProcessor::kParamSidechainC })
        require(processor.getValueTreeState().getRawParameterValue(id)->load() < 0.5f,
                "SAT MATRIX Sidechain proof unexpectedly enabled a legacy toggle");
    require(TR::Testing::writePluginCpuComparison (std::cout, "SAT", processor),
            "SAT CPU comparison could not restore modulation state");
}

void setNativeParameter(juce::AudioProcessorValueTreeState& state,
                        const char* id, float nativeValue)
{
    auto* parameter = state.getParameter(id);
    require(parameter != nullptr, "required SAT parameter is missing");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(nativeValue));
}

juce::MemoryBlock makeInstabilitySeedState()
{
    auto donor = std::make_unique<SATTRAudioProcessor>();
    juce::MemoryBlock state;
    donor->getStateInformation(state);
    return state;
}

std::vector<float> renderInstabilityAudio(const juce::MemoryBlock& seedState,
                                          bool matrix, double sampleRate,
                                          int blockSize, int model,
                                          int oversampling, bool automate,
                                          std::vector<float>* controls = nullptr)
{
    auto processor = std::make_unique<SATTRAudioProcessor>();
    processor->setStateInformation(seedState.getData(), static_cast<int>(seedState.getSize()));
    auto& parameters = processor->getValueTreeState();
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableB, 0.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableC, 0.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamMixA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamMix, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSatTypeA, static_cast<float>(model));
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSatDriveA, 0.62f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSatCharA, 0.57f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSatTypeCtrlA, 0.73f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSatBiasA, 0.18f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSeriesA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample,
                       static_cast<float>(oversampling));
    const auto initialAmount = automate ? 0.2f : 0.72f;
    if (matrix)
    {
        setNativeParameter(parameters, SATTRAudioProcessor::kParamInstabilityA, 0.0f);
        setNativeParameter(parameters, "mod_macro_1", initialAmount);
        const auto recipe = TR::SatModulation::makeInstabilityParityRecipe(
            TR::Modulation::makeDefaultState(), processor->instabilitySeeds(), 1, true);
        require(processor->setModulationState(recipe), "SAT Instability recipe rejected");
    }
    else
        setNativeParameter(parameters, SATTRAudioProcessor::kParamInstabilityA, initialAmount);
    processor->prepareToPlay(sampleRate, blockSize);

    const auto totalSamples = static_cast<int>(sampleRate * (automate ? 4.0 : 2.0));
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(totalSamples));
    juce::MidiBuffer midi;
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        const auto count = juce::jmin(blockSize, totalSamples - offset);
        constexpr std::array<float, 4> amounts { 0.2f, 0.78f, 1.0f, 0.42f };
        const auto segment = static_cast<std::size_t>((offset
            / juce::jmax(1, static_cast<int>(sampleRate * 0.5))) & 3);
        const auto amount = automate ? amounts[segment] : initialAmount;
        setNativeParameter(parameters, matrix ? "mod_macro_1"
                                               : SATTRAudioProcessor::kParamInstabilityA,
                           amount);
        juce::AudioBuffer<float> audio(2, count);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto position = static_cast<float>(offset + sample);
            const auto saw = 2.0f * std::fmod(position * 110.0f
                / static_cast<float>(sampleRate), 1.0f) - 1.0f;
            const auto value = 0.16f * saw
                + 0.07f * std::sin(2.0f * juce::MathConstants<float>::pi
                    * 659.255f * position / static_cast<float>(sampleRate));
            audio.setSample(0, sample, value);
            audio.setSample(1, sample, value * 0.83f);
        }
        processor->processBlock(audio, midi);
        if (controls != nullptr)
        {
            const auto values = matrix
                ? SatNativeSidechainTestAccess::matrixInstability(*processor, 0, count - 1)
                : SatNativeSidechainTestAccess::nativeInstability(*processor, 0);
            controls->insert(controls->end(), values.begin(), values.end());
        }
        const auto* channel = audio.getReadPointer(0);
        result.insert(result.end(), channel, channel + count);
    }
    return result;
}

bool writeInstabilityMatrix(const juce::File& output, bool automate)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "sample_rate_hz,block_size,model,oversampling,rms_ratio,correlation,rms_error,control_rms_error,max_window_rms_ratio_error,passed\n";
    bool passed = true;
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto blockSize : { 64, 257, 2048 })
        {
            const auto seedState = makeInstabilitySeedState();
            std::vector<float> nativeControls, matrixControls;
            const auto native = renderInstabilityAudio(seedState, false, sampleRate,
                blockSize, static_cast<int>(SatEngine::Model::Tube), 0, automate, &nativeControls);
            const auto matrix = renderInstabilityAudio(seedState, true, sampleRate,
                blockSize, static_cast<int>(SatEngine::Model::Tube), 0, automate, &matrixControls);
            const auto skip = static_cast<std::size_t>(sampleRate * 0.25);
            const auto ratio = TR::Modulation::Tests::rmsRatio(native, matrix, skip);
            const auto correlation = TR::Modulation::Tests::correlation(native, matrix, skip);
            const auto error = TR::Modulation::Tests::rmsDifference(native, matrix, skip);
            const auto windowError = TR::Modulation::Tests::maximumWindowedRmsRatioError(
                native, matrix, skip, static_cast<std::size_t>(sampleRate * 0.25));
            const auto controlSkip = static_cast<std::size_t>(juce::jmax(3,
                static_cast<int>(sampleRate * 0.25 / blockSize) * 3));
            const auto controlError = TR::Modulation::Tests::rmsDifference(
                nativeControls, matrixControls, controlSkip);
            const auto rowPassed = controlError <= (automate ? 0.01 : 1.0e-6)
                && ratio >= (automate ? 0.97 : 0.999)
                && ratio <= (automate ? 1.03 : 1.001)
                && (automate ? correlation >= 0.98 && windowError <= 0.15
                             : correlation >= 0.999 && error <= 0.001);
            passed = passed && rowPassed;
            csv << sampleRate << ',' << blockSize << ',' << static_cast<int>(SatEngine::Model::Tube)
                << ",0," << ratio << ',' << correlation << ',' << error << ','
                << controlError << ',' << windowError << ',' << rowPassed << '\n';
        }
    return csv.good() && passed;
}

bool writeInstabilityModelMatrix(const juce::File& output)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "model,oversampling,rms_ratio,correlation,rms_error,dc_native,dc_matrix,finite,passed\n";
    bool passed = true;
    constexpr std::array<int, 7> models {
        static_cast<int>(SatEngine::Model::Tape),
        static_cast<int>(SatEngine::Model::Tube),
        static_cast<int>(SatEngine::Model::Transistor),
        static_cast<int>(SatEngine::Model::Diode),
        static_cast<int>(SatEngine::Model::OverdriveA),
        static_cast<int>(SatEngine::Model::Clipper),
        static_cast<int>(SatEngine::Model::OverdriveB)
    };
    for (const auto model : models)
        for (const auto oversampling : { 0, 2, 4 })
        {
            const auto seedState = makeInstabilitySeedState();
            const auto native = renderInstabilityAudio(seedState, false, 48000.0, 257,
                                                        model, oversampling, false);
            const auto matrix = renderInstabilityAudio(seedState, true, 48000.0, 257,
                                                        model, oversampling, false);
            const auto skip = static_cast<std::size_t>(12000);
            const auto ratio = TR::Modulation::Tests::rmsRatio(native, matrix, skip);
            const auto correlation = TR::Modulation::Tests::correlation(native, matrix, skip);
            const auto error = TR::Modulation::Tests::rmsDifference(native, matrix, skip);
            const auto mean = [skip](const std::vector<float>& values)
            {
                double sum = 0.0;
                for (auto i = skip; i < values.size(); ++i) sum += values[i];
                return static_cast<float>(sum / static_cast<double>(values.size() - skip));
            };
            const auto dcNative = mean(native);
            const auto dcMatrix = mean(matrix);
            const auto finite = std::isfinite(ratio) && std::isfinite(correlation)
                && std::isfinite(error) && std::isfinite(dcNative) && std::isfinite(dcMatrix);
            const auto rowPassed = finite && ratio >= 0.97 && ratio <= 1.03
                && correlation >= 0.995 && error <= 0.003
                && std::abs(dcNative - dcMatrix) <= 5.0e-4f;
            passed = passed && rowPassed;
            csv << model << ',' << oversampling << ',' << ratio << ',' << correlation
                << ',' << error << ',' << dcNative << ',' << dcMatrix << ','
                << finite << ',' << rowPassed << '\n';
        }
    return csv.good() && passed;
}

bool writeInstabilityPresetEvidence(const juce::File& output)
{
    require(output.createDirectory(), "SAT Instability evidence directory unavailable");
    auto processor = std::make_unique<SATTRAudioProcessor>();
    const auto seeds = processor->instabilitySeeds();
    const auto state = TR::SatModulation::makeInstabilityParityRecipe(
        TR::Modulation::makeDefaultState(), seeds);
    setNativeParameter(processor->getValueTreeState(), SATTRAudioProcessor::kParamInstabilityA, 0.0f);
    setNativeParameter(processor->getValueTreeState(), SATTRAudioProcessor::kParamInstabilityB, 0.0f);
    setNativeParameter(processor->getValueTreeState(), SATTRAudioProcessor::kParamInstabilityC, 0.0f);
    setNativeParameter(processor->getValueTreeState(), "mod_macro_1", 1.0f);
    require(processor->setModulationState(state), "SAT Instability preset state rejected");
    const auto staging = output.getChildFile("preset-staging");
    SATTR::UIV2::SatBackendBindings backend(*processor);
    const auto& definition = SATTR::UIV2::definition();
    TR::SimpleUIV2::TRPresetManager manager(definition.product, definition.preset,
                                             backend, staging);
    constexpr const char* name = "SAT Instability MATRIX 100";
    require(manager.saveAs(name, true).wasOk(), "SAT Instability preset save failed");
    const auto saved = manager.libraryFolder().getChildFile(juce::String(name) + ".trpreset");
    const auto evidence = output.getChildFile(saved.getFileName());
    require(saved.existsAsFile() && saved.copyFileTo(evidence),
            "SAT Instability preset copy failed");
    auto restored = std::make_unique<SATTRAudioProcessor>();
    SATTR::UIV2::SatBackendBindings restoredBackend(*restored);
    TR::SimpleUIV2::TRPresetManager restoredManager(definition.product,
        definition.preset, restoredBackend, staging);
    require(restoredManager.load(name).wasOk() && restored->modulationState() == state
                && restored->instabilitySeeds() == seeds,
            "SAT Instability preset state/seed round-trip failed");
    std::ofstream proof(output.getChildFile("preset-verification.csv")
                            .getFullPathName().toStdString(), std::ios::trunc);
    proof << "preset,native_instability,macro_1,route_count,seed_round_trip,round_trip\n"
          << name << ",0,1," << state.routes.size() << ",1,1\n";
    return proof.good();
}

bool writeInstabilityCpuEvidence(const juce::File& output)
{
    const auto seedState = makeInstabilitySeedState();
    const auto makeState = [&](bool matrix)
    {
        auto processor = std::make_unique<SATTRAudioProcessor>();
        processor->setStateInformation(seedState.getData(), static_cast<int>(seedState.getSize()));
        auto& parameters = processor->getValueTreeState();
        setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableA, 1.0f);
        setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableB, 0.0f);
        setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableC, 0.0f);
        setNativeParameter(parameters, SATTRAudioProcessor::kParamSatTypeA,
                           static_cast<float>(SatEngine::Model::Tube));
        setNativeParameter(parameters, SATTRAudioProcessor::kParamSatDriveA, 0.62f);
        setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample, 2.0f);
        setNativeParameter(parameters, SATTRAudioProcessor::kParamInstabilityA, matrix ? 0.0f : 1.0f);
        if (matrix)
        {
            setNativeParameter(parameters, "mod_macro_1", 1.0f);
            const auto recipe = TR::SatModulation::makeInstabilityParityRecipe(
                TR::Modulation::makeDefaultState(), processor->instabilitySeeds());
            require(processor->setModulationState(recipe), "SAT Instability CPU recipe rejected");
        }
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        return state;
    };
    const auto nativeState = makeState(false);
    const auto matrixState = makeState(true);
    std::ofstream report(output.getFullPathName().toStdString(), std::ios::trunc);
    report << "sample_rate_hz,block_size,native_rt_percent,matrix_rt_percent,overhead_rt_percent,native_pdc,matrix_pdc,hash_equal,passed\n";
    bool passed = true;
    for (const auto sampleRate : { 48000.0, 96000.0, 192000.0 })
        for (const auto blockSize : { 64, 257, 2048 })
        {
            auto benchmark = [&](const juce::MemoryBlock& state, std::uint64_t& hash, int& pdc,
                                 bool& repeatable)
            {
                std::array<double, 3> measurements {};
                std::array<std::uint64_t, 3> hashes {};
                std::array<int, 3> latencies {};
                for (std::size_t repeat = 0; repeat < measurements.size(); ++repeat)
                {
                    auto processor = std::make_unique<SATTRAudioProcessor>();
                    processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                    const auto blocks = juce::jmax(64, static_cast<int>(std::ceil(
                        2.0 * sampleRate / static_cast<double>(blockSize))));
                    measurements[repeat] = TR::Testing::benchmarkPluginRealtimePercent(
                        *processor, sampleRate, blockSize, blocks, &hashes[repeat]);
                    latencies[repeat] = processor->getLatencySamples();
                }
                repeatable = hashes[0] == hashes[1] && hashes[1] == hashes[2]
                    && latencies[0] == latencies[1] && latencies[1] == latencies[2];
                hash = hashes[0];
                pdc = latencies[0];
                std::sort(measurements.begin(), measurements.end());
                return measurements[1];
            };
            std::uint64_t nativeHash = 0, matrixHash = 0;
            int nativePdc = -1, matrixPdc = -1;
            bool nativeRepeatable = false, matrixRepeatable = false;
            const auto nativeCpu = benchmark(nativeState, nativeHash, nativePdc, nativeRepeatable);
            const auto matrixCpu = benchmark(matrixState, matrixHash, matrixPdc, matrixRepeatable);
            const auto rowPassed = std::isfinite(nativeCpu) && std::isfinite(matrixCpu)
                && nativeRepeatable && matrixRepeatable
                && nativePdc == matrixPdc && nativeHash == matrixHash;
            passed = passed && rowPassed;
            report << sampleRate << ',' << blockSize << ',' << nativeCpu << ',' << matrixCpu
                   << ',' << (matrixCpu - nativeCpu) << ',' << nativePdc << ',' << matrixPdc
                   << ',' << (nativeHash == matrixHash) << ',' << rowPassed << '\n';
        }
    return report.good() && passed;
}

void verifyWaveShapePluginIntegration()
{
    auto processor = std::make_unique<SATTRAudioProcessor>();
    SATTR::UIV2::SatBackendBindings backend(*processor);
    auto& parameters = backend.parameters();
    for (const auto* id : { SATTRAudioProcessor::kParamWaveShapeMorphA,
                            SATTRAudioProcessor::kParamWaveShapeMorphB,
                            SATTRAudioProcessor::kParamWaveShapeMorphC,
                            SATTRAudioProcessor::kParamWaveShapeBiasA,
                            SATTRAudioProcessor::kParamWaveShapeBiasB,
                            SATTRAudioProcessor::kParamWaveShapeBiasC })
        require(parameters.getParameter(id) != nullptr && parameters.getParameter(id)->isAutomatable(),
                "WaveShape Morph/Bias host parameter contract is incomplete");

    auto identityState = SATTR::WaveShape::makeDefaultState();
    identityState.loaders[0].enabled = true;
    require(processor->setWaveShapeState(identityState),
            "processor rejected identity WaveShape state");
    processor->prepareToPlay(48000.0, 257);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableB, 0.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamEnableC, 0.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamMixA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamMix, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSeriesA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample, 0.0f);
    juce::MidiBuffer latencyMidi;
    juce::AudioBuffer<float> latencyAudio(2, 257);
    latencyAudio.clear();
    processor->processBlock(latencyAudio, latencyMidi);
    processor->processBlock(latencyAudio, latencyMidi);
    latencyAudio.clear();
    latencyAudio.setSample(0, 0, 0.5f);
    latencyAudio.setSample(1, 0, 0.5f);
    processor->processBlock(latencyAudio, latencyMidi);
    require(processor->getLatencySamples() == 1,
            "x1 SERIES x1 WaveShape PDC is not one host sample");
    int peakIndex = 0;
    for (int sample = 1; sample < 8; ++sample)
        if (std::abs(latencyAudio.getSample(0, sample))
            > std::abs(latencyAudio.getSample(0, peakIndex)))
            peakIndex = sample;
    require(peakIndex == 1,
            "physical WaveShape impulse delay disagrees with reported PDC");

    auto waveState = SATTR::WaveShape::makeDefaultState();
    TR::Curves::Curve soft;
    constexpr int sourcePoints = 33;
    for (int point = 0; point < sourcePoints; ++point)
    {
        const auto x = static_cast<float>(point) / static_cast<float>(sourcePoints - 1);
        soft.points.push_back({ x, std::tanh(3.0f * x) / std::tanh(3.0f), 0.0f });
    }
    for (auto& loader : waveState.loaders)
    {
        loader.enabled = true;
        loader.slots[0].unipolar = soft;
        loader.slots[0].bipolar = TR::Curves::mirrorXY(soft);
        loader.slots[1] = loader.slots[0];
    }
    require(processor->setWaveShapeState(waveState), "processor rejected valid WaveShape state");

    for (const auto* id : { SATTRAudioProcessor::kParamEnableA,
                            SATTRAudioProcessor::kParamEnableB,
                            SATTRAudioProcessor::kParamEnableC })
        setNativeParameter(parameters, id, 1.0f);
    for (const auto* id : { SATTRAudioProcessor::kParamMixA,
                            SATTRAudioProcessor::kParamMixB,
                            SATTRAudioProcessor::kParamMixC,
                            SATTRAudioProcessor::kParamMix })
        setNativeParameter(parameters, id, 1.0f);
    for (const auto* id : { SATTRAudioProcessor::kParamInA,
                            SATTRAudioProcessor::kParamInB,
                            SATTRAudioProcessor::kParamInC,
                            SATTRAudioProcessor::kParamInput })
        setNativeParameter(parameters, id, 24.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamHpOnA, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamLpOnC, 1.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSeriesA, 4.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSeriesB, 2.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamSeriesC, 1.0f);

    juce::MidiBuffer midi;
    for (int route = 0; route <= 5; ++route)
        for (int osOrder = 0; osOrder <= 4; ++osOrder)
        {
            setNativeParameter(parameters, SATTRAudioProcessor::kParamRoute, static_cast<float>(route));
            setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample, static_cast<float>(osOrder));
            juce::AudioBuffer<float> audio(2, 257);
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            {
                const auto value = 0.02f * std::sin(0.071f * static_cast<float>(sample));
                audio.setSample(0, sample, value);
                audio.setSample(1, sample, -0.7f * value);
            }
            processor->processBlock(audio, midi);
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                    require(std::isfinite(audio.getSample(channel, sample)),
                            "WaveShape routing/OS matrix produced non-finite audio");
            require(processor->getLatencySamples() >= 1,
                    "WaveShape ADAA latency was not reported to the host");
        }

    const auto status = backend.scopeStatuses();
    require(status[0].primary == "WAVE SHAPE" && status[1].primary == "WAVE SHAPE"
                && status[2].primary == "WAVE SHAPE",
            "backend does not expose the effective WaveShape model per loader");
    const auto signature = backend.signatureSnapshot(TR::LoaderUIV2::ScopeId::loaderA);
    require(signature.primary.find("WAVE SHAPE") != std::string::npos
                && signature.primaryTraceSize >= 64,
            "backend WaveShape transfer signature is missing");
    const auto internalPreset = backend.readMusicalState();
    require(internalPreset.textValues.count("sat_waveshape_state") == 1,
            "internal preset omitted WaveShape curve state");
    require(processor->setWaveShapeEnabled(0, false),
            "could not alter WaveShape state before preset restore");
    backend.writeMusicalState(internalPreset);
    require(processor->waveShapeState() == waveState,
            "internal preset did not restore WaveShape A/B curves and polarity");

    auto modulationState = TR::Modulation::makeDefaultState();
    require(TR::Modulation::appendRoute(modulationState, { 0, 0, true,
        TR::Modulation::SourceId::midi(TR::Modulation::MidiSourceType::note),
        TR::Modulation::Polarity::unipolar, 1.0f, "waveshape:a:morph",
        TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
        TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
        "WaveShape Morph destination rejected a MACROS route");
    require(processor->setModulationState(modulationState),
            "WaveShape modulation state was rejected");
    juce::AudioBuffer<float> audio(2, 257);
    audio.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 127, static_cast<juce::uint8>(127)), 0);
    processor->processBlock(audio, midi);
    float base = 0.0f;
    float effective = 0.0f;
    require(processor->modulationDestinationValues("waveshape:a:morph", base, effective)
                && effective > base + 0.1f,
            "MACROS did not reach WaveShape Morph A");
    setNativeParameter(parameters, SATTRAudioProcessor::kParamRoute, 0.0f);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample, 0.0f);
    const auto waveX1 = TR::Testing::benchmarkPluginRealtimePercent(*processor, 48000.0, 64);
    setNativeParameter(parameters, SATTRAudioProcessor::kParamOversample, 4.0f);
    const auto waveX16 = TR::Testing::benchmarkPluginRealtimePercent(*processor, 48000.0, 64);
    require(std::isfinite(waveX1) && std::isfinite(waveX16),
            "WaveShape plugin CPU benchmark was invalid");
    std::cout << "SAT waveshape_3loader_series_rt_percent_48k_b64_x1=" << waveX1
              << " x16=" << waveX16 << '\n';
}
}

int main(int argc, char** argv)
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-host-matrix")
            return writeInstabilityMatrix(juce::File(argv[2]), false) ? 0 : 3;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-automation")
            return writeInstabilityMatrix(juce::File(argv[2]), true) ? 0 : 4;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-instability-model-matrix")
            return writeInstabilityModelMatrix(juce::File(argv[2])) ? 0 : 5;
        if (argc == 3 && juce::String(argv[1]) == "--export-jitter-motion-evidence")
            return writeInstabilityPresetEvidence(juce::File(argv[2])) ? 0 : 2;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-cpu")
            return writeInstabilityCpuEvidence(juce::File(argv[2])) ? 0 : 6;
        if (argc == 3 && juce::String(argv[1]) == "--export-native-sidechain-baseline")
        {
            const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<SATTRAudioProcessor>(
                juce::File(argv[2]), "SAT-TR", "env_a,env_b,env_c,drive_a,drive_b,drive_c", 6,
                [](auto& processor) -> auto& { return processor.getValueTreeState(); },
                [](auto& processor, auto& state)
                {
					SatNativeSidechainTestAccess::useNative(processor, true);
                    using namespace TR::Modulation::Tests;
                    return setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableA, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableB, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableC, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeA, 0.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeB, 0.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeC, 0.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainA, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainB, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainC, 1.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainA, -6.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainB, 0.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainC, 6.0f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothA, 0.1f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothB, 0.5f)
                        && setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothC, 0.9f);
                },
                [](const auto& processor, int, int, float* values)
                { SatNativeSidechainTestAccess::extract(processor, values); });
            return ok ? 0 : 2;
        }
		if (argc == 3 && (juce::String(argv[1]) == "--export-native-sidechain-audio"
		               || juce::String(argv[1]) == "--export-shared-sidechain-audio"))
		{
			const bool native = juce::String(argv[1]).contains("native");
			const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<SATTRAudioProcessor>(
				juce::File(argv[2]), native ? "SAT-TR-audio-native" : "SAT-TR-audio-shared",
				"env,drive", 2,
				[](auto& processor) -> auto& { return processor.getValueTreeState(); },
				[native](auto& processor, auto& state)
				{
					using namespace TR::Modulation::Tests;
					SatNativeSidechainTestAccess::useNative(processor, native);
					return setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableA, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableB, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableC, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeA, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainA, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainA, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothA, 0.5f);
				},
				[native](const auto& processor, int sample, int, float* values)
				{
					if (native)
					{
						float all[6] {};
						SatNativeSidechainTestAccess::extract(processor, all);
						values[0] = all[0]; values[1] = all[3];
					}
					else
					{
						float all[6] {};
						SatNativeSidechainTestAccess::extractShared(processor, sample, all);
						values[0] = all[0]; values[1] = all[3];
					}
				});
			return ok ? 0 : 2;
		}
		if (argc == 3 && juce::String(argv[1]) == "--export-shared-sidechain-baseline")
		{
			const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<SATTRAudioProcessor>(
				juce::File(argv[2]), "SAT-TR", "env_a,env_b,env_c,drive_a,drive_b,drive_c", 6,
				[](auto& processor) -> auto& { return processor.getValueTreeState(); },
				[](auto&, auto& state)
				{
					using namespace TR::Modulation::Tests;
					return setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableA, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableB, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamEnableC, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeA, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeB, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSatTypeC, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainA, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainB, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainC, 1.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainA, -6.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainB, 0.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainGainC, 6.0f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothA, 0.1f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothB, 0.5f)
						&& setNativeBaselineParameter(state, SATTRAudioProcessor::kParamSidechainSmoothC, 0.9f);
				},
				[](const auto& processor, int sample, int, float* values)
				{ SatNativeSidechainTestAccess::extractShared(processor, sample, values); });
			return ok ? 0 : 2;
		}
        {
            auto auditProcessor = std::make_unique<SATTRAudioProcessor>();
            SATTR::UIV2::SatBackendBindings auditBackend(*auditProcessor);
            require(TR::Modulation::Tests::auditMotionRecipeBackend(
                        auditBackend, auditProcessor->getValueTreeState(),
                        SATTRAudioProcessor::kParamInstabilityA, "instability-a", 3, 1, 4).passed(),
                    "SAT Instability recipe UI/backend contract failed");
        }
        auto processor = std::make_unique<SATTRAudioProcessor>();
		verifyWaveShapePluginIntegration();
        require(processor->acceptsMidi(), "SAT does not advertise MIDI input");
        processor->prepareToPlay(48000.0, 512);

        auto state = TR::Modulation::makeDefaultState();
        state.midiSources[static_cast<std::size_t>(TR::Modulation::MidiSourceType::note)].smoothingSeconds = 0.0f;
        require(TR::Modulation::appendRoute(state, { 0, 0, true,
            TR::Modulation::SourceId::midi(TR::Modulation::MidiSourceType::note),
            TR::Modulation::Polarity::unipolar, 1.0f, "macro:1", TR::Modulation::SourceId::none(),
            TR::Modulation::Polarity::unipolar, TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "SAT MIDI to Macro route rejected");
        require(TR::Modulation::appendRoute(state, { 0, 0, true, TR::Modulation::SourceId::macro(1),
            TR::Modulation::Polarity::unipolar, 1.0f, "loader:a:drive", TR::Modulation::SourceId::none(),
            TR::Modulation::Polarity::unipolar, TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "SAT Macro to Drive route rejected");
        require(processor->setModulationState(state), "SAT modulation state rejected");

        SATTR::UIV2::SatBackendBindings backend(*processor);
        const auto presetState = backend.readMusicalState();
        require(backend.validateMusicalState(presetState)
                    && presetState.textValues.count(TR::Modulation::Integration::presetStateId) == 1,
                "SAT internal preset omitted modulation XML");
        require(backend.parameterSnapshot().count("mod_macro_1") == 1,
                "SAT internal preset omitted Macro parameters");
        require(presetState.textValues.count("sat_waveshape_state") == 1,
                "SAT internal preset omitted WaveShape state");
        require(!backend.parameters().getParameter("route")->isAutomatable()
                    && backend.parameters().getParameter("mod_macro_1")->isAutomatable(),
                "SAT Route/Macro automation contract changed");
        auto legacyPresetState = presetState;
        legacyPresetState.textValues.erase(TR::Modulation::Integration::presetStateId);
        legacyPresetState.values[TR::Modulation::Integration::presetStateId] = 0.0;
        require(backend.validateMusicalState(legacyPresetState),
                "SAT legacy preset marker was rejected");
        backend.writeMusicalState(legacyPresetState);
        require(processor->modulationState().routes.empty(),
                "SAT legacy preset did not migrate to default modulation state");
        require(processor->setModulationState(state), "SAT could not restore modulation state");

        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
        juce::Timer::callPendingTimersSynchronously();
        auto* macrosButton = dynamic_cast<juce::Button*>(findById(*editor, "macros-panel-button"));
        auto* matrixButton = dynamic_cast<juce::Button*>(findById(*editor, "matrix-workspace-button"));
        auto* workspace = findById(*editor, "auxiliary-workspace");
        require(macrosButton != nullptr && matrixButton != nullptr
                    && workspace != nullptr && !workspace->isVisible(),
                "SAT MACROS/MATRIX controls are missing");
        const auto productSize = juce::Point<int> { editor->getWidth(), editor->getHeight() };
        TR::Modulation::Tests::clickButton(*macrosButton);
        auto* compactPanel = findById(*editor, "macro-panel");
        require(compactPanel != nullptr && compactPanel->isShowing()
                    && !workspace->isVisible()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "First MACROS click did not open the compact Macro panel");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(workspace->isVisible() && matrixButton->getToggleState(),
                "SAT MATRIX workspace did not open");
        require(editor->getWidth() == 1040 && editor->getHeight() == 680,
                "SAT MATRIX workspace did not request the canonical editor size");
        const auto journey = TR::Modulation::Tests::auditMacroJourney(workspace);
        require(journey.workspaceFound && journey.visible && journey.hasAllMacroCards
                    && journey.hasFocusTargets && journey.containerHasNoFocusRing
                    && journey.nameEditingContract,
                "SAT MATRIX journey has complete cards and control-local focus");
        auto* modulationWorkspace = dynamic_cast<TR::Modulation::UI::SimpleModulationWorkspace*>(workspace);
        auto* recipeButton = dynamic_cast<juce::Button*>(findById(*editor, "matrix-motion-recipes"));
        require(modulationWorkspace != nullptr && recipeButton != nullptr && recipeButton->isShowing(),
                "SAT motion recipe selector is not reachable from the real editor");
        auto* instabilityC = processor->getValueTreeState().getParameter(
            SATTRAudioProcessor::kParamInstabilityC);
        require(instabilityC != nullptr, "SAT Instability C parameter is missing");
        instabilityC->setValueNotifyingHost(0.73f);
        modulationWorkspace->focusMacro(2);
        modulationWorkspace->openMotionRecipeSelector();
        modulationWorkspace->setSelectorSearchText("INSTABILITY C");
        require(modulationWorkspace->visibleSelectorChoiceCount() == 1
                    && modulationWorkspace->chooseVisibleSelectorItem(0),
                "SAT real editor could not install the Instability C recipe");
        require(std::abs(processor->getValueTreeState()
                             .getRawParameterValue(SATTRAudioProcessor::kParamInstabilityC)
                             ->load(std::memory_order_relaxed)) <= 1.0e-7f,
                "SAT real editor recipe did not clear native Instability C");
        require(processor->setModulationState(state),
                "SAT could not restore modulation state after the recipe UI audit");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(!workspace->isVisible() && compactPanel->isShowing()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "SAT MATRIX did not restore the originating MACROS panel");

        process(*processor, true);
        for (int block = 0; block < 32; ++block) process(*processor, false);
        const auto workspaceTelemetry = processor->modulationTelemetry();
        require(workspaceTelemetry.destinationCount > 0,
                "SAT MACROS destination telemetry is empty");
        require(!workspaceTelemetry.sources[1].available,
                "SAT disabled sidechain bus should report no live source");
        float base = 0.0f, effective = 0.0f;
        require(processor->modulationDestinationValues("loader:a:drive", base, effective),
                "SAT destination telemetry unavailable");
        require(effective > base + 0.1f, "SAT MIDI Macro route did not reach DSP destination");

        verifySidechainSource(*processor);
        require(processor->modulationTelemetry().sources[1].available,
                "SAT enabled sidechain bus did not become available in MACROS telemetry");
        require(processor->setModulationState(state), "SAT could not restore MIDI modulation state");

        juce::MemoryBlock preset;
        processor->getStateInformation(preset);
        editor.reset();
        auto restored = std::make_unique<SATTRAudioProcessor>();
        restored->setStateInformation(preset.getData(), static_cast<int>(preset.getSize()));
        require(restored->modulationState().routes.size() == 2, "SAT routes did not survive preset round-trip");
        std::cout << "SAT modulation smoke probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SAT modulation smoke probe failed: " << error.what() << '\n';
        return 1;
    }
}

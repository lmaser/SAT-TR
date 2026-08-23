#include "SatModulationConfig.h"

#include "../PluginProcessor.h"
#include "../../../TR-Shared/Modulation/Recipes/TRMotionRecipeUtilities.h"

namespace TR::SatModulation
{
const std::vector<Modulation::Integration::ParameterDestination>& destinations()
{
    using D = Modulation::Integration::ParameterDestination;
    static const std::vector<D> result {
        { "loader:a:drive", "LOADER A", "DRIVE", SATTRAudioProcessor::kParamSatDriveA,
          SATTRAudioProcessor::kSatDriveMin, SATTRAudioProcessor::kSatDriveMax, false, 0.01f },
        { "loader:a:character", "LOADER A", "CHARACTER", SATTRAudioProcessor::kParamSatCharA,
          SATTRAudioProcessor::kSatCharMin, SATTRAudioProcessor::kSatCharMax, false, 0.02f },
        { "loader:a:type", "LOADER A", "TYPE", SATTRAudioProcessor::kParamSatTypeCtrlA,
          SATTRAudioProcessor::kSatTypeCtrlMin, SATTRAudioProcessor::kSatTypeCtrlMax, false, 0.02f },
        { "loader:a:mix", "LOADER A", "MIX", SATTRAudioProcessor::kParamMixA,
          SATTRAudioProcessor::kGlobalMixMin, SATTRAudioProcessor::kGlobalMixMax, false, 0.01f },
        { "loader:b:drive", "LOADER B", "DRIVE", SATTRAudioProcessor::kParamSatDriveB,
          SATTRAudioProcessor::kSatDriveMin, SATTRAudioProcessor::kSatDriveMax, false, 0.01f },
        { "loader:b:character", "LOADER B", "CHARACTER", SATTRAudioProcessor::kParamSatCharB,
          SATTRAudioProcessor::kSatCharMin, SATTRAudioProcessor::kSatCharMax, false, 0.02f },
        { "loader:b:type", "LOADER B", "TYPE", SATTRAudioProcessor::kParamSatTypeCtrlB,
          SATTRAudioProcessor::kSatTypeCtrlMin, SATTRAudioProcessor::kSatTypeCtrlMax, false, 0.02f },
        { "loader:b:mix", "LOADER B", "MIX", SATTRAudioProcessor::kParamMixB,
          SATTRAudioProcessor::kGlobalMixMin, SATTRAudioProcessor::kGlobalMixMax, false, 0.01f },
        { "loader:c:drive", "LOADER C", "DRIVE", SATTRAudioProcessor::kParamSatDriveC,
          SATTRAudioProcessor::kSatDriveMin, SATTRAudioProcessor::kSatDriveMax, false, 0.01f },
        { "loader:c:character", "LOADER C", "CHARACTER", SATTRAudioProcessor::kParamSatCharC,
          SATTRAudioProcessor::kSatCharMin, SATTRAudioProcessor::kSatCharMax, false, 0.02f },
        { "loader:c:type", "LOADER C", "TYPE", SATTRAudioProcessor::kParamSatTypeCtrlC,
          SATTRAudioProcessor::kSatTypeCtrlMin, SATTRAudioProcessor::kSatTypeCtrlMax, false, 0.02f },
        { "loader:c:mix", "LOADER C", "MIX", SATTRAudioProcessor::kParamMixC,
          SATTRAudioProcessor::kGlobalMixMin, SATTRAudioProcessor::kGlobalMixMax, false, 0.01f },
        { "global:mix", "GLOBAL", "MIX", SATTRAudioProcessor::kParamMix,
          SATTRAudioProcessor::kGlobalMixMin, SATTRAudioProcessor::kGlobalMixMax, false, 0.01f },
        { "waveshape:a:morph", "WAVE SHAPE A", "MORPH", SATTRAudioProcessor::kParamWaveShapeMorphA,
          SATTRAudioProcessor::kWaveShapeMorphMin, SATTRAudioProcessor::kWaveShapeMorphMax, false, 0.01f },
        { "waveshape:a:bias", "WAVE SHAPE A", "BIAS", SATTRAudioProcessor::kParamWaveShapeBiasA,
          SATTRAudioProcessor::kWaveShapeBiasMin, SATTRAudioProcessor::kWaveShapeBiasMax, true, 0.01f },
        { "waveshape:b:morph", "WAVE SHAPE B", "MORPH", SATTRAudioProcessor::kParamWaveShapeMorphB,
          SATTRAudioProcessor::kWaveShapeMorphMin, SATTRAudioProcessor::kWaveShapeMorphMax, false, 0.01f },
        { "waveshape:b:bias", "WAVE SHAPE B", "BIAS", SATTRAudioProcessor::kParamWaveShapeBiasB,
          SATTRAudioProcessor::kWaveShapeBiasMin, SATTRAudioProcessor::kWaveShapeBiasMax, true, 0.01f },
        { "waveshape:c:morph", "WAVE SHAPE C", "MORPH", SATTRAudioProcessor::kParamWaveShapeMorphC,
          SATTRAudioProcessor::kWaveShapeMorphMin, SATTRAudioProcessor::kWaveShapeMorphMax, false, 0.01f },
        { "waveshape:c:bias", "WAVE SHAPE C", "BIAS", SATTRAudioProcessor::kParamWaveShapeBiasC,
          SATTRAudioProcessor::kWaveShapeBiasMin, SATTRAudioProcessor::kWaveShapeBiasMax, true, 0.01f },
        { "sidechain:a:pre-drive", "SIDECHAIN A", "PRE DRIVE", "",
          0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::blockControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f },
        { "sidechain:b:pre-drive", "SIDECHAIN B", "PRE DRIVE", "",
          0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::blockControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f },
        { "sidechain:c:pre-drive", "SIDECHAIN C", "PRE DRIVE", "",
          0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::blockControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f },
        { "instability:a:gain", "INSTABILITY A", "GAIN / GM", "", -0.16f, 0.16f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "instability:a:input", "INSTABILITY A", "INPUT LEVEL", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "instability:a:shape", "INSTABILITY A", "SHAPE", "", -0.04f, 0.04f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 2 },
        { "instability:b:gain", "INSTABILITY B", "GAIN / GM", "", -0.16f, 0.16f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "instability:b:input", "INSTABILITY B", "INPUT LEVEL", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "instability:b:shape", "INSTABILITY B", "SHAPE", "", -0.04f, 0.04f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 2 },
        { "instability:c:gain", "INSTABILITY C", "GAIN / GM", "", -0.16f, 0.16f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 0 },
        { "instability:c:input", "INSTABILITY C", "INPUT LEVEL", "", -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 1 },
        { "instability:c:shape", "INSTABILITY C", "SHAPE", "", -0.04f, 0.04f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 2 },
        { "instability:a:amount", "INSTABILITY A", "AMOUNT", "", 0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 3 },
        { "instability:b:amount", "INSTABILITY B", "AMOUNT", "", 0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 3 },
        { "instability:c:amount", "INSTABILITY C", "AMOUNT", "", 0.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f, 3 }
    };
    return result;
}

Modulation::State makeInstabilityParityRecipe(Modulation::State state,
                                              const std::array<std::uint32_t, 3>& seeds,
                                              int macroOneBased,
                                              bool exposeComponentPorts,
                                              std::uint32_t loaderMask)
{
    macroOneBased = juce::jlimit(1, Modulation::macroCount, macroOneBased);
    state.macros[static_cast<std::size_t>(macroOneBased - 1)].name = "INSTABILITY DEPTH";
    constexpr std::array<const char*, 9> destinationIds {
        "instability:a:gain", "instability:a:input", "instability:a:shape",
        "instability:b:gain", "instability:b:input", "instability:b:shape",
        "instability:c:gain", "instability:c:input", "instability:c:shape"
    };
    for (int loader = 0; loader < 3; ++loader)
    {
        if ((loaderMask & (1u << loader)) == 0) continue;
        Modulation::Recipes::removeRoutesTo(state, {
            destinationIds[static_cast<std::size_t>(loader * 3)],
            destinationIds[static_cast<std::size_t>(loader * 3 + 1)],
            destinationIds[static_cast<std::size_t>(loader * 3 + 2)],
            loader == 0 ? "instability:a:amount"
                : (loader == 1 ? "instability:b:amount" : "instability:c:amount") });
        auto& source = state.motionSources[static_cast<std::size_t>(loader)];
        source.algorithm = Modulation::MotionAlgorithm::componentInstability;
        source.seed = seeds[static_cast<std::size_t>(loader)];
        source.initialisation = Modulation::MotionInitialisation::zeroToRandom;
        source.lanePolicy = Modulation::MotionLanePolicy::destination;
        source.rateControl = Modulation::SourceId::macro(macroOneBased);
        source.adaptiveControlSmoothingSeconds = 1.0f
            / (2.0f * juce::MathConstants<float>::pi * 11.0f);
        if (exposeComponentPorts)
        for (int lane = 0; lane < 3; ++lane)
        {
            Modulation::Route route;
            route.source = Modulation::SourceId::motion(loader + 2);
            route.polarity = Modulation::Polarity::bipolar;
            route.amount = 1.0f;
            route.destinationId = destinationIds[static_cast<std::size_t>(loader * 3 + lane)];
            route.curve = Modulation::makeLinearCurve();
            route.auxCurve = Modulation::makeLinearCurve();
            Modulation::appendRoute(state, std::move(route));
        }
        Modulation::Route amountRoute;
        amountRoute.source = exposeComponentPorts
            ? Modulation::SourceId::motion(loader + 2)
            : Modulation::SourceId::macro(macroOneBased);
        amountRoute.polarity = Modulation::Polarity::unipolar;
        amountRoute.amount = 1.0f;
        amountRoute.destinationId = loader == 0 ? "instability:a:amount"
            : (loader == 1 ? "instability:b:amount" : "instability:c:amount");
        amountRoute.curve = Modulation::makeLinearCurve();
        amountRoute.auxCurve = Modulation::makeLinearCurve();
        Modulation::appendRoute(state, std::move(amountRoute));
    }
    return state;
}
}

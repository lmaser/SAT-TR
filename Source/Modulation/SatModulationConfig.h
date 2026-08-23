#pragma once

#include "../../../TR-Shared/Modulation/Integration/TRParameterModulationBridge.h"

#include <array>
#include <cstdint>
#include <vector>

namespace TR::SatModulation
{
enum Destination : int
{
    loaderADrive = 0,
    loaderACharacter,
    loaderAType,
    loaderAMix,
    loaderBDrive,
    loaderBCharacter,
    loaderBType,
    loaderBMix,
    loaderCDrive,
    loaderCCharacter,
    loaderCType,
    loaderCMix,
    globalMix,
    loaderAWaveShapeMorph,
    loaderAWaveShapeBias,
    loaderBWaveShapeMorph,
    loaderBWaveShapeBias,
    loaderCWaveShapeMorph,
    loaderCWaveShapeBias,
    loaderASidechainPreDrive,
    loaderBSidechainPreDrive,
    loaderCSidechainPreDrive,
    loaderAInstabilityGain,
    loaderAInstabilityInput,
    loaderAInstabilityShape,
    loaderBInstabilityGain,
    loaderBInstabilityInput,
    loaderBInstabilityShape,
    loaderCInstabilityGain,
    loaderCInstabilityInput,
    loaderCInstabilityShape,
    loaderAInstabilityAmount,
    loaderBInstabilityAmount,
    loaderCInstabilityAmount,
    destinationCount
};

const std::vector<Modulation::Integration::ParameterDestination>& destinations();
Modulation::State makeInstabilityParityRecipe(Modulation::State state,
                                              const std::array<std::uint32_t, 3>& seeds,
                                              int macroOneBased = 1,
                                              bool exposeComponentPorts = false,
                                              std::uint32_t loaderMask = 0x7u);
}

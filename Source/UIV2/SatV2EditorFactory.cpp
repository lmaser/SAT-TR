#include "SatV2EditorFactory.h"

#include "SatBackendBindings.h"
#include "SatUiDefinition.h"
#include "SatWaveShapeWorkspace.h"
#include "../Modulation/SatModulationConfig.h"
#include "../../../TR-Shared/LoaderUIV2/Runtime/LoaderEditorHost.h"
#include "../../../TR-Shared/Modulation/UI/TRSimpleModulationWorkspace.h"
#include "../PluginProcessor.h"
#include <memory>

namespace SATTR::UIV2
{
juce::AudioProcessorEditor* createEditor(SATTRAudioProcessor& processor)
{
    std::vector<TR::Modulation::UI::DestinationOption> destinations;
    int telemetryIndex = 0;
    for (const auto& descriptor : TR::SatModulation::destinations())
        destinations.push_back({ descriptor.id, descriptor.group, descriptor.label,
                                 true, {}, telemetryIndex++ });
    auto backend = std::make_unique<SatBackendBindings>(processor);
    auto& modulationBackend = *backend;
    auto& waveShapeBackend = *backend;
    auto modulation = std::make_unique<TR::Modulation::UI::SimpleModulationWorkspace>(
        TR::Modulation::UI::workspaceCallbacks(modulationBackend), std::move(destinations),
        modulationBackend.sidechainWorkspaceCallbacks());
    return new TR::LoaderUIV2::LoaderEditorHost(
        processor, definition(), std::move(backend),
        std::move(modulation), std::make_unique<SatWaveShapeWorkspace>(waveShapeBackend));
}
}

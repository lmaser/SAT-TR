#pragma once

#include "../../../TR-Shared/LoaderUIV2/Runtime/LoaderJuceBackend.h"
#include "../../../TR-Shared/Modulation/UI/TRSimpleModulationWorkspace.h"
#include "SatWaveShapeWorkspace.h"
#include "../WaveShape/SatWaveShapeState.h"

#include <array>
#include <memory>

class SATTRAudioProcessor;
namespace SatEngine { struct State; }

namespace SATTR::UIV2
{
class SatBackendBindings final : public TR::LoaderUIV2::LoaderJuceBackend,
                                 public TR::Modulation::UI::ModulationUiBackend,
                                 public SatWaveShapeUiBackend
{
public:
    explicit SatBackendBindings(SATTRAudioProcessor& processor);
    ~SatBackendBindings() override;

    juce::AudioProcessorValueTreeState& parameters() const noexcept override;
    TR::SimpleUIV2::ParameterSnapshot parameterSnapshot() const override;
    void updateParameterSnapshot(TR::SimpleUIV2::ParameterSnapshot&) const override;
    std::optional<juce::String> formatControlValue(std::string_view controlId,
                                                   double value) const override;
    std::optional<double> parseControlValue(std::string_view controlId,
                                            const juce::String& text) const override;
    bool invokeContextualAction(std::string_view actionId) override;
    std::optional<int> contextualChoiceIndex(std::string_view controlId) const override;
    float inputMeterPeak() const noexcept override;
    float outputMeterPeak() const noexcept override;
    TR::SimpleUIV2::MusicalState readMusicalState() const override;
    TR::SimpleUIV2::MusicalState defaultMusicalState() const override;
    bool validateMusicalState(const TR::SimpleUIV2::MusicalState&) const noexcept override;
    void writeMusicalState(const TR::SimpleUIV2::MusicalState&) override;
    TR::LoaderUIV2::LoaderUiInstanceState readLoaderUiInstanceState() const override;
    void writeLoaderUiInstanceState(const TR::LoaderUIV2::LoaderUiInstanceState&) override;
    std::array<TR::LoaderUIV2::ScopeStatus, 4> scopeStatuses() const override;
    TR::LoaderUIV2::LoaderSignatureSnapshot signatureSnapshot(TR::LoaderUIV2::ScopeId) const override;
    bool canAcceptAssetDrop(TR::LoaderUIV2::ScopeId, const juce::StringArray&) const override;
    bool loadDroppedAsset(TR::LoaderUIV2::ScopeId, const juce::StringArray&) override;
    void setMacroName(int index, const juce::String& name) override;
    SATTR::WaveShape::State waveShapeState() const;
    bool setWaveShapeState(const SATTR::WaveShape::State&);
    bool setWaveShapeEnabled(int loaderIndex, bool enabled);
    bool isWaveShapeEnabled(int loaderIndex) const noexcept;
    TR::Modulation::State modulationState() const override;
    std::uint64_t modulationStateGeneration() const noexcept override;
    std::array<float, TR::Modulation::macroCount> modulationMacroValues() const noexcept override;
    void setModulationMacroValue(int macro, float value) override;
    bool setModulationState(const TR::Modulation::State&) override;
    TR::Modulation::UI::SourceCapabilities modulationSourceCapabilities() const noexcept override;
    std::vector<TR::Modulation::UI::MotionRecipeOption> modulationRecipeOptions() const override;
    bool installModulationRecipe(const juce::String&, int) override;
    TR::Modulation::Runtime::TelemetrySnapshot modulationTelemetry() const noexcept override;
    bool refreshModulationDestinationOptions(
        std::vector<TR::Modulation::UI::DestinationOption>&) override;
    TR::Modulation::UI::SidechainWorkspaceCallbacks sidechainWorkspaceCallbacks() override;

private:
    struct TransferCache
    {
        std::array<float, 11> key {};
        std::array<TR::LoaderUIV2::SignaturePoint,
                   TR::LoaderUIV2::loaderSignatureMaxPoints> trace {};
        std::size_t size = 0;
        std::uint64_t revision = 0;
        std::uint64_t structuralRevision = 0;
        bool valid = false;
    };

    void updateTransferCache(int loaderIndex) const;
    void browseNam(int loaderIndex);
    void clearNam(int loaderIndex);
    void triggerAlign();
    int selectedLoaderIndex() const noexcept;

    SATTRAudioProcessor& processor;
    std::array<std::unique_ptr<juce::FileChooser>, 3> namChoosers;
    mutable std::array<TransferCache, 3> transferCaches;
    mutable std::unique_ptr<SatEngine::State> transferPreviewState;
    int sidechainContextIndex = 0;
};
}

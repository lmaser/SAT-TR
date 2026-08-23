#pragma once

#include "../WaveShape/SatWaveShapeState.h"
#include "../../../TR-Shared/SimpleUIV2/Runtime/SimpleEditorHost.h"
#include "../../../TR-Shared/Curves/State/TRCurveEditHistory.h"
#include "../../../TR-Shared/Curves/UI/TRCurveEditorControls.h"

#include <JuceHeader.h>

namespace SATTR::UIV2
{
class SatWaveShapeUiBackend
{
public:
    virtual ~SatWaveShapeUiBackend() = default;
    virtual WaveShape::State waveShapeState() const = 0;
    virtual bool setWaveShapeState(const WaveShape::State&) = 0;
    virtual bool isWaveShapeEnabled(int loaderIndex) const noexcept = 0;
};

class SatWaveShapeWorkspace final : public TR::SimpleUIV2::SimpleAuxiliaryWorkspace
{
public:
    explicit SatWaveShapeWorkspace(SatWaveShapeUiBackend&);
    ~SatWaveShapeWorkspace() override;

    juce::String headerButtonText() const override { return "WAVE SHAPE"; }
    juce::Point<int> preferredEditorSize() const override { return { 1040, 680 }; }
    bool canOpenContext(int loaderIndex) const override;
    bool beginContext(int loaderIndex) override;
    void refreshWorkspace() override;
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    class CurveCanvas;

    TR::Curves::Curve& activeCurve();
    const TR::Curves::Curve& activeCurve() const;
    TR::Curves::Domain activeDomain() const noexcept;
    void replaceActiveCurve(TR::Curves::Curve, bool addHistory = true);
    void pushHistory();
    void restoreHistory(int index);
    void selectSlot(int slot);
    void setBipolar(bool enabled);
    void resetActiveSegment();
    void resetSelectedPoint();
    void deleteSelectedPoint();
    void resetActiveCurve();
    void apply();
    void cancel();
    void updateControls();
    void commitPointEdit(bool horizontal);
    void cancelPointEdit(juce::Label&, TR::SimpleUIV2::SimpleTextEditContract&);
    void commitTensionEdit();
    void cancelTensionEdit();

    SatWaveShapeUiBackend& backend;
    std::unique_ptr<CurveCanvas> canvas;
    WaveShape::State original;
    WaveShape::State draft;
    TR::Curves::EditHistory<WaveShape::State> editHistory;
    int loaderIndex = -1;
    int slotIndex = 0;

    TR::Curves::UI::CurveEditorButton slotA { "A" }, slotB { "B" };
    juce::ToggleButton bipolar { "BIPOLAR" };
    TR::Curves::UI::CurveEditorButton nodeMode { "NODES" }, drawMode { "DRAW" };
    TR::Curves::UI::CurveEditorButton grid { "GRID 16x16" }, snap { "SNAP ON" };
    TR::Curves::UI::CurveEditorButton undo { "UNDO" }, redo { "REDO" };
    TR::Curves::UI::CurveEditorButton resetSegment { "RESET SEGMENT" }, resetPoint { "RESET POINT" };
    TR::Curves::UI::CurveEditorButton deletePoint { "DELETE POINT" }, reset { "RESET CURVE" };
    TR::Curves::UI::CurveEditorButton cancelButton { "CANCEL" }, applyButton { "APPLY" };
    TR::Curves::UI::CurveEditorSlider tension;
    juce::Label selectionLabel;
    TR::Curves::UI::CurveNumericField pointXValue, pointYValue, tensionValue;
    TR::SimpleUIV2::SimpleTextEditContract pointXEdit, pointYEdit, tensionEdit;
    juce::Rectangle<int> titleArea, bodyArea, inspectorArea, statusArea;
};
}

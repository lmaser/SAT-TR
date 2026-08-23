#include "SatWaveShapeWorkspace.h"

#include "../WaveShape/SatWaveShapeCore.h"
#include "../../../TR-Shared/SimpleUIV2/Style/SimpleV2LookAndFeel.h"
#include "../../../TR-Shared/Curves/UI/TRCurveEditorLayout.h"
#include "../../../TR-Shared/Curves/UI/TRCurveViewport.h"

#include <algorithm>
#include <cmath>

namespace SATTR::UIV2
{
namespace S = TR::SimpleUIV2;

class SatWaveShapeWorkspace::CurveCanvas final : public juce::Component,
                                                  public juce::SettableTooltipClient
{
public:
    explicit CurveCanvas(SatWaveShapeWorkspace& value) : owner(value)
    {
        setWantsKeyboardFocus(true);
        setTooltip("Double-click to add or remove a node. Drag nodes to edit. DRAW captures a left-to-right gesture.");
    }

    void setDrawMode(bool enabled) { drawMode = enabled; repaint(); }
    bool isDrawMode() const noexcept { return drawMode; }
    void setSnap(bool enabled) { snap = enabled; repaint(); }
    bool snapEnabled() const noexcept { return snap; }
    void setGridSize(int value) { gridSize = juce::jlimit(8, 32, value); repaint(); }
    int getGridSize() const noexcept { return gridSize; }
    int selectedSegmentIndex() const noexcept { return selectedSegment; }
    int selectedPointIndex() const noexcept { return selectedPoint; }
    int pointCount() const noexcept
    {
        return static_cast<int>(owner.activeCurve().points.size());
    }
    void resetSelection()
    {
        selectedPoint = owner.activeCurve().points.empty() ? -1 : 0;
        selectedSegment = owner.activeCurve().points.size() > 1 ? 0 : -1;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto background = S::simpleColour(S::SimpleColourRole::background, this);
        const auto divider = S::simpleColour(S::SimpleColourRole::divider, this);
        const auto accent = S::simpleColour(S::SimpleColourRole::accent, this);
        const auto focus = S::simpleColour(S::SimpleColourRole::focus, this);
        g.fillAll(background);
        for (int index = 1; index < gridSize; ++index)
        {
            const auto alpha = index % 8 == 0 ? 0.48f : 0.18f;
            g.setColour(divider.withAlpha(alpha));
            const auto x = plot.getX() + plot.getWidth() * index / gridSize;
            const auto y = plot.getY() + plot.getHeight() * index / gridSize;
            g.drawVerticalLine(x, static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
            g.drawHorizontalLine(y, static_cast<float>(plot.getX()), static_cast<float>(plot.getRight()));
        }
        if (owner.activeDomain() == TR::Curves::Domain::bipolar)
        {
            g.setColour(divider.withAlpha(0.80f));
            g.drawVerticalLine(plot.getCentreX(), static_cast<float>(plot.getY()), static_cast<float>(plot.getBottom()));
            g.drawHorizontalLine(plot.getCentreY(), static_cast<float>(plot.getX()), static_cast<float>(plot.getRight()));
        }

        const auto& loader = owner.draft.loaders[static_cast<std::size_t>(owner.loaderIndex)];
        const auto curveFor = [&loader, this](int slot) -> const TR::Curves::Curve&
        {
            const auto& state = loader.slots[static_cast<std::size_t>(slot)];
            return owner.activeDomain() == TR::Curves::Domain::unipolar
                       ? state.unipolar : state.bipolar;
        };
        drawCurve(g, curveFor(1 - owner.slotIndex), divider.withAlpha(0.72f), 1.25f);
        drawCurve(g, curveFor(owner.slotIndex), accent, 2.25f);

        const auto& curve = owner.activeCurve();
        for (int index = 0; index < static_cast<int>(curve.points.size()); ++index)
        {
            const auto point = toScreen(curve.points[static_cast<std::size_t>(index)]);
            const bool selected = index == selectedPoint;
            g.setColour(selected ? focus : accent);
            g.fillEllipse(point.x - (selected ? 6.0f : 4.0f),
                          point.y - (selected ? 6.0f : 4.0f),
                          selected ? 12.0f : 8.0f, selected ? 12.0f : 8.0f);
            g.setColour(background);
            g.drawEllipse(point.x - (selected ? 6.0f : 4.0f),
                          point.y - (selected ? 6.0f : 4.0f),
                          selected ? 12.0f : 8.0f, selected ? 12.0f : 8.0f, 1.0f);
        }
        if (hasKeyboardFocus(true))
        {
            g.setColour(focus);
            g.fillRect(0, 0, getWidth(), 2);
            g.fillRect(0, getHeight() - 2, getWidth(), 2);
        }
    }

    void resized() override
    {
        plot = getLocalBounds().reduced(
            TR::Curves::UI::CurveEditorLayout::plotSafeInset);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        if (!plot.contains(event.getPosition())) return;
        if (drawMode)
        {
            drawing.clear();
            appendDrawPoint(event.position);
            return;
        }
        selectedPoint = nearestPoint(event.position, 12.0f);
        selectedSegment = selectedPoint >= 0 ? juce::jlimit(0,
            static_cast<int>(owner.activeCurve().points.size()) - 2, selectedPoint)
            : nearestSegment(event.position);
        owner.updateControls();
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (drawMode)
        {
            appendDrawPoint(event.position);
            return;
        }
        if (selectedPoint < 0) return;
        auto curve = owner.activeCurve();
        auto point = fromScreen(event.position);
        auto& target = curve.points[static_cast<std::size_t>(selectedPoint)];
        if (selectedPoint == 0) point.x = domainMinimum();
        else if (selectedPoint == static_cast<int>(curve.points.size()) - 1)
            point.x = domainMaximum();
        else
        {
            point.x = juce::jlimit(curve.points[static_cast<std::size_t>(selectedPoint - 1)].x + 0.0001f,
                                   curve.points[static_cast<std::size_t>(selectedPoint + 1)].x - 0.0001f,
                                   point.x);
        }
        target.x = point.x;
        target.y = point.y;
        owner.replaceActiveCurve(std::move(curve), false);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (drawMode)
        {
            commitDrawing();
            return;
        }
        if (selectedPoint >= 0) owner.pushHistory();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (drawMode || !plot.contains(event.getPosition())) return;
        auto curve = owner.activeCurve();
        const auto found = nearestPoint(event.position, 10.0f);
        if (found > 0 && found < static_cast<int>(curve.points.size()) - 1)
        {
            curve.points.erase(curve.points.begin() + found);
            selectedPoint = -1;
            selectedSegment = -1;
            owner.replaceActiveCurve(std::move(curve));
            return;
        }
        const auto maximum = owner.activeDomain() == TR::Curves::Domain::unipolar
                                 ? WaveShape::maximumUnipolarPointCount
                                 : WaveShape::maximumBipolarPointCount;
        if (curve.points.size() >= maximum) return;
        auto point = fromScreen(event.position);
        const auto insert = std::lower_bound(curve.points.begin(), curve.points.end(), point.x,
            [](const auto& item, float x) { return item.x < x; });
        if (insert == curve.points.begin() || insert == curve.points.end()) return;
        selectedPoint = static_cast<int>(std::distance(curve.points.begin(), insert));
        curve.points.insert(insert, point);
        selectedSegment = selectedPoint - 1;
        owner.replaceActiveCurve(std::move(curve));
    }

    void setSelectedTension(float value)
    {
        auto curve = owner.activeCurve();
        if (!juce::isPositiveAndBelow(selectedSegment,
                                      static_cast<int>(curve.points.size()) - 1)) return;
        curve.points[static_cast<std::size_t>(selectedSegment)].curvature =
            juce::jlimit(-1.0f, 1.0f, value);
        owner.replaceActiveCurve(std::move(curve), false);
    }

    void finishTensionGesture() { owner.pushHistory(); }

    void setSelectedCoordinate(bool horizontal, float value)
    {
        auto curve = owner.activeCurve();
        if (!juce::isPositiveAndBelow(selectedPoint, static_cast<int>(curve.points.size()))) return;
        auto& point = curve.points[static_cast<std::size_t>(selectedPoint)];
        if (horizontal)
        {
            if (selectedPoint == 0 || selectedPoint == static_cast<int>(curve.points.size()) - 1)
                return;
            point.x = juce::jlimit(curve.points[static_cast<std::size_t>(selectedPoint - 1)].x + 0.0001f,
                                   curve.points[static_cast<std::size_t>(selectedPoint + 1)].x - 0.0001f,
                                   value);
        }
        else
            point.y = juce::jlimit(domainMinimum(), domainMaximum(), value);
        owner.replaceActiveCurve(std::move(curve), false);
    }

    void finishPointGesture() { owner.pushHistory(); }

    bool deleteSelection()
    {
        auto curve = owner.activeCurve();
        if (selectedPoint <= 0 || selectedPoint >= static_cast<int>(curve.points.size()) - 1)
            return false;
        curve.points.erase(curve.points.begin() + selectedPoint);
        selectedPoint = juce::jlimit(0, static_cast<int>(curve.points.size()) - 1,
                                     selectedPoint - 1);
        selectedSegment = juce::jlimit(0, static_cast<int>(curve.points.size()) - 2,
                                       selectedPoint);
        owner.replaceActiveCurve(std::move(curve));
        return true;
    }

    bool resetPointSelection()
    {
        auto curve = owner.activeCurve();
        if (!juce::isPositiveAndBelow(selectedPoint, static_cast<int>(curve.points.size())))
            return false;
        auto& point = curve.points[static_cast<std::size_t>(selectedPoint)];
        if (selectedPoint == 0) point.y = owner.activeDomain() == TR::Curves::Domain::bipolar
                                        ? -1.0f : 0.0f;
        else if (selectedPoint == static_cast<int>(curve.points.size()) - 1) point.y = 1.0f;
        else
        {
            const auto& left = curve.points[static_cast<std::size_t>(selectedPoint - 1)];
            const auto& right = curve.points[static_cast<std::size_t>(selectedPoint + 1)];
            const auto amount = (point.x - left.x) / juce::jmax(0.0001f, right.x - left.x);
            point.y = left.y + amount * (right.y - left.y);
        }
        owner.replaceActiveCurve(std::move(curve));
        return true;
    }

    bool resetSegmentSelection()
    {
        auto curve = owner.activeCurve();
        if (!juce::isPositiveAndBelow(selectedSegment,
                                      static_cast<int>(curve.points.size()) - 1)) return false;
        curve.points[static_cast<std::size_t>(selectedSegment)].curvature = 0.0f;
        owner.replaceActiveCurve(std::move(curve));
        return true;
    }

private:
    float domainMinimum() const noexcept
    {
        return owner.activeDomain() == TR::Curves::Domain::bipolar ? -1.0f : 0.0f;
    }
    float domainMaximum() const noexcept { return 1.0f; }

    TR::Curves::Point fromScreen(juce::Point<float> position) const
    {
        return viewport().fromScreen(position, snap, gridSize, gridSize);
    }

    juce::Point<float> toScreen(const TR::Curves::Point& point) const
    {
        return viewport().toScreen(point);
    }

    void drawCurve(juce::Graphics& g, const TR::Curves::Curve& source,
                   juce::Colour colour, float thickness)
    {
        WaveShape::CompiledCurve compiled;
        if (!WaveShape::compileCurve(source, owner.activeDomain(), compiled)) return;
        juce::Path path;
        constexpr int samples = 256;
        for (int index = 0; index < samples; ++index)
        {
            const auto x = juce::jmap(static_cast<float>(index) / (samples - 1),
                                     domainMinimum(), domainMaximum());
            const auto point = toScreen({ x, compiled.evaluate(x), 0.0f });
            if (index == 0) path.startNewSubPath(point); else path.lineTo(point);
        }
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(thickness));
    }

    int nearestPoint(juce::Point<float> position, float radius) const
    {
        return viewport().nearestPoint(owner.activeCurve(), position, radius);
    }

    int nearestSegment(juce::Point<float> position) const
    {
        return TR::Curves::UI::CurveViewport::segmentAt(
            owner.activeCurve(), fromScreen(position).x);
    }

    TR::Curves::UI::CurveViewport viewport() const noexcept
    {
        return { plot, domainMinimum(), domainMaximum() };
    }

    void appendDrawPoint(juce::Point<float> position)
    {
        auto point = fromScreen(position);
        if (!drawing.empty() && point.x <= drawing.back().x) return;
        const auto maximum = owner.activeDomain() == TR::Curves::Domain::unipolar
                                 ? WaveShape::maximumUnipolarPointCount
                                 : WaveShape::maximumBipolarPointCount;
        if (drawing.size() >= maximum) return;
        drawing.push_back(point);
        repaint();
    }

    void commitDrawing()
    {
        if (drawing.empty()) return;
        auto curve = owner.activeCurve();
        const auto firstY = drawing.front().y;
        const auto lastY = drawing.back().y;
        drawing.front().x = domainMinimum();
        drawing.back().x = domainMaximum();
        if (drawing.size() == 1)
            drawing = { { domainMinimum(), firstY, 0.0f },
                        { domainMaximum(), lastY, 0.0f } };
        curve.points = std::move(drawing);
        selectedPoint = -1;
        selectedSegment = -1;
        owner.replaceActiveCurve(std::move(curve));
    }

    SatWaveShapeWorkspace& owner;
    juce::Rectangle<int> plot;
    std::vector<TR::Curves::Point> drawing;
    int selectedPoint = -1;
    int selectedSegment = -1;
    int gridSize = 16;
    bool snap = true;
    bool drawMode = false;
};

SatWaveShapeWorkspace::SatWaveShapeWorkspace(SatWaveShapeUiBackend& value) : backend(value)
{
    setWantsKeyboardFocus(true);
    canvas = std::make_unique<CurveCanvas>(*this);
    canvas->setComponentID("waveshape-curve-canvas");
    addAndMakeVisible(*canvas);
    for (auto* button : { &slotA, &slotB, &nodeMode, &drawMode, &grid, &snap,
                          &undo, &redo, &resetSegment, &resetPoint, &deletePoint,
                          &reset, &cancelButton, &applyButton })
        addAndMakeVisible(button);
    slotA.setComponentID("waveshape-slot-a");
    slotB.setComponentID("waveshape-slot-b");
    bipolar.setComponentID("waveshape-bipolar");
    nodeMode.setComponentID("waveshape-mode-nodes");
    drawMode.setComponentID("waveshape-mode-draw");
    grid.setComponentID("waveshape-grid");
    snap.setComponentID("waveshape-snap");
    undo.setComponentID("waveshape-undo");
    redo.setComponentID("waveshape-redo");
    reset.setComponentID("waveshape-reset");
    resetSegment.setComponentID("waveshape-reset-segment");
    resetPoint.setComponentID("waveshape-reset-point");
    deletePoint.setComponentID("waveshape-delete-point");
    cancelButton.setComponentID("waveshape-cancel");
    applyButton.setComponentID("waveshape-apply");
    addAndMakeVisible(bipolar);
    addAndMakeVisible(tension);
    addAndMakeVisible(pointXValue);
    addAndMakeVisible(pointYValue);
    addAndMakeVisible(tensionValue);
    addAndMakeVisible(selectionLabel);
    pointXValue.setComponentID("waveshape-point-x");
    pointYValue.setComponentID("waveshape-point-y");
    tension.setComponentID("waveshape-tension");
    tensionValue.setComponentID("waveshape-tension-value");
    selectionLabel.setComponentID("waveshape-selection");
    tension.setTitle("Selected segment tension");
    tension.setTooltip("Curve the selected segment from -1 to +1");
    selectionLabel.setBorderSize({});
    selectionLabel.setInterceptsMouseClicks(false, false);
    TR::Curves::UI::configureNumericLabel(
        pointXValue, "Selected point X value", "Type an exact X position");
    TR::Curves::UI::configureNumericLabel(
        pointYValue, "Selected point Y value", "Type an exact Y position");
    TR::Curves::UI::configureNumericLabel(
        tensionValue, "Selected segment curve", "Type an exact curve from -1 to +1");
    tension.setRange(-1.0, 1.0, 0.001);
    tension.setSliderStyle(juce::Slider::LinearHorizontal);
    tension.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    tension.setTrackVisible(true);
    tension.setRange(-1.0, 1.0, 0.001);
    tension.onValueChange = [this]
    {
        if (canvas != nullptr) canvas->setSelectedTension(static_cast<float>(tension.getValue()));
    };
    tension.onDragEnd = [this] { if (canvas != nullptr) canvas->finishTensionGesture(); };
    const auto configurePointEditor = [this](juce::Label& label,
                                              S::SimpleTextEditContract& edit,
                                              bool horizontal)
    {
        label.onEditorShow = [this, &label, &edit, horizontal]
        {
            edit.begin(label.getText());
            if (auto* editor = label.getCurrentTextEditor())
            {
                editor->onTextChange = [&edit, editor] { edit.update(editor->getText()); };
                editor->onReturnKey = [this, horizontal] { commitPointEdit(horizontal); };
                editor->onEscapeKey = [this, &label, &edit]
                {
                    cancelPointEdit(label, edit);
                };
                editor->onFocusLost = [this, horizontal] { commitPointEdit(horizontal); };
            }
        };
        label.onEditorHide = [this, &edit, horizontal]
        {
            if (edit.isActive()) commitPointEdit(horizontal);
        };
    };
    configurePointEditor(pointXValue, pointXEdit, true);
    configurePointEditor(pointYValue, pointYEdit, false);
    tensionValue.onEditorShow = [this]
    {
        tensionEdit.begin(tensionValue.getText());
        if (auto* editor = tensionValue.getCurrentTextEditor())
        {
            editor->onTextChange = [this, editor] { tensionEdit.update(editor->getText()); };
            editor->onReturnKey = [this] { commitTensionEdit(); };
            editor->onEscapeKey = [this] { cancelTensionEdit(); };
            editor->onFocusLost = [this] { commitTensionEdit(); };
        }
    };
    tensionValue.onEditorHide = [this]
    {
        if (tensionEdit.isActive()) commitTensionEdit();
    };
    slotA.onClick = [this] { selectSlot(0); };
    slotB.onClick = [this] { selectSlot(1); };
    bipolar.onClick = [this] { setBipolar(bipolar.getToggleState()); };
    nodeMode.onClick = [this] { canvas->setDrawMode(false); updateControls(); };
    drawMode.onClick = [this] { canvas->setDrawMode(true); updateControls(); };
    grid.onClick = [this]
    {
        const auto next = canvas->getGridSize() == 8 ? 16 : (canvas->getGridSize() == 16 ? 32 : 8);
        canvas->setGridSize(next);
        updateControls();
    };
    snap.onClick = [this] { canvas->setSnap(!canvas->snapEnabled()); updateControls(); };
    undo.onClick = [this] { restoreHistory(editHistory.index() - 1); };
    redo.onClick = [this] { restoreHistory(editHistory.index() + 1); };
    resetSegment.onClick = [this] { resetActiveSegment(); };
    resetPoint.onClick = [this] { resetSelectedPoint(); };
    deletePoint.onClick = [this] { deleteSelectedPoint(); };
    reset.onClick = [this] { resetActiveCurve(); };
    cancelButton.onClick = [this] { cancel(); };
    applyButton.onClick = [this] { apply(); };
}

SatWaveShapeWorkspace::~SatWaveShapeWorkspace() = default;

bool SatWaveShapeWorkspace::canOpenContext(int index) const
{
    return juce::isPositiveAndBelow(index, WaveShape::loaderCount)
        && backend.isWaveShapeEnabled(index);
}

bool SatWaveShapeWorkspace::beginContext(int index)
{
    if (!canOpenContext(index)) return false;
    loaderIndex = index;
    slotIndex = 0;
    original = backend.waveShapeState();
    draft = original;
    editHistory.reset(draft);
    canvas->resetSelection();
    updateControls();
    return true;
}

void SatWaveShapeWorkspace::refreshWorkspace()
{
    if (loaderIndex < 0) return;
    lookAndFeelChanged();
    updateControls();
    canvas->repaint();
}

TR::Curves::Domain SatWaveShapeWorkspace::activeDomain() const noexcept
{
    if (loaderIndex < 0) return TR::Curves::Domain::unipolar;
    return draft.loaders[static_cast<std::size_t>(loaderIndex)].polarity
               == WaveShape::PolarityMode::bipolar
           ? TR::Curves::Domain::bipolar : TR::Curves::Domain::unipolar;
}

TR::Curves::Curve& SatWaveShapeWorkspace::activeCurve()
{
    auto& slot = draft.loaders[static_cast<std::size_t>(loaderIndex)]
                     .slots[static_cast<std::size_t>(slotIndex)];
    return activeDomain() == TR::Curves::Domain::bipolar ? slot.bipolar : slot.unipolar;
}

const TR::Curves::Curve& SatWaveShapeWorkspace::activeCurve() const
{
    const auto& slot = draft.loaders[static_cast<std::size_t>(loaderIndex)]
                           .slots[static_cast<std::size_t>(slotIndex)];
    return activeDomain() == TR::Curves::Domain::bipolar ? slot.bipolar : slot.unipolar;
}

void SatWaveShapeWorkspace::replaceActiveCurve(TR::Curves::Curve curve, bool addHistory)
{
    const auto maximum = activeDomain() == TR::Curves::Domain::unipolar
                             ? WaveShape::maximumUnipolarPointCount
                             : WaveShape::maximumBipolarPointCount;
    if (!TR::Curves::validate(curve, activeDomain(), maximum).ok()) return;
    activeCurve() = std::move(curve);
    if (addHistory) pushHistory();
    updateControls();
    canvas->repaint();
}

void SatWaveShapeWorkspace::pushHistory()
{
    editHistory.push(draft);
    updateControls();
}

void SatWaveShapeWorkspace::restoreHistory(int index)
{
    const auto* restored = editHistory.restore(index);
    if (restored == nullptr) return;
    draft = *restored;
    updateControls();
    canvas->repaint();
}

void SatWaveShapeWorkspace::selectSlot(int slot)
{
    slotIndex = juce::jlimit(0, WaveShape::slotCount - 1, slot);
    canvas->resetSelection();
    updateControls();
    canvas->repaint();
}

void SatWaveShapeWorkspace::setBipolar(bool enabled)
{
    auto next = draft;
    if (!WaveShape::setPolarityMode(next.loaders[static_cast<std::size_t>(loaderIndex)],
                                    enabled ? WaveShape::PolarityMode::bipolar
                                            : WaveShape::PolarityMode::unipolar))
        return;
    draft = std::move(next);
    canvas->resetSelection();
    pushHistory();
    updateControls();
    canvas->repaint();
}

void SatWaveShapeWorkspace::resetActiveSegment()
{
    if (canvas != nullptr) canvas->resetSegmentSelection();
}

void SatWaveShapeWorkspace::resetSelectedPoint()
{
    if (canvas != nullptr) canvas->resetPointSelection();
}

void SatWaveShapeWorkspace::deleteSelectedPoint()
{
    if (canvas != nullptr) canvas->deleteSelection();
}

void SatWaveShapeWorkspace::resetActiveCurve()
{
    replaceActiveCurve(TR::Curves::makeIdentity(activeDomain()));
}

void SatWaveShapeWorkspace::apply()
{
    if (WaveShape::validate(draft).ok() && backend.setWaveShapeState(draft))
    {
        original = draft;
        if (onCloseRequested) onCloseRequested();
    }
}

void SatWaveShapeWorkspace::cancel()
{
    draft = original;
    if (onCloseRequested) onCloseRequested();
}

void SatWaveShapeWorkspace::commitPointEdit(bool horizontal)
{
    auto& edit = horizontal ? pointXEdit : pointYEdit;
    auto& label = horizontal ? pointXValue : pointYValue;
    if (!edit.isActive()) return;
    const auto token = edit.pendingValue().retainCharacters("-+.0123456789");
    if (!token.containsAnyOf("0123456789"))
    {
        cancelPointEdit(label, edit);
        return;
    }
    if (canvas != nullptr)
    {
        canvas->setSelectedCoordinate(horizontal, static_cast<float>(token.getDoubleValue()));
        canvas->finishPointGesture();
    }
    edit.confirm({}, [](const juce::String&) { return true; });
    label.hideEditor(true);
    updateControls();
}

void SatWaveShapeWorkspace::cancelPointEdit(
    juce::Label& label, S::SimpleTextEditContract& edit)
{
    if (!edit.isActive()) return;
    label.setText(edit.cancel(), juce::dontSendNotification);
    label.hideEditor(false);
}

void SatWaveShapeWorkspace::commitTensionEdit()
{
    if (!tensionEdit.isActive()) return;
    const auto token = tensionEdit.pendingValue().retainCharacters("-+.0123456789");
    if (!token.containsAnyOf("0123456789"))
    {
        cancelTensionEdit();
        return;
    }
    if (canvas != nullptr)
    {
        canvas->setSelectedTension(
            juce::jlimit(-1.0f, 1.0f, static_cast<float>(token.getDoubleValue())));
        canvas->finishTensionGesture();
    }
    tensionEdit.confirm({}, [](const juce::String&) { return true; });
    tensionValue.hideEditor(true);
    updateControls();
}

void SatWaveShapeWorkspace::cancelTensionEdit()
{
    if (!tensionEdit.isActive()) return;
    tensionValue.setText(tensionEdit.cancel(), juce::dontSendNotification);
    tensionValue.hideEditor(false);
}

void SatWaveShapeWorkspace::updateControls()
{
    const bool bipolarEnabled = loaderIndex >= 0
        && draft.loaders[static_cast<std::size_t>(loaderIndex)].polarity
               == WaveShape::PolarityMode::bipolar;
    slotA.setToggleState(slotIndex == 0, juce::dontSendNotification);
    slotB.setToggleState(slotIndex == 1, juce::dontSendNotification);
    bipolar.setToggleState(bipolarEnabled, juce::dontSendNotification);
    nodeMode.setToggleState(!canvas->isDrawMode(), juce::dontSendNotification);
    drawMode.setToggleState(canvas->isDrawMode(), juce::dontSendNotification);
    grid.setButtonText("GRID " + juce::String(canvas->getGridSize()) + "x"
                       + juce::String(canvas->getGridSize()));
    snap.setButtonText(canvas->snapEnabled() ? "SNAP ON" : "SNAP OFF");
    undo.setEnabled(editHistory.canUndo());
    redo.setEnabled(editHistory.canRedo());
    const auto segment = canvas->selectedSegmentIndex();
    const auto pointIndex = canvas->selectedPointIndex();
    const auto& curve = activeCurve();
    const bool hasSegment = juce::isPositiveAndBelow(segment,
        static_cast<int>(curve.points.size()) - 1);
    tension.setEnabled(hasSegment);
    tension.setValue(hasSegment ? curve.points[static_cast<std::size_t>(segment)].curvature : 0.0,
                     juce::dontSendNotification);
    const bool hasPoint = juce::isPositiveAndBelow(pointIndex, static_cast<int>(curve.points.size()));
    const bool pointXEditable = hasPoint && pointIndex > 0
        && pointIndex < static_cast<int>(curve.points.size()) - 1;
    pointXValue.setEnabled(pointXEditable);
    pointYValue.setEnabled(hasPoint);
    if (!pointXValue.isBeingEdited())
        pointXValue.setText(hasPoint
            ? "X " + juce::String(curve.points[static_cast<std::size_t>(pointIndex)].x, 3)
            : "X --", juce::dontSendNotification);
    if (!pointYValue.isBeingEdited())
        pointYValue.setText(hasPoint
            ? "Y " + juce::String(curve.points[static_cast<std::size_t>(pointIndex)].y, 3)
            : "Y --", juce::dontSendNotification);
    tensionValue.setEnabled(hasSegment);
    if (!tensionValue.isBeingEdited())
        tensionValue.setText(hasSegment
            ? "CURVE " + juce::String(
                curve.points[static_cast<std::size_t>(segment)].curvature, 3)
            : "CURVE --", juce::dontSendNotification);
    selectionLabel.setText(hasPoint
        ? "POINT " + juce::String(pointIndex + 1) + " / " + juce::String(curve.points.size())
            + (pointIndex < static_cast<int>(curve.points.size()) - 1
                   ? "  OUT " + juce::String(pointIndex + 1) + " -> "
                         + juce::String(pointIndex + 2)
                   : "  ENDPOINT")
        : hasSegment
            ? "OUT SEGMENT " + juce::String(segment + 1) + " -> "
                  + juce::String(segment + 2)
            : "NO SELECTION",
        juce::dontSendNotification);
    resetSegment.setEnabled(hasSegment);
    resetPoint.setEnabled(hasPoint);
    deletePoint.setEnabled(hasPoint && pointIndex > 0
                           && pointIndex < static_cast<int>(curve.points.size()) - 1);
}

void SatWaveShapeWorkspace::paint(juce::Graphics& g)
{
    g.fillAll(S::simpleColour(S::SimpleColourRole::background, this));
    TR::Curves::UI::paintNumericField(g, *this, pointXValue);
    TR::Curves::UI::paintNumericField(g, *this, pointYValue);
    TR::Curves::UI::paintNumericField(g, *this, tensionValue);
    g.setColour(S::simpleColour(S::SimpleColourRole::text, this));
    g.setFont(S::simpleFont(S::defaultTokens().type.functional + 4.0f, true));
    const auto loader = loaderIndex >= 0
        ? juce::String(std::string(1, static_cast<char>('A' + loaderIndex))) : "";
    g.drawText("WAVE SHAPE / " + loader, titleArea, juce::Justification::centredLeft);
    g.setColour(S::simpleColour(S::SimpleColourRole::divider, this));
    g.drawHorizontalLine(titleArea.getBottom(), static_cast<float>(titleArea.getX()),
                         static_cast<float>(titleArea.getX()
                             + TR::Curves::UI::CurveEditorLayout::canvasWidth
                             + TR::Curves::UI::CurveEditorLayout::inspectorWidth));
    g.setFont(S::simpleFont(S::defaultTokens().type.auxiliary));
    g.setColour(S::simpleColour(S::SimpleColourRole::secondaryText, this));
    if (canvas != nullptr)
    {
        g.drawText(juce::String(canvas->pointCount()) + " / "
                       + juce::String(activeDomain() == TR::Curves::Domain::unipolar
                                          ? WaveShape::maximumUnipolarPointCount
                                          : WaveShape::maximumBipolarPointCount)
                       + " NODES",
                   statusArea, juce::Justification::centredLeft);
        g.drawText("GRID " + juce::String(canvas->getGridSize()) + "x"
                       + juce::String(canvas->getGridSize()),
                   statusArea, juce::Justification::centredRight);
    }
}

void SatWaveShapeWorkspace::paintOverChildren(juce::Graphics& g)
{
    const auto divider = S::simpleColour(S::SimpleColourRole::divider, this);
    g.setColour(divider);
    g.drawRect(bodyArea, 1);
    g.drawVerticalLine(inspectorArea.getX(), static_cast<float>(bodyArea.getY()),
                       static_cast<float>(bodyArea.getBottom()));
}

void SatWaveShapeWorkspace::resized()
{
    using SharedLayout = TR::Curves::UI::CurveEditorLayout;
    const auto shared = SharedLayout::calculate(getLocalBounds());
    auto titleRow = shared.title;
    titleArea = titleRow.removeFromLeft(SharedLayout::canvasWidth);
    auto titleActions = titleRow;
    cancelButton.setBounds(titleActions.removeFromLeft(116)
        .withSizeKeepingCentre(116, SharedLayout::controlHeight));
    titleActions.removeFromLeft(SharedLayout::controlGap);
    applyButton.setBounds(titleActions.removeFromLeft(116)
        .withSizeKeepingCentre(116, SharedLayout::controlHeight));

    const auto centred = [](juce::Rectangle<int> bounds)
    {
        return bounds.withSizeKeepingCentre(bounds.getWidth(),
                                            SharedLayout::controlHeight);
    };
    auto tools = shared.toolbarCanvas;
    slotA.setBounds(centred(tools.removeFromLeft(48)));
    tools.removeFromLeft(6);
    slotB.setBounds(centred(tools.removeFromLeft(48)));
    tools.removeFromLeft(10);
    bipolar.setBounds(centred(tools.removeFromLeft(110)));
    tools.removeFromLeft(10);
    nodeMode.setBounds(centred(tools.removeFromLeft(82)));
    tools.removeFromLeft(6);
    drawMode.setBounds(centred(tools.removeFromLeft(82)));
    snap.setBounds(centred(tools.removeFromRight(94)));
    tools.removeFromRight(SharedLayout::controlGap);
    grid.setBounds(centred(tools.removeFromRight(104)));

    auto history = shared.toolbarInspector;
    undo.setBounds(centred(history.removeFromLeft(116)));
    history.removeFromLeft(SharedLayout::controlGap);
    redo.setBounds(centred(history.removeFromLeft(116)));

    bodyArea = shared.body;
    inspectorArea = shared.inspector;
    statusArea = shared.status;
    canvas->setBounds(shared.canvas);
    auto inspector = shared.inspector.reduced(12);
    selectionLabel.setBounds(inspector.removeFromTop(34));
    inspector.removeFromTop(10);
    auto coordinates = inspector.removeFromTop(34);
    auto xCell = coordinates.removeFromLeft((coordinates.getWidth()
        - SharedLayout::controlGap) / 2);
    coordinates.removeFromLeft(8);
    auto yCell = coordinates;
    pointXValue.setBounds(xCell);
    pointYValue.setBounds(yCell);
    inspector.removeFromTop(8);
    auto curveRow = inspector.removeFromTop(34);
    tensionValue.setBounds(curveRow.removeFromRight(88));
    tension.setBounds(curveRow);
    constexpr int actionHeight = 34;
    constexpr int actionCount = 4;
    auto actions = inspector.removeFromBottom(actionHeight * actionCount
                                               + SharedLayout::controlGap * (actionCount - 1));
    for (auto* button : { &resetSegment, &resetPoint, &deletePoint, &reset })
    {
        button->setBounds(actions.removeFromTop(actionHeight));
        actions.removeFromTop(juce::jmin(SharedLayout::controlGap, actions.getHeight()));
    }
}

void SatWaveShapeWorkspace::lookAndFeelChanged()
{
    const auto text = S::simpleColour(S::SimpleColourRole::text, this);
    const auto secondary = S::simpleColour(S::SimpleColourRole::secondaryText, this);
    selectionLabel.setColour(juce::Label::textColourId, text);
    for (auto* label : { &pointXValue, &pointYValue, &tensionValue })
        label->setColour(juce::Label::textColourId, secondary);
    repaint();
}

bool SatWaveShapeWorkspace::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { cancel(); return true; }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z')
    {
        restoreHistory(editHistory.index() - 1);
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Y')
    {
        restoreHistory(editHistory.index() + 1);
        return true;
    }
    return false;
}
}

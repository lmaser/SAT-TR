#pragma once
//======================================================================
//  TRSharedUI.h — Shared GUI utilities for the TR plugin family
//  (ECHO-TR, DISP-TR, FREQ-TR, …)
//
//  Header-only.  All symbols live in namespace TR.
//  Include from each plugin's PluginEditor.h or PluginEditor.cpp
//  and add  `using namespace TR;`  for convenience.
//======================================================================

#include <JuceHeader.h>
#include <unordered_map>
#include <functional>
#include <cmath>

namespace TR
{

//======================================================================
//  Colour scheme
//======================================================================
struct TRScheme
{
    juce::Colour bg;
    juce::Colour fg;
    juce::Colour outline;
    juce::Colour text;
};

//======================================================================
//  String-width measurement with FNV-1a cache
//======================================================================
inline std::unordered_map<std::size_t, int>& getStringWidthCache()
{
    static thread_local std::unordered_map<std::size_t, int> cache;
    return cache;
}

inline int stringWidth (const juce::Font& font, const juce::String& s)
{
    if (s.isEmpty())
        return 0;

    auto& cache = getStringWidthCache();

    if (cache.size() > 8192)
        cache.clear();

    // FNV-1a 64-bit hash — avoids string allocation for cache key
    std::size_t h = 14695981039346656037ull;
    auto mix = [&] (std::size_t v) { h ^= v; h *= 1099511628211ull; };

    mix (static_cast<std::size_t> ((int) (font.getHeight() * 100.0f)));
    mix (static_cast<std::size_t> ((int) (font.getHorizontalScale() * 100.0f)));
    mix (static_cast<std::size_t> (font.isBold()   ? 1u : 0u));
    mix (static_cast<std::size_t> (font.isItalic()  ? 1u : 0u));

    const auto& typefaceName = font.getTypefaceName();
    for (int i = 0; i < typefaceName.length(); ++i)
        mix (static_cast<std::size_t> (typefaceName[i]));

    for (int i = 0; i < s.length(); ++i)
        mix (static_cast<std::size_t> (s[i]));

    if (const auto it = cache.find (h); it != cache.end())
        return it->second;

    juce::GlyphArrangement ga;
    ga.addLineOfText (font, s, 0.0f, 0.0f);
    const int width = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    cache.emplace (h, width);
    return width;
}

//======================================================================
//  Font factories
//======================================================================
inline const juce::Font& kBoldFont40()
{
    static const juce::Font f (juce::FontOptions (40.0f).withStyle ("Bold"));
    return f;
}

inline juce::Font makeOverlayDisplayFont()
{
    return juce::Font (juce::FontOptions (28.0f).withStyle ("Bold"));
}

//======================================================================
//  UI metrics — tick boxes, tooltips, version label
//======================================================================
namespace UiMetrics
{
    inline constexpr float tickBoxOuterScale            = 2.0f;
    inline constexpr float tickBoxHorizontalBiasRatio   = 0.1171875f;
    inline constexpr float tickBoxInnerInsetRatio       = 0.25f;

    inline constexpr int   tooltipMinWidth              = 120;
    inline constexpr int   tooltipMinHeight             = 38;
    inline constexpr float tooltipHeightScale           = 1.5f;
    inline constexpr float tooltipAnchorXRatio          = 0.42f;
    inline constexpr float tooltipAnchorYRatio          = 0.58f;
    inline constexpr float tooltipParentMarginRatio     = 0.11f;
    inline constexpr float tooltipWidthPadFontRatio     = 0.8f;
    inline constexpr float tooltipTextInsetXRatio       = 0.21f;
    inline constexpr float tooltipTextInsetYRatio       = 0.05f;

    inline constexpr float versionFontRatio             = 0.42f;
    inline constexpr float versionHeightRatio           = 0.62f;
    inline constexpr float versionDesiredWidthRatio     = 1.9f;
}

//======================================================================
//  Graphics-popup layout constants
//======================================================================
struct GraphicsPromptLayout
{
    static constexpr int toggleBox          = 34;
    static constexpr int toggleGap          = 10;
    static constexpr int swatchSize         = 40;
    static constexpr int swatchGap          = 8;
    static constexpr int columnGap          = 28;
    static constexpr int titleHeight        = 24;
    static constexpr int titleToModeGap     = 14;
    static constexpr int modeToSwatchesGap  = 14;
};

//======================================================================
//  Prompt / AlertWindow constants
//======================================================================
inline constexpr int   kPromptWidth                 = 460;
inline constexpr int   kPromptHeight                = 336;
inline constexpr int   kPromptInnerMargin           = 24;
inline constexpr int   kPromptFooterBottomPad       = 24;
inline constexpr int   kPromptFooterGap             = 12;
inline constexpr int   kPromptBodyTopPad            = 24;
inline constexpr int   kPromptBodyBottomPad         = 18;
inline constexpr const char* kPromptSuffixLabelId   = "promptSuffixLabel";

inline constexpr float kPromptEditorHeightScale     = 1.4f;
inline constexpr int   kPromptEditorHeightPadPx     = 6;
inline constexpr int   kPromptEditorRaiseYPx        = 8;
inline constexpr int   kPromptEditorMinTopPx        = 6;
inline constexpr int   kPromptEditorMinWidthPx      = 180;
inline constexpr int   kPromptEditorMaxWidthPx      = 240;
inline constexpr int   kPromptEditorHostPadPx       = 80;

inline constexpr int   kPromptInlineContentPadPx    = 8;

//======================================================================
//  Small utilities
//======================================================================
inline double roundToDecimals (double value, int decimals)
{
    const int safe = juce::jlimit (0, 9, decimals);
    const double scale = std::pow (10.0, (double) safe);
    return std::round (value * scale) / scale;
}

//======================================================================
//  Drawing helpers
//======================================================================
inline void drawOverlayPanel (juce::Graphics& g,
                              juce::Rectangle<int> bounds,
                              juce::Colour background,
                              juce::Colour outline,
                              int thickness = 2)
{
    g.setColour (background);
    g.fillRect (bounds);

    g.setColour (outline);
    g.drawRect (bounds, thickness);
}

inline bool fits (juce::Graphics& g, const juce::String& s, int w)
{
    if (w <= 0) return false;
    return stringWidth (g.getCurrentFont(), s) <= w;
}

inline bool drawIfFitsWithOptionalShrink (juce::Graphics& g,
                                          const juce::Rectangle<int>& area,
                                          const juce::String& text,
                                          float baseFontPx,
                                          float shrinkFloorPx)
{
    auto font = g.getCurrentFont();
    font.setHeight (baseFontPx);
    font.setHorizontalScale (1.0f);
    g.setFont (font);

    if (fits (g, text, area.getWidth()))
    {
        g.drawText (text, area, juce::Justification::left, false);
        return true;
    }

    // Compress horizontally instead of shrinking font height so that
    // vertical size stays constant during horizontal resize.
    const float minScale = juce::jlimit (0.4f, 1.0f, shrinkFloorPx / baseFontPx);
    for (float s = 1.0f - 0.025f; s >= minScale; s -= 0.025f)
    {
        font.setHorizontalScale (s);
        g.setFont (font);
        if (fits (g, text, area.getWidth()))
        {
            g.drawText (text, area, juce::Justification::left, false);
            return true;
        }
    }

    return false;
}

inline void drawValueNoEllipsis (juce::Graphics& g,
                                 const juce::Rectangle<int>& area,
                                 const juce::String& fullText,
                                 const juce::String& noUnitText,
                                 const juce::String& intOnlyText,
                                 float baseFontPx,
                                 float minFontPx)
{
    if (area.getWidth() <= 2 || area.getHeight() <= 2)
        return;

    const float softShrinkFloor = minFontPx;

    if (drawIfFitsWithOptionalShrink (g, area, fullText, baseFontPx, softShrinkFloor))
        return;

    if (noUnitText.isNotEmpty() && drawIfFitsWithOptionalShrink (g, area, noUnitText, baseFontPx, softShrinkFloor))
        return;

    {
        auto font = g.getCurrentFont();
        font.setHeight (baseFontPx);
        font.setHorizontalScale (1.0f);
        g.setFont (font);

        if (intOnlyText.isNotEmpty() && fits (g, intOnlyText, area.getWidth()))
        {
            g.drawText (intOnlyText, area, juce::Justification::left, false);
            return;
        }

        const float minScale = juce::jlimit (0.4f, 1.0f, minFontPx / baseFontPx);
        for (float s = 1.0f - 0.025f; s >= minScale; s -= 0.025f)
        {
            font.setHorizontalScale (s);
            g.setFont (font);
            if (intOnlyText.isNotEmpty() && fits (g, intOnlyText, area.getWidth()))
            {
                g.drawText (intOnlyText, area, juce::Justification::left, false);
                return;
            }
        }
    }
}

inline bool drawValueWithRightAlignedSuffix (juce::Graphics& g,
                                             const juce::Rectangle<int>& area,
                                             const juce::String& valueText,
                                             const juce::String& suffixText,
                                             bool enableAutoMargin,
                                             float baseFontPx,
                                             float minFontPx)
{
    constexpr int kAutoMarginThresholdPx = 24;

    if (area.getWidth() <= 2 || area.getHeight() <= 2)
        return false;

    auto font = g.getCurrentFont();
    font.setHeight (baseFontPx);

    const float minScale = juce::jlimit (0.4f, 1.0f, minFontPx / baseFontPx);
    for (float s = 1.0f; s >= minScale; s -= 0.025f)
    {
        font.setHorizontalScale (s);
        g.setFont (font);

        const int suffixW = stringWidth (font, suffixText);
        const int valueW  = stringWidth (font, valueText);
        const int gapW    = juce::jmax (2, stringWidth (font, " "));

        const int totalW = valueW + (suffixText.isNotEmpty() ? gapW : 0) + suffixW;
        if (totalW > area.getWidth())
            continue;

        const int suffixX = area.getRight() - suffixW;
        const int valueRight = suffixX - (suffixText.isNotEmpty() ? gapW : 0);
        const int fullValueAreaW = juce::jmax (1, valueRight - area.getX());
        const int freeSpace = juce::jmax (0, fullValueAreaW - valueW);

        int valueX = area.getX();
        if (enableAutoMargin && freeSpace > kAutoMarginThresholdPx)
            valueX += freeSpace / 2;

        const int valueAreaW = juce::jmax (1, valueRight - valueX);

        g.drawText (valueText, valueX, area.getY(), valueAreaW, area.getHeight(), juce::Justification::left, false);
        g.drawText (suffixText, suffixX, area.getY(), suffixW, area.getHeight(), juce::Justification::left, false);

        return true;
    }

    return false;
}

//======================================================================
//  Prompt shell & button layout
//======================================================================
inline void applyPromptShellSize (juce::AlertWindow& aw)
{
    aw.setSize (kPromptWidth, kPromptHeight);
}

inline int getAlertButtonsTop (const juce::AlertWindow& aw)
{
    int buttonsTop = aw.getHeight() - (kPromptFooterBottomPad + 36);
    for (int i = 0; i < aw.getNumButtons(); ++i)
        if (auto* btn = aw.getButton (i))
            buttonsTop = juce::jmin (buttonsTop, btn->getY());
    return buttonsTop;
}

inline void styleAlertButtons (juce::AlertWindow& aw, juce::LookAndFeel& lnf)
{
    for (int i = 0; i < aw.getNumButtons(); ++i)
    {
        if (auto* btn = dynamic_cast<juce::TextButton*> (aw.getButton (i)))
        {
            btn->setColour (juce::TextButton::buttonColourId,   lnf.findColour (juce::TextButton::buttonColourId));
            btn->setColour (juce::TextButton::buttonOnColourId, lnf.findColour (juce::TextButton::buttonOnColourId));
            btn->setColour (juce::TextButton::textColourOffId,  lnf.findColour (juce::TextButton::textColourOffId));
            btn->setColour (juce::TextButton::textColourOnId,   lnf.findColour (juce::TextButton::textColourOnId));
        }
    }
}

inline void layoutAlertWindowButtons (juce::AlertWindow& aw)
{
    const int btnCount = aw.getNumButtons();
    if (btnCount <= 0)
        return;

    const int footerY    = aw.getHeight() - kPromptFooterBottomPad;
    const int sideMargin = kPromptInnerMargin;
    const int buttonGap  = kPromptFooterGap;

    if (btnCount == 1)
    {
        if (auto* btn = aw.getButton (0))
        {
            auto r = btn->getBounds();
            r.setWidth (juce::jmax (80, r.getWidth()));
            r.setX ((aw.getWidth() - r.getWidth()) / 2);
            r.setY (footerY - r.getHeight());
            btn->setBounds (r);
        }
        return;
    }

    const int totalW   = aw.getWidth();
    const int totalGap = (btnCount - 1) * buttonGap;
    const int btnWidth = juce::jmax (20, (totalW - (2 * sideMargin) - totalGap) / btnCount);

    int x = sideMargin;
    for (int i = 0; i < btnCount; ++i)
    {
        if (auto* btn = aw.getButton (i))
        {
            auto r = btn->getBounds();
            r.setWidth (btnWidth);
            r.setY (footerY - r.getHeight());
            r.setX (x);
            btn->setBounds (r);
        }
        x += btnWidth + buttonGap;
    }
}

/** Style, layout and configure escape for all buttons in an AlertWindow.
    Call AFTER addButton(). Optionally applies a sizing function before layout. */
inline void finalizePromptButtons (juce::AlertWindow& aw, juce::LookAndFeel& lnf,
                                   std::function<void (juce::AlertWindow&)> preSizeFn = nullptr)
{
    styleAlertButtons (aw, lnf);
    if (preSizeFn) preSizeFn (aw);
    layoutAlertWindowButtons (aw);
    aw.setEscapeKeyCancels (true);
}

//======================================================================
//  Modal / overlay helpers
//======================================================================
inline void dismissEditorOwnedModalPrompts (juce::LookAndFeel& editorLookAndFeel)
{
    for (int i = juce::Component::getNumCurrentlyModalComponents() - 1; i >= 0; --i)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent (i);
        auto* alertWindow = dynamic_cast<juce::AlertWindow*> (modal);

        if (alertWindow == nullptr)
            continue;

        if (&alertWindow->getLookAndFeel() != &editorLookAndFeel)
            continue;

        alertWindow->exitModalState (0);
    }
}

inline void bringPromptWindowToFront (juce::AlertWindow& aw)
{
    aw.setAlwaysOnTop (true);
    aw.toFront (true);
}

/// Embed an AlertWindow in the editor's overlay and centre it.
/// EditorT must expose:
///   - void setPromptOverlayActive (bool)
///   - Component promptOverlay       (public member)
///   - unique_ptr<TooltipWindow> tooltipWindow  (public member)
template <typename EditorT>
inline void embedAlertWindowInOverlay (EditorT* editor,
                                       juce::AlertWindow* aw,
                                       bool bringTooltip = false)
{
    if (editor == nullptr || aw == nullptr)
        return;

    editor->setPromptOverlayActive (true);
    aw->setInterceptsMouseClicks (false, true);

    // Position BEFORE making visible to avoid a one-frame flash at (0,0)
    const int bx = juce::jmax (0, (editor->getWidth() - aw->getWidth()) / 2);
    const int by = juce::jmax (0, (editor->getHeight() - aw->getHeight()) / 2);
    aw->setBounds (bx, by, aw->getWidth(), aw->getHeight());
    editor->promptOverlay.addChildComponent (*aw);
    aw->setVisible (true);
    aw->toFront (false);
    if (bringTooltip && editor->tooltipWindow)
        editor->tooltipWindow->toFront (true);
    aw->repaint();
}

/// Ensure an AlertWindow fits the editor width when embedded.
template <typename EditorT>
inline void fitAlertWindowToEditor (juce::AlertWindow& aw,
                                    EditorT* editor,
                                    std::function<void(juce::AlertWindow&)> layoutCb = {})
{
    if (editor == nullptr)
        return;

    const int overlayPad = 12;
    const int availW = juce::jmax (120, editor->getWidth() - (overlayPad * 2));
    if (aw.getWidth() > availW)
    {
        aw.setSize (availW, juce::jmin (aw.getHeight(), editor->getHeight() - (overlayPad * 2)));
        if (layoutCb)
            layoutCb (aw);
    }
}

/// Re-centre all modal prompts owned by the editor LookAndFeel.
template <typename EditorT>
inline void anchorEditorOwnedPromptWindows (EditorT& editor,
                                            juce::LookAndFeel& editorLookAndFeel)
{
    for (int i = juce::Component::getNumCurrentlyModalComponents() - 1; i >= 0; --i)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent (i);
        auto* alertWindow = dynamic_cast<juce::AlertWindow*> (modal);

        if (alertWindow == nullptr)
            continue;

        if (&alertWindow->getLookAndFeel() != &editorLookAndFeel)
            continue;

        alertWindow->centreAroundComponent (&editor, alertWindow->getWidth(), alertWindow->getHeight());
        bringPromptWindowToFront (*alertWindow);
    }
}

//======================================================================
//  Prompt text editor styling
//======================================================================
inline void stylePromptTextEditor (juce::TextEditor& te,
                                   juce::Colour bg,
                                   juce::Colour text,
                                   juce::Colour accent,
                                   const juce::Font& baseFont,
                                   int hostWidth,
                                   bool widenAndCenter)
{
    te.setFont (baseFont);
    te.applyFontToAllText (baseFont);
    te.setJustification (juce::Justification::centred);
    te.setIndents (0, 0);

    te.setColour (juce::TextEditor::backgroundColourId,      bg);
    te.setColour (juce::TextEditor::textColourId,            text);
    te.setColour (juce::TextEditor::outlineColourId,         bg);
    te.setColour (juce::TextEditor::focusedOutlineColourId,  bg);
    te.setColour (juce::TextEditor::highlightColourId,       accent.withAlpha (0.35f));
    te.setColour (juce::TextEditor::highlightedTextColourId, text);

    auto r = te.getBounds();
    r.setHeight ((int) (baseFont.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
    r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));

    if (widenAndCenter)
    {
        const int editorW = juce::jlimit (kPromptEditorMinWidthPx,
                                          kPromptEditorMaxWidthPx,
                                          hostWidth - kPromptEditorHostPadPx);
        r.setWidth (editorW);
        r.setX ((hostWidth - r.getWidth()) / 2);
    }

    te.setBounds (r);
    te.selectAll();
}

inline void centrePromptTextEditorVertically (juce::AlertWindow& aw,
                                              juce::TextEditor& te,
                                              int minTop = kPromptEditorMinTopPx)
{
    int buttonsTop = aw.getHeight();
    for (int i = 0; i < aw.getNumButtons(); ++i)
        if (auto* btn = aw.getButton (i))
            buttonsTop = juce::jmin (buttonsTop, btn->getY());

    auto r = te.getBounds();
    const int centeredY = (buttonsTop - r.getHeight()) / 2;
    r.setY (juce::jmax (minTop, centeredY));
    te.setBounds (r);
}

inline void focusAndSelectPromptTextEditor (juce::AlertWindow& aw, const juce::String& editorId)
{
    juce::Component::SafePointer<juce::AlertWindow> safeAw (&aw);
    juce::MessageManager::callAsync ([safeAw, editorId]()
    {
        if (safeAw == nullptr)
            return;

        auto* te = safeAw->getTextEditor (editorId);
        if (te == nullptr)
            return;

        if (te->isShowing() && te->isEnabled() && te->getPeer() != nullptr)
            te->grabKeyboardFocus();

        te->selectAll();
    });
}

inline void preparePromptTextEditor (juce::AlertWindow& aw,
                                     const juce::String& editorId,
                                     juce::Colour bg,
                                     juce::Colour text,
                                     juce::Colour accent,
                                     juce::Font baseFont,
                                     bool widenAndCenter,
                                     int minTop = kPromptEditorMinTopPx)
{
    if (auto* te = aw.getTextEditor (editorId))
    {
        stylePromptTextEditor (*te, bg, text, accent, baseFont, aw.getWidth(), widenAndCenter);
        centrePromptTextEditorVertically (aw, *te, minTop);
        focusAndSelectPromptTextEditor (aw, editorId);
    }
}

//======================================================================
//  Colour helpers
//======================================================================
inline juce::String colourToHexRgb (juce::Colour c)
{
    auto h2 = [] (juce::uint8 v)
    {
        return juce::String::toHexString ((int) v).paddedLeft ('0', 2).toUpperCase();
    };

    return "#" + h2 (c.getRed()) + h2 (c.getGreen()) + h2 (c.getBlue());
}

inline bool tryParseHexColour (juce::String text, juce::Colour& out)
{
    auto isHexDigitAscii = [] (juce::juce_wchar ch)
    {
        return (ch >= '0' && ch <= '9')
            || (ch >= 'A' && ch <= 'F')
            || (ch >= 'a' && ch <= 'f');
    };

    text = text.trim();
    if (text.startsWithChar ('#'))
        text = text.substring (1);

    if (text.length() != 6 && text.length() != 8)
        return false;

    for (int i = 0; i < text.length(); ++i)
        if (! isHexDigitAscii (text[i]))
            return false;

    if (text.length() == 6)
    {
        const auto r = (juce::uint8) text.substring (0, 2).getHexValue32();
        const auto g = (juce::uint8) text.substring (2, 4).getHexValue32();
        const auto b = (juce::uint8) text.substring (4, 6).getHexValue32();
        out = juce::Colour (r, g, b);
        return true;
    }

    const auto a = (juce::uint8) text.substring (0, 2).getHexValue32();
    const auto r = (juce::uint8) text.substring (2, 4).getHexValue32();
    const auto g = (juce::uint8) text.substring (4, 6).getHexValue32();
    const auto b = (juce::uint8) text.substring (6, 8).getHexValue32();
    out = juce::Colour (r, g, b).withAlpha ((float) a / 255.0f);
    return true;
}

inline void setPaletteSwatchColour (juce::TextButton& b, juce::Colour colour)
{
    b.setButtonText ("");
    b.setColour (juce::TextButton::buttonColourId, colour);
    b.setColour (juce::TextButton::buttonOnColourId, colour);
}

//======================================================================
//  Apply scheme colours to a LookAndFeel (calls setColour for all
//  standard widget colour IDs used by TR plugins).
//======================================================================
inline void applySchemeToLookAndFeel (juce::LookAndFeel_V4& lnf, const TRScheme& scheme)
{
    lnf.setColour (juce::TooltipWindow::backgroundColourId, scheme.bg);
    lnf.setColour (juce::TooltipWindow::textColourId,       scheme.text);
    lnf.setColour (juce::TooltipWindow::outlineColourId,    scheme.outline);

    lnf.setColour (juce::BubbleComponent::backgroundColourId, scheme.bg);
    lnf.setColour (juce::BubbleComponent::outlineColourId,    scheme.outline);

    lnf.setColour (juce::AlertWindow::backgroundColourId, scheme.bg);
    lnf.setColour (juce::AlertWindow::textColourId,       scheme.text);
    lnf.setColour (juce::AlertWindow::outlineColourId,    scheme.outline);

    lnf.setColour (juce::TextButton::buttonColourId,   scheme.bg);
    lnf.setColour (juce::TextButton::buttonOnColourId,  scheme.fg);
    lnf.setColour (juce::TextButton::textColourOffId,   scheme.text);
    lnf.setColour (juce::TextButton::textColourOnId,    scheme.bg);

    lnf.setColour (juce::Label::textColourId,       scheme.text);
    lnf.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);

    lnf.setColour (juce::ToggleButton::textColourId, scheme.text);
    lnf.setColour (juce::ToggleButton::tickColourId, scheme.text);
    lnf.setColour (juce::ToggleButton::tickDisabledColourId, scheme.text.withAlpha (0.4f));

    lnf.setColour (juce::Slider::textBoxTextColourId,    scheme.text);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    lnf.setColour (juce::ComboBox::backgroundColourId, scheme.bg);
    lnf.setColour (juce::ComboBox::textColourId,       scheme.text);
    lnf.setColour (juce::ComboBox::outlineColourId,    scheme.outline);

    lnf.setColour (juce::PopupMenu::backgroundColourId,            scheme.bg);
    lnf.setColour (juce::PopupMenu::textColourId,                  scheme.text);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, scheme.fg);
    lnf.setColour (juce::PopupMenu::highlightedTextColourId,       scheme.bg);

    lnf.setColour (juce::ScrollBar::thumbColourId,     scheme.text);
    lnf.setColour (juce::ScrollBar::trackColourId,     scheme.bg);
    lnf.setColour (juce::ScrollBar::backgroundColourId, scheme.bg);

    lnf.setColour (juce::ListBox::backgroundColourId, scheme.bg);
    lnf.setColour (juce::ListBox::textColourId,       scheme.text);
    lnf.setColour (juce::ListBox::outlineColourId,    scheme.outline);

    lnf.setColour (juce::TextEditor::backgroundColourId,       scheme.bg);
    lnf.setColour (juce::TextEditor::textColourId,             scheme.text);
    lnf.setColour (juce::TextEditor::highlightColourId,        scheme.text.withAlpha (0.3f));
    lnf.setColour (juce::TextEditor::highlightedTextColourId,  scheme.text);
    lnf.setColour (juce::TextEditor::outlineColourId,          scheme.outline);
    lnf.setColour (juce::TextEditor::focusedOutlineColourId,   scheme.text);
    lnf.setColour (juce::TextEditor::shadowColourId,           juce::Colours::transparentBlack);
}

//======================================================================
//  PromptOverlay — translucent backdrop for modal prompts
//======================================================================
class PromptOverlay : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.5f));
    }
};

//======================================================================
//  Input filters
//======================================================================

/// General-purpose numeric input filter with range, length, decimals,
/// sign control, and leading-zero rejection.
struct NumericInputFilter : juce::TextEditor::InputFilter
{
    double minVal, maxVal;
    int maxLen, maxDecimals;

    NumericInputFilter (double minV, double maxV, int maxLength, int maxDecs)
        : minVal (minV), maxVal (maxV), maxLen (maxLength), maxDecimals (maxDecs) {}

    juce::String filterNewText (juce::TextEditor& editor,
                                const juce::String& newText) override
    {
        const bool existingHasDot = editor.getText().containsChar ('.');
        bool seenDot = false;
        int decimals = 0;
        juce::String result;

        for (auto c : newText)
        {
            if (c == '.')
            {
                if (seenDot || existingHasDot || maxDecimals == 0)
                    continue;
                seenDot = true;
                result += c;
            }
            else if (juce::CharacterFunctions::isDigit (c))
            {
                if (seenDot) ++decimals;
                if (decimals > maxDecimals)
                    break;
                result += c;
            }
            else if ((c == '+' || c == '-') && result.isEmpty())
            {
                if (minVal >= 0.0)
                    continue;
                result += c;
            }

            if (maxLen > 0 && result.length() >= maxLen)
                break;
        }

        juce::String proposed = editor.getText();
        int insertPos = editor.getCaretPosition();
        proposed = proposed.substring (0, insertPos) + result
                 + proposed.substring (insertPos + editor.getHighlightedText().length());

        if (maxLen > 0 && proposed.length() > maxLen)
            return juce::String();

        // Reject leading zeros (e.g. "007" — only "0" or "0.x" are valid)
        juce::String stripped = proposed.trimStart();
        if (stripped.startsWithChar ('-') || stripped.startsWithChar ('+'))
            stripped = stripped.substring (1);
        if (stripped.length() > 1
            && stripped[0] == '0'
            && stripped[1] != '.')
            return juce::String();

        double val = proposed.replaceCharacter(',', '.').getDoubleValue();
        if (val > maxVal || val < minVal)
            return juce::String();

        return result;
    }
};

/// Hex colour input filter: optional '#' prefix + up to 6 hex digits.
struct HexInputFilter : juce::TextEditor::InputFilter
{
    juce::String filterNewText (juce::TextEditor& editor, const juce::String& newText) override
    {
        juce::String result;
        for (auto c : newText)
        {
            if (c == '#' && result.isEmpty())
            {
                result += c;
                continue;
            }
            if ((c >= '0' && c <= '9')
                || (c >= 'A' && c <= 'F')
                || (c >= 'a' && c <= 'f'))
            {
                result += c;
            }
        }

        juce::String proposed = editor.getText();
        int insertPos = editor.getCaretPosition();
        proposed = proposed.substring (0, insertPos) + result
                 + proposed.substring (insertPos + editor.getHighlightedText().length());

        juce::String hex = proposed;
        if (hex.startsWithChar ('#'))
            hex = hex.substring (1);
        if (hex.length() > 6)
            return juce::String();

        return result;
    }
};

/// Integer percentage filter: 0–100, no decimals, no leading zeros.
struct PctInputFilter : juce::TextEditor::InputFilter
{
    juce::String filterNewText (juce::TextEditor& editor, const juce::String& newText) override
    {
        juce::String result;
        for (auto c : newText)
        {
            if (juce::CharacterFunctions::isDigit (c))
                result += c;
            if (result.length() >= 3)
                break;
        }

        juce::String proposed = editor.getText();
        int insertPos = editor.getCaretPosition();
        proposed = proposed.substring (0, insertPos) + result
                 + proposed.substring (insertPos + editor.getHighlightedText().length());
        if (proposed.length() > 3 || proposed.getIntValue() > 100)
            return juce::String();

        if (proposed.length() > 1 && proposed[0] == '0')
            return juce::String();

        return result;
    }
};

} // namespace TR

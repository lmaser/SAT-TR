// PluginEditor.cpp - SAT-TR
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// ----------------------------------------------------------------
//  Timer & display constants
// ----------------------------------------------------------------
static constexpr int kCrtTimerHz   = 10;
static constexpr int kIdleTimerHz  = 4;
static constexpr float kSilenceDb  = -80.0f;

// ----------------------------------------------------------------
//  Parameter listener IDs
// ----------------------------------------------------------------
static constexpr std::array<const char*, 10> kUiMirrorParamIds {
	SATTRAudioProcessor::kParamUiPalette,
	SATTRAudioProcessor::kParamUiFxTail,
	SATTRAudioProcessor::kParamUiColor0,
	SATTRAudioProcessor::kParamUiColor1,
	SATTRAudioProcessor::kParamEnableA,
	SATTRAudioProcessor::kParamEnableB,
	SATTRAudioProcessor::kParamEnableC,
	SATTRAudioProcessor::kParamSatTypeA,
	SATTRAudioProcessor::kParamSatTypeB,
	SATTRAudioProcessor::kParamSatTypeC
};

// ----------------------------------------------------------------
//  Popup helper classes
// ----------------------------------------------------------------
namespace
{
	bool isGainFaderFloor (float dB) noexcept
	{
		return dB <= SATTRAudioProcessor::kGainFloorDb + 0.001f;
	}

	juce::String formatGainFaderDb (float dB)
	{
		if (isGainFaderFloor (dB))
			return "-INF dB";
		return juce::String (dB, 1) + " dB";
	}

	juce::String formatChaosTooltip (float amountPercent, float speedHz)
	{
		return "AMT " + juce::String (juce::roundToInt (juce::jlimit (0.0f, 100.0f, amountPercent))) + "%"
		     + " | SPD " + juce::String (juce::jlimit (SATTRAudioProcessor::kChaosSpdMin,
		                                               SATTRAudioProcessor::kChaosSpdMax,
		                                               speedHz), 1)
		     + " Hz";
	}

	juce::String formatFilterPromptFrequency (float hz)
	{
		if (hz >= 1000.0f)
			return juce::String (hz, 2);
		if (hz >= 100.0f)
			return juce::String (hz, 1);
		return juce::String (hz, 2);
	}

	juce::String formatFrequencyWithUnitForPrompt (float hz)
	{
		const float safeHz = juce::jlimit (20.0f, 20000.0f, hz);
		if (safeHz >= 1000.0f)
			return juce::String (safeHz / 1000.0f, 2) + " kHz";
		if (safeHz >= 100.0f)
			return juce::String (safeHz, 1) + " Hz";
		return juce::String (safeHz, 2) + " Hz";
	}

	juce::String formatSatDelayMsNumberForUi (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);

		if (safeMs >= 1000.0)
			return juce::String (safeMs / 1000.0, 2);
		if (safeMs >= 100.0)
			return juce::String (safeMs, 1);
		return juce::String (safeMs, 2);
	}

	juce::String formatSatDelayMsForUi (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);
		return formatSatDelayMsNumberForUi (safeMs) + (safeMs >= 1000.0 ? " s" : " ms");
	}

	juce::String formatTimeMsForPromptValue (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);

		if (safeMs >= 100.0)
			return juce::String (safeMs, 1);
		if (safeMs >= 1.0)
			return juce::String (safeMs, 2);
		return juce::String (safeMs, 3);
	}

	constexpr int kFooterMixValueWidthPx = 56;
	constexpr int kFooterDbValueWidthPx = 66;
	constexpr int kCompactLoaderColumnWidthPx = 360;
	constexpr int kCompactFixedHeightPx = 752;
	constexpr int kCompactMinVisibleLoaders = 1;
	constexpr int kCompactMaxVisibleLoaders = 3;
	constexpr int kCompactEdgeReserveWidthPx = 18;
	constexpr int kCompactEdgeReserveSlotPx = 32;
	constexpr int kCompactEdgeContentGapPx = 2;
	constexpr int kCompactLoaderTabYInsetPx = 86;
	constexpr int kCompactLoaderTabWidthPx = 34;
	constexpr int kCompactLoaderTabHeightPx = 52;
	constexpr int kCompactLoaderTabGapPx = 10;
	constexpr int kCompactLoaderTabSafeInsetRightPx = 10;
	constexpr int kCompactFooterRailSlotHeightPx = 34;
	constexpr int kCompactFooterRailHeightPx = 28;
	constexpr int kCompactFooterRailWidthPx = 132;
	constexpr int kCompactFooterRailXInsetPx = 18;
	constexpr int kCompactFooterPanelWidthPx = 500;

	constexpr int getCompactLoaderContentSideInsetPx() noexcept
	{
		// Keep the external compact width equal to the simple plugins while
		// reserving safe space for edge tabs inside each loader.
		return ((kCompactEdgeReserveSlotPx + kCompactEdgeReserveWidthPx) / 2) + kCompactEdgeContentGapPx;
	}

	juce::Rectangle<int> makeFooterValueArea (const juce::Rectangle<int>& barBounds, int valueWidthPx)
	{
		return { barBounds.getRight() + 4, barBounds.getY(), valueWidthPx, barBounds.getHeight() };
	}

	juce::Rectangle<int> makeExpandedFooterValueArea (int panelRight, const juce::Rectangle<int>& barBounds)
	{
		return { barBounds.getRight() + 10, barBounds.getY() - 2,
		         juce::jmax (0, panelRight - barBounds.getRight() - 10),
		         barBounds.getHeight() + 4 };
	}

	constexpr int satTypeModelToVisibleComboId (int modelIndex) noexcept
	{
		switch (modelIndex)
		{
			case 0: return 1;  // CLEAN
			case 1: return 2;  // TAPE
			case 2: return 3;  // TUBE
			case 3: return 4;  // TRANSISTOR
			case 4: return 5;  // DIODE
			case 5: return 6;  // CLIPPER
			default: return 1;
		}
	}

	constexpr int visibleComboIdToSatTypeModel (int comboId) noexcept
	{
		switch (comboId)
		{
			case 1: return 0; // CLEAN
			case 2: return 1; // TAPE
			case 3: return 2; // TUBE
			case 4: return 3; // TRANSISTOR
			case 5: return 4; // DIODE
			case 6: return 5; // CLIPPER
			default: return 0;
		}
	}

	float expRatioInternalToDisplay (float internalRatio) noexcept
	{
		return juce::jlimit (SATTRAudioProcessor::kExpRatioMin,
		                     SATTRAudioProcessor::kExpRatioMax,
		                     internalRatio);
	}

	float expRatioDisplayToInternal (float displayRatio) noexcept
	{
		return juce::jlimit (SATTRAudioProcessor::kExpRatioMin,
		                     SATTRAudioProcessor::kExpRatioMax,
		                     displayRatio);
	}

	float expRatioDisplayToNorm (float displayRatio) noexcept
	{
		const float clamped = expRatioInternalToDisplay (displayRatio);
		const float minLog = std::log10 (SATTRAudioProcessor::kExpRatioMin);
		const float maxLog = std::log10 (SATTRAudioProcessor::kExpRatioMax);
		return juce::jlimit (0.0f, 1.0f, (std::log10 (clamped) - minLog) / (maxLog - minLog));
	}

	float expRatioNormToDisplay (float norm) noexcept
	{
		const float minLog = std::log10 (SATTRAudioProcessor::kExpRatioMin);
		const float maxLog = std::log10 (SATTRAudioProcessor::kExpRatioMax);
		return std::pow (10.0f, minLog + juce::jlimit (0.0f, 1.0f, norm) * (maxLog - minLog));
	}

	juce::String formatExpRatioDisplay (float internalRatio, int decimals = 1)
	{
		return juce::String (expRatioInternalToDisplay (internalRatio), decimals);
	}

	juce::String formatExpTooltip (bool post, float ratio)
	{
		return juce::String (post ? "POST" : "PRE")
		     + " | 1:" + formatExpRatioDisplay (ratio);
	}

	juce::String filterSlopeToText (int slope)
	{
		if (slope <= 0) return "6dB";
		if (slope == 1) return "12dB";
		return "24dB";
	}

	struct PopupSwatchButton final : public juce::TextButton
	{
		std::function<void()> onLeftClick;
		std::function<void()> onRightClick;

		void clicked() override
		{
			if (onLeftClick)
				onLeftClick();
			else
				juce::TextButton::clicked();
		}

		void mouseUp (const juce::MouseEvent& e) override
		{
			if (e.mods.isPopupMenu())
			{
				if (onRightClick)
					onRightClick();
				return;
			}

			juce::TextButton::mouseUp (e);
		}
	};

	struct PopupClickableLabel final : public juce::Label
	{
		using juce::Label::Label;
		std::function<void()> onClick;

		void mouseUp (const juce::MouseEvent& e) override
		{
			juce::Label::mouseUp (e);
			if (! e.mods.isPopupMenu() && onClick)
				onClick();
		}
	};

}

// ----------------------------------------------------------------
//  Popup static layout helpers
// ----------------------------------------------------------------
static void syncGraphicsPopupState (juce::AlertWindow& aw,
                                    const std::array<juce::Colour, 2>& defaultPalette,
                                    const std::array<juce::Colour, 2>& customPalette,
                                    bool useCustomPalette)
{
	if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle")))
		t->setToggleState (! useCustomPalette, juce::dontSendNotification);
	if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle")))
		t->setToggleState (useCustomPalette, juce::dontSendNotification);

	for (int i = 0; i < 2; ++i)
	{
		if (auto* dflt = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("defaultSwatch" + juce::String (i))))
			setPaletteSwatchColour (*dflt, defaultPalette[(size_t) i]);
		if (auto* custom = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("customSwatch" + juce::String (i))))
		{
			setPaletteSwatchColour (*custom, customPalette[(size_t) i]);
			custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
		}
	}

	auto applyLabelTextColourTo = [] (juce::Label* lbl, juce::Colour col)
	{
		if (lbl != nullptr)
			lbl->setColour (juce::Label::textColourId, col);
	};

	const juce::Colour activeText = useCustomPalette ? customPalette[0] : defaultPalette[0];
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel")), activeText);
}

static void layoutGraphicsPopupContent (juce::AlertWindow& aw)
{
	layoutAlertWindowButtons (aw);

	auto snapEven = [] (int v) { return v & ~1; };

	const int contentLeft = kPromptInnerMargin;
	const int contentRight = aw.getWidth() - kPromptInnerMargin;
	const int contentW = juce::jmax (0, contentRight - contentLeft);

	auto* dfltToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle"));
	auto* dfltLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel"));
	auto* customToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle"));
	auto* customLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel"));
	auto* paletteTitle = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle"));
	auto* fxToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("fxToggle"));
	auto* fxLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel"));
	auto* okBtn = aw.getNumButtons() > 0 ? aw.getButton (0) : nullptr;

	constexpr int toggleBox = GraphicsPromptLayout::toggleBox;
	constexpr int toggleGap = 4;
	constexpr int toggleVisualInsetLeft = 2;
	constexpr int swatchSize = GraphicsPromptLayout::swatchSize;
	constexpr int swatchGap = GraphicsPromptLayout::swatchGap;
	constexpr int columnGap = GraphicsPromptLayout::columnGap;
	constexpr int titleH = GraphicsPromptLayout::titleHeight;

	const int toggleVisualSide = juce::jlimit (14, juce::jmax (14, toggleBox - 2), (int) std::lround ((double) toggleBox * 0.65));

	const int swatchW = swatchSize;
	const int swatchH = (2 * swatchSize) + swatchGap;
	const int swatchGroupSize = (2 * swatchW) + swatchGap;
	const int swatchesH = swatchH;
	const int modeH = toggleBox;

	const int baseGap1 = GraphicsPromptLayout::titleToModeGap;
	const int baseGap2 = GraphicsPromptLayout::modeToSwatchesGap;

	const int titleY = snapEven (kPromptFooterBottomPad);
	const int footerY = getAlertButtonsTop (aw);

	const int bodyH = modeH + baseGap2 + swatchesH;
	const int bodyZoneTop = titleY + titleH + baseGap1;
	const int bodyZoneBottom = footerY - baseGap1;
	const int bodyZoneH = juce::jmax (0, bodyZoneBottom - bodyZoneTop);
	const int bodyY = snapEven (bodyZoneTop + juce::jmax (0, (bodyZoneH - bodyH) / 2));

	const int modeY = bodyY;
	const int blocksY = snapEven (modeY + modeH + baseGap2);

	const int dfltLabelW = (dfltLabel != nullptr) ? juce::jmax (38, stringWidth (dfltLabel->getFont(), "DFLT") + 2) : 40;
	const int customLabelW = (customLabel != nullptr) ? juce::jmax (38, stringWidth (customLabel->getFont(), "CSTM") + 2) : 40;
	const int fxLabelW = (fxLabel != nullptr)
	                   ? juce::jmax (90, stringWidth (fxLabel->getFont(), fxLabel->getText().toUpperCase()) + 2)
	                   : 96;

	const int toggleLabelStartOffset = toggleVisualInsetLeft + toggleVisualSide + toggleGap;
	const int dfltRowW = toggleLabelStartOffset + dfltLabelW;
	const int customRowW = toggleLabelStartOffset + customLabelW;
	const int fxRowW = toggleLabelStartOffset + fxLabelW;
	const int okBtnW = (okBtn != nullptr) ? okBtn->getWidth() : 96;

	const int leftColumnW = juce::jmax (swatchGroupSize, juce::jmax (dfltRowW, fxRowW));
	const int rightColumnW = juce::jmax (swatchGroupSize, juce::jmax (customRowW, okBtnW));
	const int columnsRowW = leftColumnW + columnGap + rightColumnW;
	const int columnsX = snapEven (contentLeft + juce::jmax (0, (contentW - columnsRowW) / 2));
	const int col0X = columnsX;
	const int col1X = columnsX + leftColumnW + columnGap;

	const int dfltX = col0X;
	const int customX = col1X;

	const int defaultSwatchStartX = col0X;
	const int customSwatchStartX = col1X;

	if (paletteTitle != nullptr)
	{
		const int paletteW = juce::jmax (100, juce::jmin (leftColumnW, contentRight - col0X));
		paletteTitle->setBounds (col0X, titleY, paletteW, titleH);
	}

	if (dfltToggle != nullptr)   dfltToggle->setBounds (dfltX, modeY, toggleBox, toggleBox);
	if (dfltLabel != nullptr)    dfltLabel->setBounds (dfltX + toggleLabelStartOffset, modeY, dfltLabelW, toggleBox);
	if (customToggle != nullptr) customToggle->setBounds (customX, modeY, toggleBox, toggleBox);
	if (customLabel != nullptr)  customLabel->setBounds (customX + toggleLabelStartOffset, modeY, customLabelW, toggleBox);

	auto placeSwatchGroup = [&] (const juce::String& prefix, int startX)
	{
		const int startY = blocksY;
		for (int i = 0; i < 2; ++i)
		{
			if (auto* b = dynamic_cast<juce::TextButton*> (aw.findChildWithID (prefix + juce::String (i))))
			{
				b->setBounds (startX + i * (swatchW + swatchGap), startY, swatchW, swatchH);
			}
		}
	};

	placeSwatchGroup ("defaultSwatch", defaultSwatchStartX);
	placeSwatchGroup ("customSwatch", customSwatchStartX);

	if (okBtn != nullptr)
	{
		auto okR = okBtn->getBounds();
		okR.setX (col1X);
		okR.setY (footerY);
		okBtn->setBounds (okR);

		const int fxY = snapEven (footerY + juce::jmax (0, (okR.getHeight() - toggleBox) / 2));
		const int fxX = col0X;
		if (fxToggle != nullptr) fxToggle->setBounds (fxX, fxY, toggleBox, toggleBox);
		if (fxLabel != nullptr)  fxLabel->setBounds (fxX + toggleLabelStartOffset, fxY, fxLabelW, toggleBox);
	}

	auto updateVisualBounds = [] (juce::Component* c, int& minX, int& maxR)
	{
		if (c == nullptr)
			return;
		const auto r = c->getBounds();
		minX = juce::jmin (minX, r.getX());
		maxR = juce::jmax (maxR, r.getRight());
	};

	int visualMinX = aw.getWidth();
	int visualMaxR = 0;

	updateVisualBounds (paletteTitle, visualMinX, visualMaxR);
	updateVisualBounds (dfltToggle, visualMinX, visualMaxR);
	updateVisualBounds (dfltLabel, visualMinX, visualMaxR);
	updateVisualBounds (customToggle, visualMinX, visualMaxR);
	updateVisualBounds (customLabel, visualMinX, visualMaxR);
	updateVisualBounds (fxToggle, visualMinX, visualMaxR);
	updateVisualBounds (fxLabel, visualMinX, visualMaxR);
	updateVisualBounds (okBtn, visualMinX, visualMaxR);

	for (int i = 0; i < 2; ++i)
	{
		updateVisualBounds (aw.findChildWithID ("defaultSwatch" + juce::String (i)), visualMinX, visualMaxR);
		updateVisualBounds (aw.findChildWithID ("customSwatch" + juce::String (i)), visualMinX, visualMaxR);
	}

	if (visualMaxR > visualMinX)
	{
		const int leftMarginToPrompt = visualMinX;
		const int rightMarginToPrompt = aw.getWidth() - visualMaxR;

		int dx = (rightMarginToPrompt - leftMarginToPrompt) / 2;

		const int minDx = contentLeft - visualMinX;
		const int maxDx = contentRight - visualMaxR;
		dx = juce::jlimit (minDx, maxDx, dx);

		if (dx != 0)
		{
			auto shiftX = [dx] (juce::Component* c)
			{
				if (c == nullptr)
					return;
				auto r = c->getBounds();
				r.setX (r.getX() + dx);
				c->setBounds (r);
			};

			shiftX (paletteTitle);
			shiftX (dfltToggle);
			shiftX (dfltLabel);
			shiftX (customToggle);
			shiftX (customLabel);
			shiftX (fxToggle);
			shiftX (fxLabel);
			shiftX (okBtn);

			for (int i = 0; i < 2; ++i)
			{
				shiftX (aw.findChildWithID ("defaultSwatch" + juce::String (i)));
				shiftX (aw.findChildWithID ("customSwatch" + juce::String (i)));
			}
		}
	}
}

static void layoutInfoPopupContent (juce::AlertWindow& aw)
{
	layoutAlertWindowButtons (aw);

	const int contentTop = kPromptBodyTopPad;
	const int contentBottom = getAlertButtonsTop (aw) - kPromptBodyBottomPad;
	const int contentH = juce::jmax (0, contentBottom - contentTop);
	const int bodyW = aw.getWidth() - (2 * kPromptInnerMargin);

	auto* viewport = dynamic_cast<juce::Viewport*> (aw.findChildWithID ("bodyViewport"));
	if (viewport == nullptr)
		return;

	viewport->setBounds (kPromptInnerMargin, contentTop, bodyW, contentH);

	auto* content = viewport->getViewedComponent();
	if (content == nullptr)
		return;

	constexpr int kItemGap = 10;
	constexpr int kBodyInsetX = 5;
	int y = 0;
	const int innerW = juce::jmax (0, bodyW - kBodyInsetX * 2);

	for (int i = 0; i < content->getNumChildComponents(); ++i)
	{
		auto* child = content->getChildComponent (i);
		if (child == nullptr || ! child->isVisible())
			continue;

		int itemH = 30;
		if (auto* label = dynamic_cast<juce::Label*> (child))
		{
			auto font = label->getFont();
			const auto text = label->getText();
			const auto border = label->getBorderSize();

			if (! text.containsChar ('\n'))
			{
				itemH = (int) std::ceil (font.getHeight()) + border.getTopAndBottom();
			}
			else
			{
				juce::AttributedString as;
				as.append (text, font, label->findColour (juce::Label::textColourId));
				as.setJustification (label->getJustificationType());
				juce::TextLayout layout;
				const int textAreaW = innerW - border.getLeftAndRight();
				layout.createLayout (as, (float) juce::jmax (1, textAreaW));
				itemH = juce::jmax (20, (int) std::ceil (layout.getHeight() + font.getDescent())
				                        + border.getTopAndBottom() + 4);
			}
		}
		else if (dynamic_cast<juce::HyperlinkButton*> (child) != nullptr)
		{
			itemH = 28;
		}

		child->setBounds (kBodyInsetX, y, innerW, itemH);

		if (auto* label = dynamic_cast<juce::Label*> (child))
		{
			const auto& props = label->getProperties();
			if (props.contains ("poemPadFraction"))
			{
				const float padFrac = (float) props["poemPadFraction"];
				const int padPx = juce::jmax (4, (int) std::round (innerW * padFrac));
				label->setBorderSize (juce::BorderSize<int> (0, padPx, 0, padPx));

				auto font = label->getFont();
				const int textAreaW = innerW - 2 * padPx;
				for (float scale = 1.0f; scale >= 0.65f; scale -= 0.025f)
				{
					font.setHorizontalScale (scale);
					juce::GlyphArrangement glyphs;
					glyphs.addLineOfText (font, label->getText(), 0.0f, 0.0f);
					if (static_cast<int> (std::ceil (glyphs.getBoundingBox (0, -1, false).getWidth())) <= textAreaW)
						break;
				}
				label->setFont (font);
			}
		}

		y += itemH + kItemGap;
	}

	if (y > kItemGap)
		y -= kItemGap;

	content->setSize (bodyW, juce::jmax (contentH, y));
}

// ----------------------------------------------------------------
//  BarSlider::getTextFromValue
// ----------------------------------------------------------------
juce::String SATTRAudioProcessorEditor::BarSlider::getTextFromValue (double v)
{
	if (owner == nullptr)
		return juce::Slider::getTextFromValue (v);

	switch (type_)
	{
		case Type::HpFreq:
		case Type::LpFreq:
			return juce::String (v, 1) + " Hz";

		case Type::Input:
			return formatGainFaderDb ((float) v);

		case Type::Output:
		case Type::GlobalOutput:
			return formatGainFaderDb ((float) v);

		case Type::LimThreshold:
			return juce::String (v, 1) + " dB";

		case Type::Tilt:
			return juce::String (v, 1) + " dB";

		case Type::Series:
			return juce::String (static_cast<int> (std::round (v))) + "x";

		case Type::Detail:
		case Type::Instability:
			return juce::String (juce::roundToInt (v * 100.0)) + "%";

		case Type::Delay:
			return formatSatDelayMsForUi (v);

		case Type::Pan:
		{
			double percent = v * 100.0;
			if (std::abs (percent - 50.0) < 1.0)
				return "C";
			if (percent < 50.0)
				return "L" + juce::String (50.0 - percent, 0);
			return "R" + juce::String (percent - 50.0, 0);
		}

		case Type::Fred:
		case Type::Pos:
		case Type::Mix:
		case Type::GlobalMix:
			return juce::String (juce::roundToInt (v * 100.0)) + "%";

		case Type::SatDrive:
		case Type::SatGirth:
		case Type::SatMod:
		case Type::SatSag:
			return juce::String (juce::roundToInt (v * 100.0)) + "%";

		case Type::SatBias:
			return juce::String (juce::roundToInt (v * 100.0)) + "%";

		default:
			break;
	}

	return juce::Slider::getTextFromValue (v);
}

// ----------------------------------------------------------------
//  FilterBarComponent implementations
// ----------------------------------------------------------------
juce::Rectangle<float> SATTRAudioProcessorEditor::FilterBarComponent::getInnerArea() const
{
	return getLocalBounds().toFloat().reduced (kPad);
}

float SATTRAudioProcessorEditor::FilterBarComponent::freqToNormX (float freq) const
{
	const float clamped = juce::jlimit (kMinFreq, kMaxFreq, freq);
	return std::log2 (clamped / kMinFreq) / std::log2 (kMaxFreq / kMinFreq);
}

float SATTRAudioProcessorEditor::FilterBarComponent::normXToFreq (float normX) const
{
	const float n = juce::jlimit (0.0f, 1.0f, normX);
	return kMinFreq * std::pow (2.0f, n * std::log2 (kMaxFreq / kMinFreq));
}

float SATTRAudioProcessorEditor::FilterBarComponent::getMarkerScreenX (float freq) const
{
	const auto inner = getInnerArea();
	return inner.getX() + freqToNormX (freq) * inner.getWidth();
}

SATTRAudioProcessorEditor::FilterBarComponent::DragTarget
SATTRAudioProcessorEditor::FilterBarComponent::hitTestMarker (juce::Point<float> p) const
{
	const float hpX = getMarkerScreenX (hpFreq_);
	const float lpX = getMarkerScreenX (lpFreq_);
	const float distHp = std::abs (p.x - hpX);
	const float distLp = std::abs (p.x - lpX);

	if (distHp <= kMarkerHitPx && distHp <= distLp)
		return HP;
	if (distLp <= kMarkerHitPx)
		return LP;
	if (distHp <= kMarkerHitPx)
		return HP;

	return None;
}

void SATTRAudioProcessorEditor::FilterBarComponent::setFreqFromMouseX (float mouseX, DragTarget target)
{
	if (owner == nullptr || target == None)
		return;

	const auto inner = getInnerArea();
	const float normX = (inner.getWidth() > 0.0f) ? (mouseX - inner.getX()) / inner.getWidth() : 0.0f;
	float freq = normXToFreq (normX);

	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };
	const char* hpId = pick (SATTRAudioProcessor::kParamHpFreqA, SATTRAudioProcessor::kParamHpFreqB, SATTRAudioProcessor::kParamHpFreqC);
	const char* lpId = pick (SATTRAudioProcessor::kParamLpFreqA, SATTRAudioProcessor::kParamLpFreqB, SATTRAudioProcessor::kParamLpFreqC);

	// Clamp so HP never exceeds LP and vice-versa
	if (target == HP)
	{
		const float otherFreq = proc.getValueTreeState().getRawParameterValue (lpId)->load();
		freq = juce::jmin (freq, otherFreq);
	}
	else
	{
		const float otherFreq = proc.getValueTreeState().getRawParameterValue (hpId)->load();
		freq = juce::jmax (freq, otherFreq);
	}

	const char* paramId = (target == HP) ? hpId : lpId;
	if (auto* param = proc.getValueTreeState().getParameter (paramId))
		param->setValueNotifyingHost (param->convertTo0to1 (freq));
}

void SATTRAudioProcessorEditor::FilterBarComponent::updateTooltipForTarget (DragTarget target)
{
	if (target == HP)
	{
		const int hz = juce::roundToInt (hpFreq_);
		setTooltip ("HP " + juce::String (hz) + " Hz");
	}
	else if (target == LP)
	{
		const int hz = juce::roundToInt (lpFreq_);
		setTooltip ("LP " + juce::String (hz) + " Hz");
	}
	else
	{
		setTooltip ({});
	}
}

void SATTRAudioProcessorEditor::FilterBarComponent::updateFromProcessor()
{
	if (owner == nullptr) return;
	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };
	const char* hpId   = pick (SATTRAudioProcessor::kParamHpFreqA, SATTRAudioProcessor::kParamHpFreqB, SATTRAudioProcessor::kParamHpFreqC);
	const char* lpId   = pick (SATTRAudioProcessor::kParamLpFreqA, SATTRAudioProcessor::kParamLpFreqB, SATTRAudioProcessor::kParamLpFreqC);
	const char* hpOnId = pick (SATTRAudioProcessor::kParamHpOnA,   SATTRAudioProcessor::kParamHpOnB,   SATTRAudioProcessor::kParamHpOnC);
	const char* lpOnId = pick (SATTRAudioProcessor::kParamLpOnA,   SATTRAudioProcessor::kParamLpOnB,   SATTRAudioProcessor::kParamLpOnC);

	const float newHpFreq = proc.getValueTreeState().getRawParameterValue (hpId)->load();
	const float newLpFreq = proc.getValueTreeState().getRawParameterValue (lpId)->load();
	const bool  newHpOn   = proc.getValueTreeState().getRawParameterValue (hpOnId)->load() > 0.5f;
	const bool  newLpOn   = proc.getValueTreeState().getRawParameterValue (lpOnId)->load() > 0.5f;

	if (newHpFreq == hpFreq_ && newLpFreq == lpFreq_ && newHpOn == hpOn_ && newLpOn == lpOn_)
		return;

	hpFreq_ = newHpFreq;
	lpFreq_ = newLpFreq;
	hpOn_   = newHpOn;
	lpOn_   = newLpOn;
	repaint();
}

void SATTRAudioProcessorEditor::FilterBarComponent::paint (juce::Graphics& g)
{
	const auto r = getLocalBounds().toFloat();

	// Outline
	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);

	// Background
	const auto inner = getInnerArea();
	g.setColour (scheme.bg);
	g.fillRect (inner);

	// Pass-band fill (between HP and LP)
	if (hpOn_ || lpOn_)
	{
		const float hpX = hpOn_ ? getMarkerScreenX (hpFreq_) : inner.getX();
		const float lpX = lpOn_ ? getMarkerScreenX (lpFreq_) : inner.getRight();

		if (lpX > hpX)
		{
			const auto band = juce::Rectangle<float> (hpX, inner.getY(), lpX - hpX, inner.getHeight());
			g.setColour (scheme.fg.withAlpha (0.12f));
			g.fillRect (band.getIntersection (inner));
		}
	}

	// HP marker
	{
		const float mx = getMarkerScreenX (hpFreq_);
		if (mx >= inner.getX() && mx <= inner.getRight())
		{
			const float alpha = hpOn_ ? 1.0f : 0.25f;
			g.setColour (scheme.fg.withAlpha (alpha));
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}

	// LP marker
	{
		const float mx = getMarkerScreenX (lpFreq_);
		if (mx >= inner.getX() && mx <= inner.getRight())
		{
			const float alpha = lpOn_ ? 1.0f : 0.25f;
			g.setColour (scheme.fg.withAlpha (alpha));
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}
}

void SATTRAudioProcessorEditor::FilterBarComponent::mouseDown (const juce::MouseEvent& e)
{
	if (e.mods.isPopupMenu())
	{
		if (owner != nullptr)
			owner->openFilterPrompt (loaderIndex_);
		return;
	}

	currentDrag_ = hitTestMarker (e.position);
	if (currentDrag_ != None)
	{
		setFreqFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
		updateTooltipForTarget (currentDrag_);
	}
}

void SATTRAudioProcessorEditor::FilterBarComponent::mouseDrag (const juce::MouseEvent& e)
{
	if (currentDrag_ != None)
	{
		setFreqFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
		updateTooltipForTarget (currentDrag_);
	}
}

void SATTRAudioProcessorEditor::FilterBarComponent::mouseUp (const juce::MouseEvent&)
{
	currentDrag_ = None;
}

void SATTRAudioProcessorEditor::FilterBarComponent::mouseMove (const juce::MouseEvent& e)
{
	updateTooltipForTarget (hitTestMarker (e.position));
}

void SATTRAudioProcessorEditor::FilterBarComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
	if (owner == nullptr) return;
	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };

	const auto target = hitTestMarker (e.position);
	if (target == HP)
	{
		const char* paramId = pick (SATTRAudioProcessor::kParamHpOnA, SATTRAudioProcessor::kParamHpOnB, SATTRAudioProcessor::kParamHpOnC);
		if (auto* param = proc.getValueTreeState().getParameter (paramId))
		{
			const bool current = param->getValue() > 0.5f;
			param->setValueNotifyingHost (current ? 0.0f : 1.0f);
		}
	}
	else if (target == LP)
	{
		const char* paramId = pick (SATTRAudioProcessor::kParamLpOnA, SATTRAudioProcessor::kParamLpOnB, SATTRAudioProcessor::kParamLpOnC);
		if (auto* param = proc.getValueTreeState().getParameter (paramId))
		{
			const bool current = param->getValue() > 0.5f;
			param->setValueNotifyingHost (current ? 0.0f : 1.0f);
		}
	}
	else
	{
		owner->openFilterPrompt (loaderIndex_);
	}
}

// ----------------------------------------------------------------
//  DualMixBarComponent implementations
// ----------------------------------------------------------------
juce::Rectangle<float> SATTRAudioProcessorEditor::DualMixBarComponent::getInnerArea() const
{
	return getLocalBounds().toFloat().reduced (kPad);
}

SATTRAudioProcessorEditor::DualMixBarComponent::DragTarget
SATTRAudioProcessorEditor::DualMixBarComponent::hitTestMarker (juce::Point<float> p) const
{
	const auto inner = getInnerArea();
	const float halfW = inner.getWidth() * 0.5f;
	const float midX  = inner.getX() + halfW;
	return (p.x < midX) ? DRY : WET;
}

void SATTRAudioProcessorEditor::DualMixBarComponent::setLevelFromMouseX (float mouseX, DragTarget target)
{
	if (owner == nullptr || target == None)
		return;
	const auto inner = getInnerArea();
	const float halfW = inner.getWidth() * 0.5f;
	float level;
	if (target == DRY)
		level = (halfW > 0.0f) ? juce::jlimit (0.0f, 1.0f, (mouseX - inner.getX()) / halfW) : 0.0f;
	else
		level = (halfW > 0.0f) ? juce::jlimit (0.0f, 1.0f, (mouseX - (inner.getX() + halfW)) / halfW) : 0.0f;

	const char* paramId = (target == DRY) ? SATTRAudioProcessor::kParamDryLevel
	                                      : SATTRAudioProcessor::kParamWetLevel;
	auto& proc = owner->audioProcessor;
	if (auto* param = proc.getValueTreeState().getParameter (paramId))
		param->setValueNotifyingHost (level);
}

void SATTRAudioProcessorEditor::DualMixBarComponent::updateTooltipForTarget (DragTarget target)
{
	if (target == None)
	{
		setTooltip ({});
		return;
	}

	const float level = (target == DRY) ? dryLevel_ : wetLevel_;
	const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
	const juce::String label = (target == DRY) ? "DRY" : "WET";
	if (dB <= -100.0f)
		setTooltip (label + " -INF dB");
	else if (std::abs (dB) < 0.05f)
		setTooltip (label + " 0.0 dB");
	else
		setTooltip (label + " " + juce::String (dB, 1) + " dB");
}

void SATTRAudioProcessorEditor::DualMixBarComponent::updateFromProcessor()
{
	if (owner == nullptr) return;
	auto& proc = owner->audioProcessor;
	const float newDry = proc.getValueTreeState().getRawParameterValue (SATTRAudioProcessor::kParamDryLevel)->load();
	const float newWet = proc.getValueTreeState().getRawParameterValue (SATTRAudioProcessor::kParamWetLevel)->load();
	if (newDry == dryLevel_ && newWet == wetLevel_)
		return;
	dryLevel_ = newDry;
	wetLevel_ = newWet;
	repaint();
}

void SATTRAudioProcessorEditor::DualMixBarComponent::paint (juce::Graphics& g)
{
	const auto r = getLocalBounds().toFloat();
	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);
	const auto inner = getInnerArea();
	g.setColour (scheme.bg);
	g.fillRect (inner);
	const float halfW = inner.getWidth() * 0.5f;
	const float divX  = inner.getX() + halfW;

	g.setColour (scheme.fg.withAlpha (0.25f));
	g.drawVerticalLine ((int) divX, inner.getY(), inner.getBottom());

	{
		const float fillW = dryLevel_ * halfW;
		g.setColour (scheme.fg.withAlpha (0.18f));
		g.fillRect (juce::Rectangle<float> (inner.getX(), inner.getY(), fillW, inner.getHeight())
		             .getIntersection (inner));
	}
	{
		const float fillW = wetLevel_ * halfW;
		g.setColour (scheme.fg.withAlpha (0.35f));
		g.fillRect (juce::Rectangle<float> (divX, inner.getY(), fillW, inner.getHeight())
		             .getIntersection (inner));
	}
	{
		const float mx = inner.getX() + dryLevel_ * halfW;
		if (mx >= inner.getX() && mx <= divX)
		{
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.setColour (scheme.fg.withAlpha (0.7f));
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}
	{
		const float mx = divX + wetLevel_ * halfW;
		if (mx >= divX && mx <= inner.getRight())
		{
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.setColour (scheme.fg);
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}
}

void SATTRAudioProcessorEditor::DualMixBarComponent::mouseDown (const juce::MouseEvent& e)
{
	if (e.mods.isPopupMenu())
	{
		if (owner != nullptr)
			owner->openMixSendPrompt();
		return;
	}
	currentDrag_ = hitTestMarker (e.position);
	if (currentDrag_ != None)
	{
		lastTouched_ = currentDrag_;
		setLevelFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
		updateTooltipForTarget (currentDrag_);
		if (owner != nullptr)
		{
			owner->legendDirty = true;
			owner->repaint();
		}
	}
}

void SATTRAudioProcessorEditor::DualMixBarComponent::mouseDrag (const juce::MouseEvent& e)
{
	if (currentDrag_ != None)
	{
		setLevelFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
		updateTooltipForTarget (currentDrag_);
		if (owner != nullptr)
		{
			owner->legendDirty = true;
			owner->repaint();
		}
	}
}

void SATTRAudioProcessorEditor::DualMixBarComponent::mouseUp (const juce::MouseEvent&)
{
	currentDrag_ = None;
}

void SATTRAudioProcessorEditor::DualMixBarComponent::mouseMove (const juce::MouseEvent& e)
{
	updateTooltipForTarget (hitTestMarker (e.position));
}

// ----------------------------------------------------------------
//  LookAndFeel implementations
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::MinimalLNF::drawLinearSlider (
	juce::Graphics& g, int x, int y, int width, int height,
	float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
	const juce::Slider::SliderStyle /*style*/, juce::Slider& /*slider*/)
{
	const juce::Rectangle<float> r ((float) x, (float) y, (float) width, (float) height);

	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);

	const float pad = 7.0f;
	auto inner = r.reduced (pad);

	g.setColour (scheme.bg);
	g.fillRect (inner);

	const float fillW = juce::jlimit (0.0f, inner.getWidth(), sliderPos - inner.getX());
	auto fill = inner.withWidth (fillW);

	g.setColour (scheme.fg);
	g.fillRect (fill);
}

void SATTRAudioProcessorEditor::MinimalLNF::drawTickBox (
	juce::Graphics& g, juce::Component& button,
	float x, float y, float w, float h,
	bool ticked, bool /*isEnabled*/, bool /*highlighted*/, bool /*down*/)
{
	juce::ignoreUnused (x, y, w, h);

	const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
	const float side = juce::jlimit (14.0f,
	                                 juce::jmax (14.0f, local.getHeight() - 2.0f),
	                                 std::round (local.getHeight() * 0.65f));

	auto r = juce::Rectangle<float> (local.getX() + 2.0f,
	                                 local.getCentreY() - (side * 0.5f),
	                                 side, side).getIntersection (local);

	if (ticked)
	{
		g.setColour (scheme.outline);
		g.fillRect (r);
	}
	else
	{
		g.setColour (scheme.outline);
		g.drawRect (r, 4.0f);

		const float innerInset = juce::jlimit (1.0f, side * 0.45f, side * UiMetrics::tickBoxInnerInsetRatio);
		auto inner = r.reduced (innerInset);
		g.setColour (scheme.bg);
		g.fillRect (inner);
	}
}

void SATTRAudioProcessorEditor::MinimalLNF::drawToggleButton (
	juce::Graphics& g, juce::ToggleButton& button,
	bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
	const float side = juce::jlimit (14.0f,
	                                 juce::jmax (14.0f, local.getHeight() - 2.0f),
	                                 std::round (local.getHeight() * 0.65f));

	drawTickBox (g, button, 0, 0, 0, 0,
	             button.getToggleState(), button.isEnabled(),
	             shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

	const float textX = local.getX() + 2.0f + side + 2.0f;
	auto textArea = button.getLocalBounds().toFloat();
	textArea.removeFromLeft (textX);

	g.setColour (button.findColour (juce::ToggleButton::textColourId));

	float fontSize = juce::jlimit (12.0f, 40.0f, (float) button.getHeight() - 6.0f);

	// Shrink font if text would overflow available width
	const auto text = button.getButtonText();
	const float availW = textArea.getWidth();
	if (availW > 0)
	{
		juce::Font testFont (juce::FontOptions (fontSize).withStyle ("Bold"));
		juce::GlyphArrangement ga;
		ga.addLineOfText (testFont, text, 0.0f, 0.0f);
		const float neededW = ga.getBoundingBox (0, -1, false).getWidth();
		if (neededW > availW)
			fontSize = juce::jmax (8.0f, fontSize * (availW / neededW));
	}

	g.setFont (juce::Font (juce::FontOptions (fontSize).withStyle ("Bold")));

	g.drawText (text, textArea,
	            juce::Justification::centredLeft, false);
}

void SATTRAudioProcessorEditor::MinimalLNF::drawButtonBackground (
	juce::Graphics& g, juce::Button& button,
	const juce::Colour& backgroundColour,
	bool shouldDrawButtonAsHighlighted,
	bool shouldDrawButtonAsDown)
{
	auto r = button.getLocalBounds();

	auto fill = backgroundColour;
	if (shouldDrawButtonAsDown)
		fill = fill.brighter (0.12f);
	else if (shouldDrawButtonAsHighlighted)
		fill = fill.brighter (0.06f);

	g.setColour (fill);
	g.fillRect (r);

	g.setColour (scheme.outline);
	g.drawRect (r.reduced (1), 3);
}

void SATTRAudioProcessorEditor::MinimalLNF::drawComboBox (
	juce::Graphics& g, int width, int height,
	bool /*isButtonDown*/, int /*buttonX*/, int /*buttonY*/,
	int /*buttonW*/, int /*buttonH*/, juce::ComboBox& /*box*/)
{
	const juce::Rectangle<int> r (0, 0, width, height);

	g.setColour (scheme.bg);
	g.fillRect (r);

	g.setColour (scheme.outline);
	g.drawRect (r, 3);
}

void SATTRAudioProcessorEditor::MinimalLNF::drawPopupMenuBackground (
	juce::Graphics& g, int width, int height)
{
	g.fillAll (scheme.bg);
	g.setColour (scheme.outline);
	g.drawRect (0, 0, width, height, 2);
}

juce::Font SATTRAudioProcessorEditor::MinimalLNF::getComboBoxFont (juce::ComboBox& box)
{
	const float h = juce::jlimit (12.0f, 24.0f, box.getHeight() * 0.59f);
	return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

void SATTRAudioProcessorEditor::MinimalLNF::drawScrollbar (
	juce::Graphics& g, juce::ScrollBar&,
	int x, int y, int width, int height,
	bool isScrollbarVertical,
	int thumbStartPosition, int thumbSize,
	bool isMouseOver, bool isMouseDown)
{
	juce::ignoreUnused (x, y, width, height);

	const auto thumbColour = scheme.text.withAlpha (isMouseDown ? 0.7f
	                                                 : isMouseOver ? 0.5f
	                                                               : 0.3f);
	constexpr float barThickness = 7.0f;
	constexpr float cornerRadius = 3.5f;

	if (isScrollbarVertical)
	{
		const float bx = (float) (x + width) - barThickness - 1.0f;
		g.setColour (thumbColour);
		g.fillRoundedRectangle (bx, (float) thumbStartPosition,
		                        barThickness, (float) thumbSize, cornerRadius);
	}
	else
	{
		const float by = (float) (y + height) - barThickness - 1.0f;
		g.setColour (thumbColour);
		g.fillRoundedRectangle ((float) thumbStartPosition, by,
		                        (float) thumbSize, barThickness, cornerRadius);
	}
}

void SATTRAudioProcessorEditor::MinimalLNF::drawAlertBox (juce::Graphics& g,
                                                          juce::AlertWindow& alert,
                                                          const juce::Rectangle<int>& textArea,
                                                          juce::TextLayout& textLayout)
{
	auto bounds = alert.getLocalBounds();

	g.setColour (scheme.bg);
	g.fillRect (bounds);

	g.setColour (scheme.outline);
	g.drawRect (bounds.reduced (1), 3);

	g.setColour (scheme.text);
	textLayout.draw (g, textArea.toFloat());
}

void SATTRAudioProcessorEditor::MinimalLNF::drawBubble (juce::Graphics& g,
                                                        juce::BubbleComponent&,
                                                        const juce::Point<float>&,
                                                        const juce::Rectangle<float>& body)
{
	g.fillAll (findColour (juce::TooltipWindow::backgroundColourId));
	drawOverlayPanel (g,
	                  body.getSmallestIntegerContainer(),
	                  findColour (juce::TooltipWindow::backgroundColourId),
	                  findColour (juce::TooltipWindow::outlineColourId));
}

juce::Font SATTRAudioProcessorEditor::MinimalLNF::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
	const float h = juce::jlimit (12.0f, 26.0f, buttonHeight * 0.48f);
	return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

juce::Font SATTRAudioProcessorEditor::MinimalLNF::getAlertWindowMessageFont()
{
	auto f = juce::LookAndFeel_V4::getAlertWindowMessageFont();
	f.setBold (true);
	return f;
}

juce::Font SATTRAudioProcessorEditor::MinimalLNF::getLabelFont (juce::Label& label)
{
	auto f = label.getFont();
	if (f.getHeight() <= 0.0f)
	{
		const float h = juce::jlimit (12.0f, 40.0f, (float) juce::jmax (12, label.getHeight() - 6));
		f = juce::Font (juce::FontOptions (h).withStyle ("Bold"));
	}
	else
	{
		f.setBold (true);
	}
	return f;
}

juce::Font SATTRAudioProcessorEditor::MinimalLNF::getSliderPopupFont (juce::Slider&)
{
	return makeOverlayDisplayFont();
}

juce::Rectangle<int> SATTRAudioProcessorEditor::MinimalLNF::getTooltipBounds (const juce::String& tipText,
                                                                               juce::Point<int> screenPos,
                                                                               juce::Rectangle<int> parentArea)
{
	const auto f = makeOverlayDisplayFont();
	const int h = juce::jmax (UiMetrics::tooltipMinHeight,
	                          (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));

	const int anchorOffsetX = juce::jmax (8, (int) std::round ((double) h * UiMetrics::tooltipAnchorXRatio));
	const int anchorOffsetY = juce::jmax (10, (int) std::round ((double) h * UiMetrics::tooltipAnchorYRatio));
	const int parentMargin = juce::jmax (2, (int) std::round ((double) h * UiMetrics::tooltipParentMarginRatio));
	const int widthPad = juce::jmax (16, (int) std::round (f.getHeight() * UiMetrics::tooltipWidthPadFontRatio));

	const int w = juce::jmax (UiMetrics::tooltipMinWidth, stringWidth (f, tipText) + widthPad);
	auto r = juce::Rectangle<int> (screenPos.x + anchorOffsetX, screenPos.y + anchorOffsetY, w, h);
	return r.constrainedWithin (parentArea.reduced (parentMargin));
}

void SATTRAudioProcessorEditor::MinimalLNF::drawTooltip (juce::Graphics& g,
                                                          const juce::String& text,
                                                          int width,
                                                          int height)
{
	const auto f = makeOverlayDisplayFont();
	const int h = juce::jmax (UiMetrics::tooltipMinHeight,
	                          (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));
	const int textInsetX = juce::jmax (4, (int) std::round ((double) h * UiMetrics::tooltipTextInsetXRatio));
	const int textInsetY = juce::jmax (1, (int) std::round ((double) h * UiMetrics::tooltipTextInsetYRatio));

	g.fillAll (findColour (juce::TooltipWindow::backgroundColourId));
	drawOverlayPanel (g,
	                  { 0, 0, width, height },
	                  findColour (juce::TooltipWindow::backgroundColourId),
	                  findColour (juce::TooltipWindow::outlineColourId));

	g.setColour (findColour (juce::TooltipWindow::textColourId));
	g.setFont (f);
	g.drawFittedText (text,
	                  textInsetX,
	                  textInsetY,
	                  juce::jmax (1, width - (textInsetX * 2)),
	                  juce::jmax (1, height - (textInsetY * 2)),
	                  juce::Justification::centred,
	                  1);
}

// ----------------------------------------------------------------
//  Static loader param-ID table
// ----------------------------------------------------------------
const SATTRAudioProcessorEditor::LoaderParamIds SATTRAudioProcessorEditor::kLoaderParams[3] =
{
	{ // A
		SATTRAudioProcessor::kParamEnableA,
		SATTRAudioProcessor::kParamHpFreqA, SATTRAudioProcessor::kParamLpFreqA, SATTRAudioProcessor::kParamInA, SATTRAudioProcessor::kParamOutA, SATTRAudioProcessor::kParamTiltA,
		SATTRAudioProcessor::kParamSeriesA, SATTRAudioProcessor::kParamPanA,    SATTRAudioProcessor::kParamFredA, SATTRAudioProcessor::kParamPosA, SATTRAudioProcessor::kParamResoA,
		SATTRAudioProcessor::kParamInvA,    SATTRAudioProcessor::kParamChaosA, SATTRAudioProcessor::kParamChaosFilterA,
		SATTRAudioProcessor::kParamChaosAmtA, SATTRAudioProcessor::kParamChaosSpdA,
		SATTRAudioProcessor::kParamChaosAmtFilterA, SATTRAudioProcessor::kParamChaosSpdFilterA,
		SATTRAudioProcessor::kParamModeInA, SATTRAudioProcessor::kParamModeOutA, SATTRAudioProcessor::kParamSumBusA, SATTRAudioProcessor::kParamFilterPosA, SATTRAudioProcessor::kParamMixA,
		SATTRAudioProcessor::kParamSatTypeA, SATTRAudioProcessor::kParamSatRawA, SATTRAudioProcessor::kParamSatDriveA, SATTRAudioProcessor::kParamSatGirthA,
		SATTRAudioProcessor::kParamSatModA, SATTRAudioProcessor::kParamSatBiasA, SATTRAudioProcessor::kParamSatSagA,
		SATTRAudioProcessor::kParamDetailA,
		SATTRAudioProcessor::kParamInstabilityA,
		SATTRAudioProcessor::kParamDelayA,
		SATTRAudioProcessor::kParamExpA
	},
	{ // B
		SATTRAudioProcessor::kParamEnableB,
		SATTRAudioProcessor::kParamHpFreqB, SATTRAudioProcessor::kParamLpFreqB, SATTRAudioProcessor::kParamInB, SATTRAudioProcessor::kParamOutB, SATTRAudioProcessor::kParamTiltB,
		SATTRAudioProcessor::kParamSeriesB, SATTRAudioProcessor::kParamPanB,    SATTRAudioProcessor::kParamFredB, SATTRAudioProcessor::kParamPosB, SATTRAudioProcessor::kParamResoB,
		SATTRAudioProcessor::kParamInvB,    SATTRAudioProcessor::kParamChaosB, SATTRAudioProcessor::kParamChaosFilterB,
		SATTRAudioProcessor::kParamChaosAmtB, SATTRAudioProcessor::kParamChaosSpdB,
		SATTRAudioProcessor::kParamChaosAmtFilterB, SATTRAudioProcessor::kParamChaosSpdFilterB,
		SATTRAudioProcessor::kParamModeInB, SATTRAudioProcessor::kParamModeOutB, SATTRAudioProcessor::kParamSumBusB, SATTRAudioProcessor::kParamFilterPosB, SATTRAudioProcessor::kParamMixB,
		SATTRAudioProcessor::kParamSatTypeB, SATTRAudioProcessor::kParamSatRawB, SATTRAudioProcessor::kParamSatDriveB, SATTRAudioProcessor::kParamSatGirthB,
		SATTRAudioProcessor::kParamSatModB, SATTRAudioProcessor::kParamSatBiasB, SATTRAudioProcessor::kParamSatSagB,
		SATTRAudioProcessor::kParamDetailB,
		SATTRAudioProcessor::kParamInstabilityB,
		SATTRAudioProcessor::kParamDelayB,
		SATTRAudioProcessor::kParamExpB
	},
	{ // C
		SATTRAudioProcessor::kParamEnableC,
		SATTRAudioProcessor::kParamHpFreqC, SATTRAudioProcessor::kParamLpFreqC, SATTRAudioProcessor::kParamInC, SATTRAudioProcessor::kParamOutC, SATTRAudioProcessor::kParamTiltC,
		SATTRAudioProcessor::kParamSeriesC, SATTRAudioProcessor::kParamPanC,    SATTRAudioProcessor::kParamFredC, SATTRAudioProcessor::kParamPosC, SATTRAudioProcessor::kParamResoC,
		SATTRAudioProcessor::kParamInvC,    SATTRAudioProcessor::kParamChaosC, SATTRAudioProcessor::kParamChaosFilterC,
		SATTRAudioProcessor::kParamChaosAmtC, SATTRAudioProcessor::kParamChaosSpdC,
		SATTRAudioProcessor::kParamChaosAmtFilterC, SATTRAudioProcessor::kParamChaosSpdFilterC,
		SATTRAudioProcessor::kParamModeInC, SATTRAudioProcessor::kParamModeOutC, SATTRAudioProcessor::kParamSumBusC, SATTRAudioProcessor::kParamFilterPosC, SATTRAudioProcessor::kParamMixC,
		SATTRAudioProcessor::kParamSatTypeC, SATTRAudioProcessor::kParamSatRawC, SATTRAudioProcessor::kParamSatDriveC, SATTRAudioProcessor::kParamSatGirthC,
		SATTRAudioProcessor::kParamSatModC, SATTRAudioProcessor::kParamSatBiasC, SATTRAudioProcessor::kParamSatSagC,
		SATTRAudioProcessor::kParamDetailC,
		SATTRAudioProcessor::kParamInstabilityC,
		SATTRAudioProcessor::kParamDelayC,
		SATTRAudioProcessor::kParamExpC
	}
};

// ----------------------------------------------------------------
//  Loader ref accessors
// ----------------------------------------------------------------
SATTRAudioProcessorEditor::LoaderRefs SATTRAudioProcessorEditor::getLoaderRefs (int i)
{
	switch (i)
	{
		case 1: return { enableButtonB,
		                 hpFreqSliderB, lpFreqSliderB, inSliderB, outSliderB, tiltSliderB, seriesSliderB, panSliderB, fredSliderB, posSliderB,
		                 invButtonB, chaosButtonB, chaosFilterButtonB, chaosDisplayB,
		                 expButtonB, expDisplayB,
		                 modeInComboB, modeOutComboB, sumBusComboB, filterPosComboB, filterBarB_, mixSliderB,
		                 satTypeComboB, rawButtonB, satDriveSliderB, satGirthSliderB, satModSliderB, satBiasSliderB, satSagSliderB, detailSliderB, instabilitySliderB, delaySliderB };
		case 2: return { enableButtonC,
		                 hpFreqSliderC, lpFreqSliderC, inSliderC, outSliderC, tiltSliderC, seriesSliderC, panSliderC, fredSliderC, posSliderC,
		                 invButtonC, chaosButtonC, chaosFilterButtonC, chaosDisplayC,
		                 expButtonC, expDisplayC,
		                 modeInComboC, modeOutComboC, sumBusComboC, filterPosComboC, filterBarC_, mixSliderC,
		                 satTypeComboC, rawButtonC, satDriveSliderC, satGirthSliderC, satModSliderC, satBiasSliderC, satSagSliderC, detailSliderC, instabilitySliderC, delaySliderC };
		default: return { enableButtonA,
		                  hpFreqSliderA, lpFreqSliderA, inSliderA, outSliderA, tiltSliderA, seriesSliderA, panSliderA, fredSliderA, posSliderA,
		                  invButtonA, chaosButtonA, chaosFilterButtonA, chaosDisplayA,
		                  expButtonA, expDisplayA,
		                  modeInComboA, modeOutComboA, sumBusComboA, filterPosComboA, filterBarA_, mixSliderA,
		                  satTypeComboA, rawButtonA, satDriveSliderA, satGirthSliderA, satModSliderA, satBiasSliderA, satSagSliderA, detailSliderA, instabilitySliderA, delaySliderA };
	}
}

SATTRAudioProcessorEditor::AttachRefs SATTRAudioProcessorEditor::getAttachRefs (int i)
{
	switch (i)
	{
		case 1: return { enableAttachB,
		                 hpFreqAttachB, lpFreqAttachB, inAttachB, outAttachB, tiltAttachB, seriesAttachB, panAttachB, fredAttachB, posAttachB,
		                 invAttachB, chaosAttachB, chaosFilterAttachB, expAttachB,
		                 modeInAttachB, modeOutAttachB, sumBusAttachB, filterPosAttachB, mixAttachB,
		                 satTypeAttachB, rawAttachB, satDriveAttachB, satGirthAttachB, satModAttachB, satBiasAttachB, satSagAttachB, detailAttachB, instabilityAttachB, delayAttachB };
		case 2: return { enableAttachC,
		                 hpFreqAttachC, lpFreqAttachC, inAttachC, outAttachC, tiltAttachC, seriesAttachC, panAttachC, fredAttachC, posAttachC,
		                 invAttachC, chaosAttachC, chaosFilterAttachC, expAttachC,
		                 modeInAttachC, modeOutAttachC, sumBusAttachC, filterPosAttachC, mixAttachC,
		                 satTypeAttachC, rawAttachC, satDriveAttachC, satGirthAttachC, satModAttachC, satBiasAttachC, satSagAttachC, detailAttachC, instabilityAttachC, delayAttachC };
		default: return { enableAttachA,
		                  hpFreqAttachA, lpFreqAttachA, inAttachA, outAttachA, tiltAttachA, seriesAttachA, panAttachA, fredAttachA, posAttachA,
		                  invAttachA, chaosAttachA, chaosFilterAttachA, expAttachA,
		                  modeInAttachA, modeOutAttachA, sumBusAttachA, filterPosAttachA, mixAttachA,
		                  satTypeAttachA, rawAttachA, satDriveAttachA, satGirthAttachA, satModAttachA, satBiasAttachA, satSagAttachA, detailAttachA, instabilityAttachA, delayAttachA };
	}
}

// ----------------------------------------------------------------
//  setupLoaderUI - unified per-loader component initialisation
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::setupLoaderUI (int loaderIndex, LoaderRefs r,
                                                const char* chaosAmtId, const char* chaosSpdId)
{
	const juce::String suffix = juce::String (loaderIndex == 0 ? "A" : loaderIndex == 1 ? "B" : "C");

	addAndMakeVisible (r.enableBtn);
	r.enableBtn.setButtonText ("ENABLE " + suffix);
	r.enableBtn.addListener (this);

	using ST = BarSlider::Type;
	auto setupSlider = [this] (BarSlider& slider, const juce::String& /*tooltip*/, ST type) {
		addAndMakeVisible (slider);
		slider.setOwner (this);
		slider.setType (type);
		setupBar (slider);
		slider.addListener (this);
	};

	setupSlider (r.hp,    "HP Filter " + suffix,                       ST::HpFreq);
	setupSlider (r.lp,    "LP Filter " + suffix,                       ST::LpFreq);
	setupSlider (r.in,    "Input Gain " + suffix,                      ST::Input);
	setupSlider (r.out,   "Output Gain " + suffix,                     ST::Output);
	setupSlider (r.tilt,  "Tilt EQ " + suffix + " (-6/+6 dB)",    ST::Tilt);
	setupSlider (r.detail, "Detail " + suffix + " (0-100%)",           ST::Detail);
	setupSlider (r.series, "Series " + suffix + " (1-6x cascade)",      ST::Series);
	r.series.setAllowNumericPopup (false);
	setupSlider (r.instability,   "Instability " + suffix + " (0-100%)",         ST::Instability);
	setupSlider (r.delay, "Delay " + suffix + " (auto-align)",         ST::Delay);
	r.delay.setEnabled (false);  // read-only, set by ALIGN
	setupSlider (r.pan,   "Pan " + suffix + " (L-R)",                  ST::Pan);
	setupSlider (r.fred,  "Angle " + suffix + " (off-axis mic simulation)", ST::Fred);
	setupSlider (r.pos,   "Distance " + suffix + " (proximity/distance)",   ST::Pos);

	addAndMakeVisible (r.inv);   r.inv.setButtonText ("INV");         r.inv.addListener (this);
	r.inv.setTooltip ({});
	addAndMakeVisible (r.chaos); r.chaos.setButtonText ("CHSD"); r.chaos.addListener (this);
	r.chaos.addMouseListener (this, false);
	addAndMakeVisible (r.chaosFilter); r.chaosFilter.setButtonText ("CHSF"); r.chaosFilter.addListener (this);
	r.chaosFilter.addMouseListener (this, false);

	{
		const auto& ids = kLoaderParams[juce::jlimit (0, 2, loaderIndex)];
		const float savedAmtD = audioProcessor.getValueTreeState().getRawParameterValue (chaosAmtId)->load();
		const float savedSpdD = audioProcessor.getValueTreeState().getRawParameterValue (chaosSpdId)->load();
		const float savedAmtF = audioProcessor.getValueTreeState().getRawParameterValue (ids.chaosAmtFilter)->load();
		const float savedSpdF = audioProcessor.getValueTreeState().getRawParameterValue (ids.chaosSpdFilter)->load();
		r.chaos.setTooltip (formatChaosTooltip (savedAmtD, savedSpdD));
		r.chaosFilter.setTooltip (formatChaosTooltip (savedAmtF, savedSpdF));
		r.chaosDisp.setText ("", juce::dontSendNotification);
		r.chaosDisp.setInterceptsMouseClicks (false, false);
		r.chaosDisp.setTooltip ({});
		r.chaosDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setOpaque (false);
		addAndMakeVisible (r.chaosDisp);
	}

	// EXP uses a compact tooltip; full SC/filter details live in the right-click prompt.
	addAndMakeVisible (r.exp);   r.exp.setButtonText ("EXP");   r.exp.addListener (this);
	r.exp.addMouseListener (this, false);
	{
		const auto& orderParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpOrderA
		                         : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpOrderB
		                                            : SATTRAudioProcessor::kParamExpOrderC;
		const auto& ratioParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpRatioA
		                         : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpRatioB
		                                            : SATTRAudioProcessor::kParamExpRatioC;
		const bool savedOrder = audioProcessor.getValueTreeState().getRawParameterValue (orderParamId)->load() >= 0.5f;
		const float savedRatio = audioProcessor.getValueTreeState().getRawParameterValue (ratioParamId)->load();
		r.exp.setTooltip (formatExpTooltip (savedOrder, savedRatio));

		r.expDisp.setText ("", juce::dontSendNotification);
		r.expDisp.setInterceptsMouseClicks (false, false);
		r.expDisp.setTooltip ({});
		r.expDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.expDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.expDisp.setOpaque (false);
		addAndMakeVisible (r.expDisp);
	}

	auto setupModeCombo = [this] (juce::ComboBox& combo) {
		addAndMakeVisible (combo);
		combo.addItem ("L+R", 1);
		combo.addItem ("MID", 2);
		combo.addItem ("SIDE", 3);
		combo.setJustificationType (juce::Justification::centred);
		combo.setLookAndFeel (&lnf);
		combo.addListener (this);
	};
	setupModeCombo (r.modeIn);
	setupModeCombo (r.modeOut);

	{
		addAndMakeVisible (r.sumBus);
		r.sumBus.addItem ("ST",  1);
		r.sumBus.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")) + "M", 2);
		r.sumBus.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")) + "S", 3);
		r.sumBus.setJustificationType (juce::Justification::centred);
		r.sumBus.setLookAndFeel (&lnf);
		r.sumBus.addListener (this);
	}

	{
		addAndMakeVisible (r.filterPos);
		r.filterPos.addItem (juce::String::fromUTF8 ("F\xe2\x96\xbc T\xe2\x96\xbc"), 1);
		r.filterPos.addItem (juce::String::fromUTF8 ("F\xe2\x96\xb2 T\xe2\x96\xb2"), 2);
		r.filterPos.addItem (juce::String::fromUTF8 ("F\xe2\x96\xb2 T\xe2\x96\xbc"), 3);
		r.filterPos.addItem (juce::String::fromUTF8 ("F\xe2\x96\xbc T\xe2\x96\xb2"), 4);
		r.filterPos.setJustificationType (juce::Justification::centred);
		r.filterPos.setLookAndFeel (&lnf);
		r.filterPos.addListener (this);
	}

	r.filterBar.setOwner (this, loaderIndex);
	r.filterBar.setScheme (activeScheme);
	addAndMakeVisible (r.filterBar);

	setupSlider (r.mix, "Mix " + suffix + " (Dry/Wet)", ST::Mix);

	// Saturation controls
	{
		addAndMakeVisible (r.satType);
		r.satType.addItem ("CLEAN",     1);
		r.satType.addItem ("TAPE",      2);
		r.satType.addItem ("TUBE",      3);
		r.satType.addItem ("TRANSISTOR", 4);
		r.satType.addItem ("DIODE",     5);
		r.satType.addItem ("CLIPPER",   6);
		r.satType.setJustificationType (juce::Justification::centred);
		r.satType.setLookAndFeel (&lnf);
		r.satType.addListener (this);

		addAndMakeVisible (r.raw);
		r.raw.setButtonText ("RAW");
		r.raw.setLookAndFeel (&lnf);
	}

	setupSlider (r.satDrive, "Drive " + suffix + " (0-100%)",         ST::SatDrive);
	setupSlider (r.satGirth, "Girth " + suffix + " (low emphasis)",   ST::SatGirth);
	setupSlider (r.satMod,   "Mod " + suffix + " (model modulation)", ST::SatMod);
	setupSlider (r.satBias,  "Bias " + suffix + " (asymmetry)",       ST::SatBias);
	setupSlider (r.satSag,   "Dynamics " + suffix + " (model-dependent dynamics)", ST::SatSag);
}

// ----------------------------------------------------------------
//  createLoaderAttachments - unified per-loader param attachment wiring
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::createLoaderAttachments (juce::AudioProcessorValueTreeState& params,
                                                          int loaderIndex,
                                                          LoaderRefs ui, AttachRefs a)
{
	const auto& ids = kLoaderParams[loaderIndex];

	a.enableAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.enable,  ui.enableBtn);
	a.hpAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.hpFreq,  ui.hp);
	a.lpAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.lpFreq,  ui.lp);
	a.inAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.in,      ui.in);
	a.outAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.out,     ui.out);
	a.tiltAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.tilt,    ui.tilt);
	a.seriesAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.series,  ui.series);
	a.panAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.pan,     ui.pan);
	a.fredAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.fred,    ui.fred);
	a.posAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.pos,     ui.pos);
	a.invAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.inv,     ui.inv);
	a.chaosAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.chaos,   ui.chaos);
	a.chaosFilterAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (params, ids.chaosFilter, ui.chaosFilter);
	a.expAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.exp,     ui.exp);
	a.modeInAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeIn,  ui.modeIn);
	a.modeOutAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeOut, ui.modeOut);
	a.sumBusAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.sumBus, ui.sumBus);
	a.filterPosAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.filterPos, ui.filterPos);
	a.mixAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.mix,     ui.mix);

	a.satTypeAtt.reset();
	a.rawAtt      = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.satRaw,   ui.raw);
	a.satDriveAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satDrive, ui.satDrive);
	a.satGirthAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satGirth, ui.satGirth);
	a.satModAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satMod,   ui.satMod);
	a.satBiasAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satBias,  ui.satBias);
	a.satSagAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satSag,   ui.satSag);
	a.detailAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.detail,   ui.detail);
	a.instabilityAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.instability,      ui.instability);
	a.delayAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.delay,    ui.delay);

	// UI-only skew: changes slider feel without altering VST3 parameter normalization
	ui.hp.setSkewFactor (0.35);
	ui.lp.setSkewFactor (0.35);
	ui.in.setSkewFactor (SATTRAudioProcessor::kGainSkew);
	ui.out.setSkewFactor (SATTRAudioProcessor::kGainSkew);
}

int SATTRAudioProcessorEditor::getSelectedSatTypeModelIndex (const juce::ComboBox& combo) const noexcept
{
	return visibleComboIdToSatTypeModel (combo.getSelectedId());
}

void SATTRAudioProcessorEditor::syncSatTypeComboSelection (int loaderIndex)
{
	auto refs = getLoaderRefs (loaderIndex);
	const char* paramId = loaderIndex == 0 ? SATTRAudioProcessor::kParamSatTypeA
	                     : loaderIndex == 1 ? SATTRAudioProcessor::kParamSatTypeB
	                                        : SATTRAudioProcessor::kParamSatTypeC;

	if (auto* raw = audioProcessor.getValueTreeState().getRawParameterValue (paramId))
	{
		const int modelIndex = juce::roundToInt (raw->load());
		refs.satType.setSelectedId (satTypeModelToVisibleComboId (modelIndex), juce::dontSendNotification);
	}
}

void SATTRAudioProcessorEditor::commitSatTypeComboSelection (int loaderIndex)
{
	auto refs = getLoaderRefs (loaderIndex);
	const char* paramId = loaderIndex == 0 ? SATTRAudioProcessor::kParamSatTypeA
	                     : loaderIndex == 1 ? SATTRAudioProcessor::kParamSatTypeB
	                                        : SATTRAudioProcessor::kParamSatTypeC;

	if (auto* param = audioProcessor.getValueTreeState().getParameter (paramId))
	{
		const float modelIndex = static_cast<float> (getSelectedSatTypeModelIndex (refs.satType));
		param->setValueNotifyingHost (param->convertTo0to1 (modelIndex));
	}
}

// ----------------------------------------------------------------
//  Constructor
// ----------------------------------------------------------------
SATTRAudioProcessorEditor::SATTRAudioProcessorEditor (SATTRAudioProcessor& p)
	: AudioProcessorEditor (&p), audioProcessor (p)
{
	setOpaque (true);
	setBufferedToImage (true);
	setLookAndFeel (&lnf);

	// Setup loader A/B/C components (unified)
	for (int i = 0; i < 3; ++i)
		setupLoaderUI (i, getLoaderRefs (i), kLoaderParams[i].chaosAmt, kLoaderParams[i].chaosSpd);

	// Setup global controls
	addAndMakeVisible (routeCombo);
	routeCombo.addItem ("A>B>C", 1);
	routeCombo.addItem ("A|B|C", 2);
	routeCombo.addItem ("A>B|C", 3);
	routeCombo.addItem ("A|B>C", 4);
	routeCombo.addItem ("(A|B)>C", 5);
	routeCombo.addItem ("A>(B|C)", 6);
	routeCombo.setJustificationType (juce::Justification::centred);
	routeCombo.setLookAndFeel (&lnf);
	routeCombo.addListener (this);

	addAndMakeVisible (matchCombo);
	matchCombo.addItem ("x1", 1);
	matchCombo.addItem ("x2", 2);
	matchCombo.addItem ("x4", 3);
	matchCombo.addItem ("x8", 4);
	matchCombo.addItem ("x16", 5);
	matchCombo.setJustificationType (juce::Justification::centred);
	matchCombo.setLookAndFeel (&lnf);

	addAndMakeVisible (trimCombo);
	trimCombo.addItem ("Off", 1);
	trimCombo.addItem ("0 dB", 2);
	trimCombo.addItem ("-3 dB", 3);
	trimCombo.addItem ("-6 dB", 4);
	trimCombo.addItem ("-12 dB", 5);
	trimCombo.addItem ("-18 dB", 6);
	trimCombo.setJustificationType (juce::Justification::centred);
	trimCombo.setLookAndFeel (&lnf);

	// Global MIX bar slider (footer)
	addAndMakeVisible (globalMixSlider);
	globalMixSlider.setOwner (this);
	globalMixSlider.setType (BarSlider::Type::GlobalMix);
	setupBar (globalMixSlider);
	globalMixSlider.addListener (this);

	// Dual mix bar for SEND mode (footer)
	addAndMakeVisible (dualMixBar_);
	dualMixBar_.setOwner (this);
	dualMixBar_.setVisible (false);

	// Mix mode combo (footer row 2)
	addAndMakeVisible (mixModeCombo);
	mixModeCombo.addItem ("INSERT", 1);
	mixModeCombo.addItem ("SEND",   2);
	mixModeCombo.setJustificationType (juce::Justification::centred);
	mixModeCombo.setLookAndFeel (&lnf);

	// Global OUTPUT bar slider (footer)
	addAndMakeVisible (globalOutputSlider);
	globalOutputSlider.setOwner (this);
	globalOutputSlider.setType (BarSlider::Type::GlobalOutput);
	setupBar (globalOutputSlider);
	globalOutputSlider.setSkewFactor (SATTRAudioProcessor::kGainSkew);
	globalOutputSlider.addListener (this);

	// Limiter threshold bar slider (footer)
	addAndMakeVisible (limThresholdSlider);
	limThresholdSlider.setOwner (this);
	limThresholdSlider.setType (BarSlider::Type::LimThreshold);
	setupBar (limThresholdSlider);
	limThresholdSlider.addListener (this);

	// Limiter mode combo (footer)
	addAndMakeVisible (limModeCombo);
	limModeCombo.addItem ("NONE",   1);
	limModeCombo.addItem ("WET",    2);
	limModeCombo.addItem ("GLOBAL", 3);
	limModeCombo.setJustificationType (juce::Justification::centred);
	limModeCombo.setLookAndFeel (&lnf);

	addAndMakeVisible (invPolCombo);
	invPolCombo.addItem ("NONE",   1);
	invPolCombo.addItem ("WET",    2);
	invPolCombo.addItem ("GLOBAL", 3);
	invPolCombo.setJustificationType (juce::Justification::centred);
	invPolCombo.setLookAndFeel (&lnf);

	addAndMakeVisible (invStrCombo);
	invStrCombo.addItem ("NONE",   1);
	invStrCombo.addItem ("WET",    2);
	invStrCombo.addItem ("GLOBAL", 3);
	invStrCombo.setJustificationType (juce::Justification::centred);
	invStrCombo.setLookAndFeel (&lnf);

	// Align button (momentary toggle in header)
	addAndMakeVisible (alignButton);
	alignButton.setButtonText ("ALIGN");
	alignButton.addListener (this);

	// Create parameter attachments
	auto& params = audioProcessor.getValueTreeState();

	// Create per-loader parameter attachments
	for (int i = 0; i < 3; ++i)
		createLoaderAttachments (params, i, getLoaderRefs (i), getAttachRefs (i));

	for (int i = 0; i < 3; ++i)
		syncSatTypeComboSelection (i);

	// Global parameter attachments
	routeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamRoute, routeCombo);
	matchAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamOversample, matchCombo);
	trimAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamTrim, trimCombo);
	mixModeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamMixMode, mixModeCombo);
	globalMixAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, SATTRAudioProcessor::kParamMix, globalMixSlider);
	globalOutputAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, SATTRAudioProcessor::kParamOutput, globalOutputSlider);
	limThresholdAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, SATTRAudioProcessor::kParamLimThreshold, limThresholdSlider);
	limModeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamLimMode, limModeCombo);
	invPolAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamInvPol, invPolCombo);
	invStrAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, SATTRAudioProcessor::kParamInvStr, invStrCombo);
	alignAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, SATTRAudioProcessor::kParamAlign, alignButton);

	// Initialize per-loader collapse state from processor
	ioExpandedA_ = audioProcessor.getUiIoExpanded (0);
	ioExpandedB_ = audioProcessor.getUiIoExpanded (1);
	ioExpandedC_ = audioProcessor.getUiIoExpanded (2);

	// Setup tooltip window
	tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 250);
	tooltipWindow->setLookAndFeel (&lnf);
	tooltipWindow->setAlwaysOnTop (true);
	tooltipWindow->setInterceptsMouseClicks (false, false);

	// Setup prompt overlay
	addChildComponent (promptOverlay);
	promptOverlay.setInterceptsMouseClicks (true, true);

	// Setup parameter listeners for UI state
	for (auto* paramId : kUiMirrorParamIds)
		params.addParameterListener (paramId, this);

	// Restore persisted UI state from processor (palette, CRT, colors)
	useCustomPalette = audioProcessor.getUiUseCustomPalette();
	crtEnabled       = audioProcessor.getUiFxTailEnabled();
	applyCrtState (crtEnabled);
	for (int i = 0; i < 2; ++i)
		customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

	// Initialize palette
	applyActivePalette();

	// Initial refresh of cached text
	refreshLegendTextCache();
	legendDirty = false;

	// Restore persisted window size. SAT/CAB compact layout uses one fixed
	// column height and up to three loader-width columns.
	const int restoredW = juce::jlimit (getCompactTargetWidthForLoaderCount (kCompactMinVisibleLoaders),
	                                    getCompactTargetWidthForLoaderCount (kCompactMaxVisibleLoaders),
	                                    audioProcessor.getUiEditorWidth());
	const int restoredH = kCompactFixedHeightPx;
	visibleLoaderCount_ = getMaxVisibleLoaderCountForWidth (restoredW);
	setSize (restoredW, restoredH);
	setResizable (true, true);
	setResizeLimits (getCompactTargetWidthForLoaderCount (kCompactMinVisibleLoaders),
	                 kCompactFixedHeightPx,
	                 getCompactTargetWidthForLoaderCount (kCompactMaxVisibleLoaders),
	                 kCompactFixedHeightPx);

	// Initialize loader enabled/disabled visual state
	updateLoaderEnabledState (0);
	updateLoaderEnabledState (1);
	updateLoaderEnabledState (2);
}

SATTRAudioProcessorEditor::~SATTRAudioProcessorEditor()
{
	TR::dismissEditorOwnedModalPrompts (lnf);
	setPromptOverlayActive (false);
	stopTimer();

	// Persist UI state to processor before teardown
	audioProcessor.setUiUseCustomPalette (useCustomPalette);
	audioProcessor.setUiFxTailEnabled (crtEnabled);

	setComponentEffect (nullptr);
	setLookAndFeel (nullptr);

	if (tooltipWindow != nullptr)
		tooltipWindow->setLookAndFeel (nullptr);

	invPolCombo.setLookAndFeel (nullptr);
	invStrCombo.setLookAndFeel (nullptr);
	limModeCombo.setLookAndFeel (nullptr);
	mixModeCombo.setLookAndFeel (nullptr);
	satTypeComboA.setLookAndFeel (nullptr);
	satTypeComboB.setLookAndFeel (nullptr);
	satTypeComboC.setLookAndFeel (nullptr);
	rawButtonA.setLookAndFeel (nullptr);
	rawButtonB.setLookAndFeel (nullptr);
	rawButtonC.setLookAndFeel (nullptr);

	setLookAndFeel (nullptr);

	auto& params = audioProcessor.getValueTreeState();
	for (auto* paramId : kUiMirrorParamIds)
		params.removeParameterListener (paramId, this);
}

// ----------------------------------------------------------------
//  Paint
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::paint (juce::Graphics& g)
{
	using namespace TR;

	g.fillAll (activeScheme.bg);
	g.setColour (activeScheme.text);

	if (legendDirty)
	{
		refreshLegendTextCache();
		legendDirty = false;
	}

	// Helper lambda for drawing legend text with fallback
	auto tryDrawLegend = [&] (const juce::Rectangle<int>& area,
	                          const juce::String& fullText,
	                          const juce::String& shortText,
	                          const juce::String& intText) -> bool {
		constexpr float baseFontPx = 32.0f;
		constexpr float minFontPx = 14.0f;

		if (area.isEmpty())
			return false;

		g.setFont (kBoldFont40());
		const float shrinkFloor = baseFontPx * 0.75f;

		if (drawIfFitsWithOptionalShrink (g, area, fullText, baseFontPx, shrinkFloor))
			return true;

		if (drawIfFitsWithOptionalShrink (g, area, shortText, baseFontPx, minFontPx))
			return true;

		drawValueNoEllipsis (g, area, intText, juce::String(), intText, baseFontPx, minFontPx);
		return true;
	};

	// Title & version
	{
		const int titleX = cachedHeaderTitleX_;
		constexpr int titleY = 12;
		constexpr int titleH = 32;
		g.setFont (juce::Font (juce::FontOptions (28.0f).withStyle ("Bold")));
		g.drawText ("SAT-TR", titleX, titleY, 200, titleH, juce::Justification::left);

		// Draw version near the gear icon (scaled like other TR plugins)
		const auto iconArea = getInfoIconArea();
		constexpr int kVersionGapPx = 8;
		auto versionFont = juce::Font (juce::FontOptions (
		    juce::jmax (10.0f, (float) titleH * UiMetrics::versionFontRatio)).withStyle ("Bold"));
		g.setFont (versionFont);

		const int versionH = juce::jlimit (10, iconArea.getHeight(),
		    (int) std::round ((double) iconArea.getHeight() * UiMetrics::versionHeightRatio));
		const int versionY = iconArea.getBottom() - versionH;
		const int desiredVersionW = juce::jlimit (28, 64,
		    (int) std::round ((double) iconArea.getWidth() * UiMetrics::versionDesiredWidthRatio));
		const int versionRight = iconArea.getX() - kVersionGapPx;
		const int versionLeftLimit = titleX;
		const int versionX = juce::jmax (versionLeftLimit, versionRight - desiredVersionW);
		const int versionW = juce::jmax (0, versionRight - versionX);

		if (versionW > 0)
			g.drawText (juce::String ("v") + InfoContent::version, versionX, versionY, versionW, versionH,
			            juce::Justification::bottomRight, false);

		if (footerExpanded_ && ! cachedFooterTitleArea_.isEmpty())
		{
			g.setColour (activeScheme.text);
			g.setFont (juce::Font (juce::FontOptions (28.0f).withStyle ("Bold")));
			g.drawText ("GLOBAL", cachedFooterTitleArea_, juce::Justification::centred);
		}

		auto drawFooterComboLabel = [&] (juce::ComboBox& combo, const juce::String& text)
		{
			if (! combo.isVisible()) return;

			g.setColour (activeScheme.text);
			g.setFont (lnf.getComboBoxFont (combo));
			const auto labelArea = combo.getBounds().withHeight (22).translated (0, -24);
			g.drawText (text, labelArea, juce::Justification::centred);
		};

		drawFooterComboLabel (routeCombo, "ROUTE");
		drawFooterComboLabel (matchCombo, "OS");

		drawFooterComboLabel (mixModeCombo, "MIX");

		// Global MIX label + value (right of bar)
		if (globalMixSlider.isVisible())
		{
			const auto mixBounds = globalMixSlider.getBounds();
			if (! footerExpanded_)
			{
				const auto mixArea = mixBounds.withHeight (16).translated (0, -18);
				g.drawText ("MIX", mixArea, juce::Justification::centred);
			}

			const int gMixPct = juce::roundToInt (globalMixSlider.getValue() * 100.0);
			if (footerExpanded_)
			{
				const auto valArea = makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), mixBounds);
				const auto txt = juce::String (gMixPct) + "%";
				tryDrawLegend (valArea, txt + " MIX", txt + " MX", txt);
			}
			else
			{
				const auto valArea = makeFooterValueArea (mixBounds, kFooterMixValueWidthPx);
				g.drawText (juce::String (gMixPct) + "%", valArea, juce::Justification::centredLeft);
			}
		}
		else if (dualMixBar_.isVisible())
		{
			const auto mixBounds = dualMixBar_.getBounds();
			if (! footerExpanded_)
			{
				const auto mixArea = mixBounds.withHeight (16).translated (0, -18);
				g.drawText ("MIX", mixArea, juce::Justification::centred);
			}

			if (footerExpanded_)
			{
				const auto valArea = makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), mixBounds);
				tryDrawLegend (valArea, cachedMixTextFull + " MIX", cachedMixTextShort + " MX", cachedMixIntOnly);
			}
			else
			{
				const auto valArea = makeFooterValueArea (mixBounds, kFooterMixValueWidthPx);
				g.drawText (cachedMixIntOnly, valArea, juce::Justification::centredLeft);
			}
		}

		// Global OUTPUT label + value (right of bar)
		if (globalOutputSlider.isVisible())
		{
			const auto outBounds = globalOutputSlider.getBounds();
			if (! footerExpanded_)
			{
				const auto outArea = outBounds.withHeight (16).translated (0, -18);
				g.drawText ("OUTPUT", outArea, juce::Justification::centred);
			}

			const float gOutDb = (float) globalOutputSlider.getValue();
			juce::String outTxt = formatGainFaderDb (gOutDb);
			if (footerExpanded_)
			{
				const auto valArea = makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), outBounds);
				tryDrawLegend (valArea, outTxt + " OUT", outTxt + " OUT", outTxt);
			}
			else
			{
				const auto valArea = makeFooterValueArea (outBounds, kFooterDbValueWidthPx);
				g.drawText (outTxt, valArea, juce::Justification::centredLeft);
			}
		}

		// LIM THRESHOLD label + value (right of bar)
		if (limThresholdSlider.isVisible())
		{
			const auto limBounds = limThresholdSlider.getBounds();
			if (! footerExpanded_)
			{
				const auto limArea = limBounds.withHeight (16).translated (0, -18);
				g.drawText ("LIM", limArea, juce::Justification::centred);
			}

			const float limDb = (float) limThresholdSlider.getValue();
			juce::String limTxt = (limDb <= -35.9f) ? "-36.0 dB" : juce::String (limDb, 1) + " dB";
			if (footerExpanded_)
			{
				const auto valArea = makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), limBounds);
				tryDrawLegend (valArea, limTxt + " LIM", limTxt + " LIM", limTxt);
			}
			else
			{
				const auto valArea = makeFooterValueArea (limBounds, kFooterDbValueWidthPx);
				g.drawText (limTxt, valArea, juce::Justification::centredLeft);
			}
		}

		drawFooterComboLabel (limModeCombo, "LIMIT");
		drawFooterComboLabel (invPolCombo, "INV POL");
		drawFooterComboLabel (invStrCombo, "INV STR");
	}

	// Per-loader MODE IN / MODE OUT labels (only when that loader is expanded)
	{
		auto drawModeLabels = [&] (juce::ComboBox& modeIn, juce::ComboBox& modeOut, juce::ComboBox& sumBus, juce::ComboBox& filterPos, juce::ToggleButton& enableBtn)
		{
			if (! modeIn.isVisible()) return;
			const float alpha = enableBtn.getToggleState() ? 1.0f : 0.35f;
			g.setColour (activeScheme.text.withAlpha (alpha));
			const auto font = juce::Font (juce::FontOptions (17.0f).withStyle ("Bold"));
			g.setFont (font);
			const auto miArea = modeIn.getBounds().withHeight (20).translated (0, -21);
			const auto moArea = modeOut.getBounds().withHeight (20).translated (0, -21);
			const auto sbArea = sumBus.getBounds().withHeight (20).translated (0, -21);
			const auto fpArea = filterPos.getBounds().withHeight (20).translated (0, -21);
			const float comboW = (float) modeIn.getWidth();
			juce::GlyphArrangement ga;
			ga.addLineOfText (font, "MODE OUT", 0.0f, 0.0f);
			const bool useShort = ga.getBoundingBox (0, -1, false).getWidth() > comboW;
			g.drawText (useShort ? "IN"  : "MODE IN",  miArea, juce::Justification::centred);
			g.drawText (useShort ? "OUT" : "MODE OUT", moArea, juce::Justification::centred);
			g.drawText (useShort ? "SUM" : "SUM BUS",  sbArea, juce::Justification::centred);
			g.drawText (useShort ? "F/T" : "F / T",    fpArea, juce::Justification::centred);
		};
		if (ioExpandedA_) drawModeLabels (modeInComboA, modeOutComboA, sumBusComboA, filterPosComboA, enableButtonA);
		if (ioExpandedB_) drawModeLabels (modeInComboB, modeOutComboB, sumBusComboB, filterPosComboB, enableButtonB);
		if (ioExpandedC_) drawModeLabels (modeInComboC, modeOutComboC, sumBusComboC, filterPosComboC, enableButtonC);
	}

	// Draw gear icon (in paint, like other TR plugins)
	{
		if (cachedInfoGearPath.isEmpty())
			updateInfoIconCache();

		g.setColour (activeScheme.text);
		g.fillPath (cachedInfoGearPath);
		g.strokePath (cachedInfoGearPath, juce::PathStrokeType (1.0f));

		g.setColour (activeScheme.bg);
		g.fillEllipse (cachedInfoGearHole);
	}

	// (oversampling label removed - now in footer combo)

	// Draw value legends for all bar sliders
	{
		g.setFont (kBoldFont40());

		for (int loader = 0; loader < 3; ++loader)
		{
			auto refs = getLoaderRefs (loader);
			const bool enabled = refs.enableBtn.getToggleState();
			const int colR = columnRight_[loader];

			juce::Slider* loaderSliders[kNumCachedParams] = {
				&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt, &refs.series,
				&refs.pan, &refs.fred, &refs.pos, &refs.mix,
				&refs.satDrive, &refs.satGirth, &refs.satMod, &refs.satBias, &refs.satSag, &refs.detail, &refs.instability, &refs.delay
			};

			for (int i = 0; i < kNumCachedParams; ++i)
			{
				if (loaderSliders[i]->isVisible())
				{
					const bool sliderEnabled = enabled && loaderSliders[i]->isEnabled();
					g.setColour (sliderEnabled ? activeScheme.text : activeScheme.text.withAlpha (0.35f));
					const auto valueArea = getValueAreaFor (loaderSliders[i]->getBounds(), colR);
					cachedValueAreas_[(size_t) (loader * kNumCachedParams + i)] = valueArea;
					tryDrawLegend (valueArea, cachedTexts[loader][i].full,
					               cachedTexts[loader][i].short_, cachedTexts[loader][i].intOnly);
				}
			}

			if (refs.filterBar.isVisible())
			{
				const auto filterValueArea = getValueAreaFor (refs.filterBar.getBounds(), colR);
				g.drawText ("FILTER", filterValueArea, juce::Justification::centredLeft);
			}
		}
	}
}

void SATTRAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
	// Skip toggle bar when prompt overlay is active (it would paint over the prompt)
	if (promptOverlayActive)
		return;

	const bool tooltipVisible = tooltipWindow != nullptr && tooltipWindow->isVisible();
	if (tooltipVisible)
	{
		g.saveState();
		g.excludeClipRegion (tooltipWindow->getBounds().expanded (2));
	}

	// -- Per-loader toggle bars (triangle + rounded horizontal bar) --
	auto drawToggleBar = [&] (const juce::Rectangle<int>& area, bool expanded)
	{
		if (area.isEmpty()) return;
		const float barRadius = (float) area.getHeight() * 0.3f;
		g.setColour (activeScheme.fg.withAlpha (0.25f));
		g.fillRoundedRectangle (area.toFloat(), barRadius);

		const float triH = (float) area.getHeight() * 0.8f;
		const float triW = triH * 1.125f;
		const float cx = (float) area.getCentreX();
		const float cy = (float) area.getCentreY();

		juce::Path tri;
		if (expanded)
		{
			tri.addTriangle (cx - triW * 0.5f, cy + triH * 0.35f,
			                 cx + triW * 0.5f, cy + triH * 0.35f,
			                 cx,               cy - triH * 0.35f);
		}
		else
		{
			tri.addTriangle (cx - triW * 0.5f, cy - triH * 0.35f,
			                 cx + triW * 0.5f, cy - triH * 0.35f,
			                 cx,               cy + triH * 0.35f);
		}
		g.setColour (activeScheme.text);
		g.fillPath (tri);
	};

	drawToggleBar (cachedToggleBarAreaA_, ioExpandedA_);
	drawToggleBar (cachedToggleBarAreaB_, ioExpandedB_);
	drawToggleBar (cachedToggleBarAreaC_, ioExpandedC_);

	auto makeLoaderTabLabel = [] (int startIndex, int visibleCount)
	{
		static constexpr const char* singleLabels[] = { "A", "B", "C" };
		if (visibleCount <= 1)
			return juce::String (singleLabels[juce::jlimit (0, 2, startIndex)]);

		if (visibleCount == 2)
			return startIndex <= 0 ? juce::String ("AB") : juce::String ("BC");

		return juce::String();
	};

	for (int i = 0; i < cachedLoaderTabCount_; ++i)
	{
		const auto area = cachedLoaderTabAreas_[i];
		if (area.isEmpty())
			continue;

		const bool selected = cachedLoaderTabStartIndices_[i] == firstVisibleLoaderIndex_;
		const auto tabBounds = area.toFloat();
		const float radius = 8.0f;

		g.setColour (selected ? activeScheme.text : activeScheme.bg);
		g.fillRoundedRectangle (tabBounds, radius);
		g.setColour (activeScheme.text);
		g.drawRoundedRectangle (tabBounds.reduced (1.0f), radius, 2.0f);

		g.setColour (selected ? activeScheme.bg : activeScheme.text);
		g.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
		g.drawFittedText (makeLoaderTabLabel (cachedLoaderTabStartIndices_[i], visibleLoaderCount_),
		                  area.reduced (3, 0),
		                  juce::Justification::centred,
		                  1);
	}

	if (! cachedFooterRailArea_.isEmpty())
	{
		const auto tabBounds = cachedFooterRailArea_.toFloat();
		const float radius = (float) cachedFooterRailArea_.getHeight() * 0.28f;
		g.setColour (footerExpanded_ ? activeScheme.text : activeScheme.bg);
		g.fillRoundedRectangle (tabBounds, radius);
		g.setColour (activeScheme.text);
		g.drawRoundedRectangle (tabBounds.reduced (1.0f), radius, 2.0f);

		g.setColour (footerExpanded_ ? activeScheme.bg : activeScheme.text);
		g.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
		g.drawFittedText ("GLOBAL",
		                  cachedFooterRailArea_.reduced (8, 0),
		                  juce::Justification::centred,
		                  1);
	}

	if (tooltipVisible)
		g.restoreState();
}

// ----------------------------------------------------------------
//  Resized & Layout
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::resized()
{
	if (! applyingCompactResize_)
	{
		const int snappedW = getCompactTargetWidthForLoaderCount (getMaxVisibleLoaderCountForWidth (getWidth()));
		if (getWidth() != snappedW || getHeight() != kCompactFixedHeightPx)
		{
			juce::ScopedValueSetter<bool> guard (applyingCompactResize_, true);
			setSize (snappedW, kCompactFixedHeightPx);
			return;
		}
	}

	// Persist window size to processor
	audioProcessor.setUiEditorSize (getWidth(), getHeight());

	auto bounds = getLocalBounds();
	clearCompactRailAreas();
	cachedToggleBarAreaA_ = {};
	cachedToggleBarAreaB_ = {};
	cachedToggleBarAreaC_ = {};
	cachedValueAreas_.fill (juce::Rectangle<int>());
	
	// Header (title area + buttons)
	auto header = bounds.removeFromTop (40);
	cachedHeaderTitleX_ = getCompactLoaderContentSideInsetPx();

	// Place ALIGN button in header, next to title
	{
		const int titleX = cachedHeaderTitleX_;
		constexpr int titleY = 12;
		constexpr int titleH = 32;
		constexpr int titleW = 100; // approximate width of "SAT-TR" text
		const int alignW = 60;
		const int alignH = 24;
		const int alignX = titleX + titleW + 8;
		const int alignY = titleY + (titleH - alignH) / 2;
		alignButton.setBounds (alignX, alignY, alignW, alignH);
	}

	// Bottom rail toggles between loader view and the global footer view.
	auto footerRailSlot = bounds.removeFromBottom (kCompactFooterRailSlotHeightPx);
	const int footerTabW = juce::jmin (kCompactFooterRailWidthPx,
	                                   juce::jmax (0, footerRailSlot.getWidth() - (kCompactFooterRailXInsetPx * 2)));
	const int footerTabH = juce::jmin (kCompactFooterRailHeightPx, footerRailSlot.getHeight());
	cachedFooterRailArea_ = juce::Rectangle<int> (footerRailSlot.getCentreX() - (footerTabW / 2),
	                                              footerRailSlot.getCentreY() - (footerTabH / 2),
	                                              footerTabW,
	                                              footerTabH);

	if (footerExpanded_)
	{
		for (int loader = 0; loader < 3; ++loader)
			hideLoaderSection (loader);
		layoutFooterControls (bounds);
	}
	else
	{
		hideFooterControls();
		for (int loader = 0; loader < 3; ++loader)
			hideLoaderSection (loader);

		// Split remaining area into the number of loader columns supported by the
		// current width. Hidden loaders keep processing; only their UI is hidden.
		visibleLoaderCount_ = getMaxVisibleLoaderCountForWidth (bounds.getWidth());
		firstVisibleLoaderIndex_ = juce::jlimit (0,
		                                         kCompactMaxVisibleLoaders - visibleLoaderCount_,
		                                         firstVisibleLoaderIndex_);

		auto loaderBounds = bounds;
		if (visibleLoaderCount_ < kCompactMaxVisibleLoaders)
		{
			cachedLoaderTabCount_ = kCompactMaxVisibleLoaders - visibleLoaderCount_ + 1;
			const int totalTabsH = cachedLoaderTabCount_ * kCompactLoaderTabHeightPx
			                     + (cachedLoaderTabCount_ - 1) * kCompactLoaderTabGapPx;
			const int railTop = bounds.getY() + kCompactLoaderTabYInsetPx;
			const int railH = juce::jmax (0, bounds.getHeight() - kCompactLoaderTabYInsetPx * 2);
			const int firstY = railTop + juce::jmax (0, (railH - totalTabsH) / 2);
			const int x = bounds.getRight() - kCompactLoaderTabWidthPx;

			for (int i = 0; i < cachedLoaderTabCount_; ++i)
			{
				cachedLoaderTabStartIndices_[i] = i;
				cachedLoaderTabAreas_[i] = { x,
				                             firstY + i * (kCompactLoaderTabHeightPx + kCompactLoaderTabGapPx),
				                             kCompactLoaderTabWidthPx,
				                             kCompactLoaderTabHeightPx };
			}
		}

		const int fixedLoadersW = juce::jmin (loaderBounds.getWidth(),
		                                      visibleLoaderCount_ * kCompactLoaderColumnWidthPx);
		auto fixedLoaderBounds = loaderBounds.withWidth (fixedLoadersW);
		for (int viewSlot = 0; viewSlot < visibleLoaderCount_; ++viewSlot)
		{
			const int loader = firstVisibleLoaderIndex_ + viewSlot;
			auto loaderArea = (viewSlot == visibleLoaderCount_ - 1) ? fixedLoaderBounds
			                                                       : fixedLoaderBounds.removeFromLeft (kCompactLoaderColumnWidthPx);
			columnLeft_[loader] = loaderArea.getX() + getCompactLoaderContentSideInsetPx();
			columnRight_[loader] = loaderArea.getRight() - getCompactLoaderContentSideInsetPx()
			                     - kCompactLoaderTabSafeInsetRightPx;
			layoutLoaderSection (loaderArea, loader);
		}
	}

	promptOverlay.setBounds (getLocalBounds());

	legendDirty = true;
	updateInfoIconCache();
}

void SATTRAudioProcessorEditor::layoutLoaderSection (juce::Rectangle<int> area, int loaderIndex)
{
	const int margin = 10;
	const int buttonH = 30;
	const int gap = 5;
	const int toggleBarH = 20;
	constexpr int toggleToFirstControlGapPx = 18; // Matches simple-plugin compact rhythm at 752 px.

	auto pick = [&] (auto& a, auto& b, auto& c) -> auto& { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	const int railSafeInsetPx = juce::jmax (0, getCompactLoaderContentSideInsetPx() - margin);
	area.reduce (railSafeInsetPx, 0);
	area.reduce (margin, margin);
	area.removeFromRight (kCompactLoaderTabSafeInsetRightPx);

	// Enable checkbox at top
	auto& enableBtn = pick (enableButtonA, enableButtonB, enableButtonC);
	enableBtn.setBounds (area.removeFromTop (buttonH));
	enableBtn.setVisible (true);
	area.removeFromTop (gap);

	// Toggle bar area - full column width (union computed in resized)
	auto toggleBarArea = area.removeFromTop (toggleBarH);
	if (loaderIndex == 0)
		cachedToggleBarAreaA_ = toggleBarArea;
	else if (loaderIndex == 1)
		cachedToggleBarAreaB_ = toggleBarArea;
	else
		cachedToggleBarAreaC_ = toggleBarArea;
	area.removeFromTop (toggleToFirstControlGapPx);

	const int sliderW = static_cast<int> (area.getWidth() * 0.50f);

	// Component references
	auto& hp    = pick (hpFreqSliderA,  hpFreqSliderB,  hpFreqSliderC);
	auto& lp    = pick (lpFreqSliderA,  lpFreqSliderB,  lpFreqSliderC);
	auto& in_   = pick (inSliderA,      inSliderB,      inSliderC);
	auto& out   = pick (outSliderA,     outSliderB,     outSliderC);
	auto& tilt  = pick (tiltSliderA,    tiltSliderB,    tiltSliderC);
	auto& series = pick (seriesSliderA,   seriesSliderB,   seriesSliderC);
	auto& detail = pick (detailSliderA,   detailSliderB,   detailSliderC);
	auto& pan   = pick (panSliderA,     panSliderB,     panSliderC);
	auto& fred  = pick (fredSliderA,    fredSliderB,    fredSliderC);
	auto& pos   = pick (posSliderA,     posSliderB,     posSliderC);
	auto& mix   = pick (mixSliderA,     mixSliderB,     mixSliderC);
	auto& inv   = pick (invButtonA,     invButtonB,     invButtonC);
	auto& chaos = pick (chaosButtonA,   chaosButtonB,   chaosButtonC);
	auto& chaosFilter = pick (chaosFilterButtonA, chaosFilterButtonB, chaosFilterButtonC);
	auto& filterBar  = pick (filterBarA_,      filterBarB_,      filterBarC_);
	auto& modeInCmb  = pick (modeInComboA,     modeInComboB,     modeInComboC);
	auto& modeOutCmb = pick (modeOutComboA,    modeOutComboB,    modeOutComboC);
	auto& sumBusCmb  = pick (sumBusComboA,     sumBusComboB,     sumBusComboC);
	auto& filterPosCmb = pick (filterPosComboA, filterPosComboB, filterPosComboC);
	auto& chaosDisp  = pick (chaosDisplayA,    chaosDisplayB,    chaosDisplayC);
	auto& expBtn     = pick (expButtonA,       expButtonB,       expButtonC);
	auto& expDisp    = pick (expDisplayA,      expDisplayB,      expDisplayC);

	const bool expanded = (loaderIndex == 0) ? ioExpandedA_
	                     : (loaderIndex == 1) ? ioExpandedB_
	                     :                      ioExpandedC_;

	constexpr int loaderHeaderBlockH = 72;
	const int modeComboLabelOffset = 21;
	const int modeComboGapY = 8;
	const int checkH = 42;

	// Both views share one header + parameter grid so the algorithm/RAW anchor
	// and the first parameter row line up exactly when toggling compact sections.
	const int numRows = 10;
	const int parameterRows = numRows - 1;
	const int parameterGaps = parameterRows - 1;
	const int compactBottomSpacer = modeComboLabelOffset + gap * 2;
	const int bottomSpacer = expanded ? (gap * 2) : compactBottomSpacer;
	auto contentArea = area;
	auto checkArea = contentArea.removeFromBottom (checkH);
	contentArea.removeFromBottom (bottomSpacer);

	// Use one shared row pitch for both views; otherwise the IN/OUT page and
	// the normal page drift by a few pixels even though they share anchors.
	const int sharedContentH = area.getHeight() - checkH - compactBottomSpacer;
	const int sliderH = juce::jmax (18, (sharedContentH - loaderHeaderBlockH - (parameterGaps * gap)) / parameterRows);
	const int visualSliderH = juce::jlimit (24, 32, sliderH);
	const int visualComboH = 38;
	const int visualAlgorithmH = 42;
	auto fitControlHeight = [] (juce::Rectangle<int> r, int h)
	{
		return r.withSizeKeepingCentre (r.getWidth(), juce::jmin (h, r.getHeight()));
	};

	if (expanded)
	{
		// -- Expanded IO view: algorithm/RAW anchor, IN, OUT, TILT, FILTER, PAN, MIX, MODE IN/OUT, CHAOS --

		auto& satTypeCmb = pick (satTypeComboA, satTypeComboB, satTypeComboC);
		auto& rawBtn     = pick (rawButtonA,    rawButtonB,    rawButtonC);
		auto headerBlock = contentArea.removeFromTop (loaderHeaderBlockH);
		auto comboRow = headerBlock.removeFromTop (sliderH);
		satTypeCmb.setBounds (fitControlHeight (comboRow.removeFromLeft (sliderW), visualAlgorithmH));
		satTypeCmb.setVisible (true);
		constexpr int rawGap = 6;
		rawBtn.setBounds (fitControlHeight ({ comboRow.getX() + rawGap, comboRow.getY(), comboRow.getWidth() - rawGap, sliderH }, visualSliderH));
		rawBtn.setVisible (true);

		auto sliderRow = contentArea.removeFromTop (sliderH);
		in_.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		in_.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		out.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		out.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		tilt.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		tilt.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		filterBar.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		filterBar.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		pan.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		pan.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		mix.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		mix.setVisible (true);
		contentArea.removeFromTop (gap);

		// MODE IN / MODE OUT / F/T / SUM BUS combos (2x2 grid, same height as sliders)
		const int modeComboW = (sliderW - gap) / 2;
		const int comboSlotH = juce::jlimit (38, 48, sliderH + 14);
		const int modeComboBlockH = modeComboLabelOffset + comboSlotH
		                          + modeComboGapY + modeComboLabelOffset + comboSlotH;
		const int modeBlockTopLimit = mix.getBottom() + gap;
		const int modeBlockBottomLimit = checkArea.getY() - gap;
		const int availableModeBlockH = juce::jmax (modeComboBlockH, modeBlockBottomLimit - modeBlockTopLimit);
		const int modeVisualTop = modeBlockTopLimit + juce::jmax (0, (availableModeBlockH - modeComboBlockH) / 2);
		const int modeY = modeVisualTop + modeComboLabelOffset;
		const int modeX = contentArea.getX();
		auto modeRow1 = juce::Rectangle<int> (modeX, modeY, sliderW, comboSlotH);
		modeInCmb.setBounds  (fitControlHeight ({ modeRow1.getX(), modeRow1.getY(), modeComboW, comboSlotH }, visualComboH));
		modeOutCmb.setBounds (fitControlHeight ({ modeRow1.getX() + modeComboW + gap, modeRow1.getY(), modeComboW, comboSlotH }, visualComboH));
		modeInCmb.setVisible (true);
		modeOutCmb.setVisible (true);
		auto modeRow2 = juce::Rectangle<int> (modeX, modeY + comboSlotH + modeComboGapY + modeComboLabelOffset, sliderW, comboSlotH);
		filterPosCmb.setBounds (fitControlHeight ({ modeRow2.getX(), modeRow2.getY(), modeComboW, comboSlotH }, visualComboH));
		sumBusCmb.setBounds    (fitControlHeight ({ modeRow2.getX() + modeComboW + gap, modeRow2.getY(), modeComboW, comboSlotH }, visualComboH));
		filterPosCmb.setVisible (true);
		sumBusCmb.setVisible (true);

		// CHSF + CHSD checkboxes - each button owns its full legend hit area.
		constexpr int valuePadPx = 8;
		const int chsfW = sliderW;
		const int chsdX = checkArea.getX() + sliderW + valuePadPx;
		const int chsdW = checkArea.getRight() - chsdX;
		chaosFilter.setBounds (checkArea.getX(), checkArea.getY(), chsfW, checkH);
		chaos.setBounds (chsdX, checkArea.getY(), chsdW, checkH);
		chaosFilter.setVisible (true);
		chaos.setVisible (true);
		chaosDisp.setBounds (0, 0, 0, 0);
		chaosDisp.setVisible (false);

		// Hide collapsed-only controls
		hp.setVisible (false);     lp.setVisible (false);
		series.setVisible (false); fred.setVisible (false);
		pos.setVisible (false);
		inv.setVisible (false);
		expBtn.setVisible (false);  expDisp.setVisible (false);

		// Hide sat sliders (collapsed-only)
		auto& drive = pick (satDriveSliderA, satDriveSliderB, satDriveSliderC);
		auto& girthS = pick (satGirthSliderA, satGirthSliderB, satGirthSliderC);
		auto& sModS  = pick (satModSliderA,   satModSliderB,   satModSliderC);
		auto& sBiasS = pick (satBiasSliderA,  satBiasSliderB,  satBiasSliderC);
		auto& sSagS  = pick (satSagSliderA,   satSagSliderB,   satSagSliderC);
		auto& detailS = pick (detailSliderA, detailSliderB, detailSliderC);
		auto& sInstabilityS  = pick (instabilitySliderA,      instabilitySliderB,      instabilitySliderC);
		auto& delayS = pick (delaySliderA,    delaySliderB,    delaySliderC);
		drive.setVisible (false);  girthS.setVisible (false);
		sModS.setVisible (false);  sBiasS.setVisible (false);  sSagS.setVisible (false);
		detailS.setVisible (false); sInstabilityS.setVisible (false);  delayS.setVisible (false);
	}
	else
	{
		// -- Collapsed main view: SatType combo + sat sliders + SERIES + DTL + INST + delay --

		// SatType combo (same height as sliders) + RAW checkbox
		auto& satTypeCmb = pick (satTypeComboA, satTypeComboB, satTypeComboC);
		auto& rawBtn     = pick (rawButtonA,    rawButtonB,    rawButtonC);
		auto headerBlock = contentArea.removeFromTop (loaderHeaderBlockH);
		auto comboRow = headerBlock.removeFromTop (sliderH);
		satTypeCmb.setBounds (fitControlHeight (comboRow.removeFromLeft (sliderW), visualAlgorithmH));
		satTypeCmb.setVisible (true);
		// RAW checkbox sits to the right of the combo
		constexpr int rawGap = 6;
		rawBtn.setBounds (fitControlHeight ({ comboRow.getX() + rawGap, comboRow.getY(), comboRow.getWidth() - rawGap, sliderH }, visualSliderH));
		rawBtn.setVisible (true);

		// Saturation sliders
		auto& drive = pick (satDriveSliderA, satDriveSliderB, satDriveSliderC);
		auto& girth = pick (satGirthSliderA, satGirthSliderB, satGirthSliderC);
		auto& sMod  = pick (satModSliderA,   satModSliderB,   satModSliderC);
		auto& sBias = pick (satBiasSliderA,  satBiasSliderB,  satBiasSliderC);
		auto& sSag  = pick (satSagSliderA,   satSagSliderB,   satSagSliderC);
		auto& sInstability  = pick (instabilitySliderA,      instabilitySliderB,      instabilitySliderC);

		auto sliderRow = contentArea.removeFromTop (sliderH);
		drive.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		drive.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		girth.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		girth.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		sMod.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		sMod.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		sBias.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		sBias.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		sSag.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		sSag.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		series.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		series.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		detail.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		detail.setVisible (true);
		contentArea.removeFromTop (gap);

		sliderRow = contentArea.removeFromTop (sliderH);
		sInstability.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		sInstability.setVisible (true);
		contentArea.removeFromTop (gap);

		// Delay slider (read-only, driven by ALIGN)
		auto& delayS = pick (delaySliderA, delaySliderB, delaySliderC);
		sliderRow = contentArea.removeFromTop (sliderH);
		delayS.setBounds (fitControlHeight (sliderRow.removeFromLeft (sliderW), visualSliderH));
		delayS.setVisible (true);

		// Checkbox: INV + EXP - each button owns its full legend hit area.
		constexpr int valuePadPx2 = 8;
		const int invW = sliderW;
		const int expX = checkArea.getX() + sliderW + valuePadPx2;
		const int expW = checkArea.getRight() - expX;
		inv.setBounds  (checkArea.getX(), checkArea.getY(), invW, checkH);
		inv.setVisible (true);
		expBtn.setBounds (expX, checkArea.getY(), expW, checkH);
		expBtn.setVisible (true);
		expDisp.setBounds (0, 0, 0, 0);
		expDisp.setVisible (false);

		// Hide expanded-only controls
		in_.setVisible (false);        out.setVisible (false);     tilt.setVisible (false);
		filterBar.setVisible (false);  pan.setVisible (false);
		mix.setVisible (false);
		modeInCmb.setVisible (false);    modeOutCmb.setVisible (false);    sumBusCmb.setVisible (false);    filterPosCmb.setVisible (false);
		chaos.setVisible (false);      chaosFilter.setVisible (false);  chaosDisp.setVisible (false);
		hp.setVisible (false);         lp.setVisible (false);      fred.setVisible (false);
		pos.setVisible (false);
	}
}

// ----------------------------------------------------------------
//  Loader enabled/disabled visual state
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::updateLoaderEnabledState (int loaderIndex)
{
	auto r = getLoaderRefs (loaderIndex);

	const bool enabled = r.enableBtn.getToggleState();
	const float alpha = enabled ? 1.0f : 0.35f;
	const bool interactive = enabled;

	juce::Component* components[] = {
		&r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		&r.series, &r.pan, &r.fred, &r.pos,
		&r.inv, &r.chaos, &r.chaosFilter, &r.chaosDisp,
		&r.exp, &r.expDisp,
		&r.modeIn, &r.modeOut, &r.sumBus, &r.filterPos,
		&r.filterBar, &r.mix,
		&r.satType, &r.raw, &r.satDrive, &r.satGirth, &r.satMod, &r.satBias, &r.satSag, &r.detail, &r.instability, &r.delay
	};

	for (auto* c : components)
	{
		c->setAlpha (alpha);
		c->setEnabled (interactive);
	}

	// Also sync sat control enablement (CLEAN disables sat knobs)
	updateSatControlsEnabledState (loaderIndex);

	repaint();
}

void SATTRAudioProcessorEditor::updateSatControlsEnabledState (int loaderIndex)
{
	auto r = getLoaderRefs (loaderIndex);

	// Check if the loader itself is enabled first
	const bool loaderEnabled = r.enableBtn.getToggleState();

	const bool isClean = (getSelectedSatTypeModelIndex (r.satType) == static_cast<int> (SatEngine::Model::Clean));

	// Sat-specific controls should be interactive only when loader enabled AND not CLEAN.
	// Modal prompts are blocked by promptOverlay itself, so they must not alter visual alpha.
	const bool satInteractive = loaderEnabled && ! isClean;
	const float satAlpha = satInteractive ? 1.0f : 0.35f;

	juce::Component* satControls[] = {
		&r.satDrive, &r.satGirth, &r.satMod, &r.satBias, &r.satSag, &r.series, &r.detail, &r.instability, &r.raw
	};

	for (auto* c : satControls)
	{
		c->setAlpha (satAlpha);
		c->setEnabled (satInteractive);
	}
}

// ----------------------------------------------------------------
//  Callbacks
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::timerCallback()
{
	// Sync filter bars from processor
	filterBarA_.updateFromProcessor();
	filterBarB_.updateFromProcessor();
	filterBarC_.updateFromProcessor();

	// Keep dual mix bar markers up to date + visibility swap
	{
		const float prevDry = dualMixBar_.getDryLevel();
		const float prevWet = dualMixBar_.getWetLevel();
		dualMixBar_.updateFromProcessor();
		const bool isSendMode = mixModeCombo.getSelectedId() == 2;

		// Refresh legend when levels change in SEND mode
		if (isSendMode && (dualMixBar_.getDryLevel() != prevDry || dualMixBar_.getWetLevel() != prevWet))
		{
			refreshLegendTextCache();
			repaint();
		}

		if (globalMixSlider.isVisible() == isSendMode)
		{
			resized();
			refreshLegendTextCache();
			repaint();
		}
	}

	// CRT effect animation
	if (crtEnabled)
	{
		crtTime += 1.0f / (float) kCrtTimerHz;
		crtEffect.setTime (crtTime);
		repaint();
	}
}

void SATTRAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
	juce::ignoreUnused (slider);
	legendDirty = true;
	repaint();
}

void SATTRAudioProcessorEditor::buttonClicked (juce::Button* button)
{
	juce::ignoreUnused (button);
}

void SATTRAudioProcessorEditor::comboBoxChanged (juce::ComboBox* combo)
{
	if (combo == &satTypeComboA) { commitSatTypeComboSelection (0); updateSatControlsEnabledState (0); }
	else if (combo == &satTypeComboB) { commitSatTypeComboSelection (1); updateSatControlsEnabledState (1); }
	else if (combo == &satTypeComboC) { commitSatTypeComboSelection (2); updateSatControlsEnabledState (2); }

	legendDirty = true;
	repaint();
}

void SATTRAudioProcessorEditor::applyCrtState (bool enabled)
{
	crtEnabled = enabled;
	crtEffect.setEnabled (crtEnabled);
	setComponentEffect (crtEnabled ? &crtEffect : nullptr);
	crtTime = 0.0f;
	stopTimer();
	startTimerHz (crtEnabled ? kCrtTimerHz : kIdleTimerHz);
}

void SATTRAudioProcessorEditor::parameterChanged (const juce::String& paramID, float newValue)
{
	if (paramID == SATTRAudioProcessor::kParamUiFxTail)
	{
		applyCrtState (newValue > 0.5f);
		return;
	}

	const char* enableIds[] = { SATTRAudioProcessor::kParamEnableA,
	                            SATTRAudioProcessor::kParamEnableB,
	                            SATTRAudioProcessor::kParamEnableC };
	for (int i = 0; i < 3; ++i)
	{
		if (paramID == enableIds[i])
		{
			const int idx = i;
			juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<SATTRAudioProcessorEditor> (this), idx] ()
			{
				if (safeThis != nullptr)
					safeThis->updateLoaderEnabledState (idx);
			});
			return;
		}
	}

	const char* satTypeIds[] = { SATTRAudioProcessor::kParamSatTypeA,
	                             SATTRAudioProcessor::kParamSatTypeB,
	                             SATTRAudioProcessor::kParamSatTypeC };
	for (int i = 0; i < 3; ++i)
	{
		if (paramID == satTypeIds[i])
		{
			const int idx = i;
			juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<SATTRAudioProcessorEditor> (this), idx] ()
			{
				if (safeThis != nullptr)
				{
					safeThis->syncSatTypeComboSelection (idx);
					safeThis->updateSatControlsEnabledState (idx);
				}
			});
			return;
		}
	}
}

// ----------------------------------------------------------------
//  Mouse Events
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
	const auto p = e.getEventRelativeTo (this).getPosition();

	if (! e.mods.isPopupMenu())
	{
		if (cachedFooterRailArea_.contains (p))
		{
			setFooterExpanded (! footerExpanded_);
			return;
		}

		for (int i = 0; i < cachedLoaderTabCount_; ++i)
		{
			if (cachedLoaderTabAreas_[i].contains (p))
			{
				setFirstVisibleLoaderIndex (cachedLoaderTabStartIndices_[i]);
				return;
			}
		}
	}

	// Toggle IO section expand/collapse (per-loader independent)
	{
		struct { juce::Rectangle<int>& area; bool& state; int idx; } bars[] = {
			{ cachedToggleBarAreaA_, ioExpandedA_, 0 },
			{ cachedToggleBarAreaB_, ioExpandedB_, 1 },
			{ cachedToggleBarAreaC_, ioExpandedC_, 2 }
		};
		for (auto& b : bars)
		{
			if (b.area.contains (p))
			{
				b.state = ! b.state;
				audioProcessor.setUiIoExpanded (b.idx, b.state);
				resized();
				repaint();
				return;
			}
		}
	}

	// Click on gear icon opens info popup (with Graphics button inside)
	if (getInfoIconArea().contains (p))
	{
		openInfoPopup();
		return;
	}

	// Click on OS label - no longer needed (now a footer combo)
	// (removed)

	// CHAOS checkboxes: left-click is handled by the button, right-click opens amount/speed prompt.
	{
		juce::ToggleButton* enableBtns[]      = { &enableButtonA,  &enableButtonB,  &enableButtonC };
		juce::ToggleButton* chaosBtns[]       = { &chaosButtonA,   &chaosButtonB,   &chaosButtonC };
		juce::ToggleButton* chaosFilterBtns[] = { &chaosFilterButtonA, &chaosFilterButtonB, &chaosFilterButtonC };

		for (int i = 0; i < 3; ++i)
		{
			if (! enableBtns[i]->getToggleState())
				continue;

			const bool hitFilter = chaosFilterBtns[i]->isVisible()
				&& chaosFilterBtns[i]->getBounds().contains (p);

			const bool hitDelay = !hitFilter
				&& chaosBtns[i]->isVisible()
				&& chaosBtns[i]->getBounds().contains (p);

			if (hitFilter)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, true);
				return;
			}
			if (hitDelay)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, false);
				return;
			}
		}
	}

	// EXP checkbox: left-click is handled by the button, right-click opens EXP prompt.
	{
		juce::ToggleButton* enableBtns[] = { &enableButtonA,  &enableButtonB,  &enableButtonC };
		juce::ToggleButton* expBtns[]    = { &expButtonA,     &expButtonB,     &expButtonC };
		juce::Label*        expDisps[]   = { &expDisplayA,    &expDisplayB,    &expDisplayC };

		for (int i = 0; i < 3; ++i)
		{
			if (! enableBtns[i]->getToggleState())
				continue;

			const bool hitExp = expBtns[i]->isVisible()
				&& (expBtns[i]->getBounds().contains (p)
					|| (expDisps[i]->isVisible() && expDisps[i]->getBounds().contains (p)));

			if (hitExp)
			{
				if (e.mods.isPopupMenu())
					openExpPrompt (i);
				return;
			}
		}
	}

	// Right-click on value area opens numeric entry popup
	if (e.mods.isPopupMenu())
	{
		if (dualMixBar_.isVisible()
			&& (footerExpanded_ ? makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), dualMixBar_.getBounds())
			                    : makeFooterValueArea (dualMixBar_.getBounds(), kFooterMixValueWidthPx)).contains (p))
		{
			openMixSendPrompt();
			return;
		}

		if (auto* slider = getSliderForValueAreaPoint (p))
		{
			openNumericEntryPopupForSlider (*slider);
			return;
		}
	}

	// Window dragging (if applicable)
	if (e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && e.mods.isAltDown()))
	{
		isDraggingWindow = true;
		dragStartPos = e.getScreenPosition();
	}
}

void SATTRAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent&)
{
}

void SATTRAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
	if (isDraggingWindow && e.mods.isLeftButtonDown())
	{
		// Window dragging handled by OS in plugin contexts
	}
}

// ----------------------------------------------------------------
//  TR-style label/value system helpers
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::setupBar (juce::Slider& s)
{
	s.setSliderStyle (juce::Slider::LinearBar);
	s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
	s.setPopupDisplayEnabled (false, false, this);
	s.setTooltip (juce::String());
	s.setPopupMenuEnabled (false);
	s.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
	s.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
	s.setColour (juce::Slider::thumbColourId, juce::Colours::transparentBlack);
}

int SATTRAudioProcessorEditor::getCompactTargetWidthForLoaderCount (int loaderCount) noexcept
{
	const int safeCount = juce::jlimit (kCompactMinVisibleLoaders, kCompactMaxVisibleLoaders, loaderCount);
	return safeCount * kCompactLoaderColumnWidthPx;
}

int SATTRAudioProcessorEditor::getMaxVisibleLoaderCountForWidth (int width) noexcept
{
	const int oneLoaderW = getCompactTargetWidthForLoaderCount (1);
	const int twoLoadersW = getCompactTargetWidthForLoaderCount (2);
	const int threeLoadersW = getCompactTargetWidthForLoaderCount (3);
	const int twoLoaderThreshold = (oneLoaderW + twoLoadersW) / 2;
	const int threeLoaderThreshold = (twoLoadersW + threeLoadersW) / 2;

	if (width >= threeLoaderThreshold)
		return 3;
	if (width >= twoLoaderThreshold)
		return 2;
	return 1;
}

void SATTRAudioProcessorEditor::clearCompactRailAreas() noexcept
{
	cachedLoaderTabCount_ = 0;
	for (int i = 0; i < 3; ++i)
	{
		cachedLoaderTabAreas_[i] = {};
		cachedLoaderTabStartIndices_[i] = 0;
	}
	cachedFooterRailArea_ = {};
	cachedFooterPanelArea_ = {};
	cachedFooterTitleArea_ = {};
}

void SATTRAudioProcessorEditor::setVisibleLoaderCount (int loaderCount, bool requestResize)
{
	const int safeCount = juce::jlimit (kCompactMinVisibleLoaders, kCompactMaxVisibleLoaders, loaderCount);
	const bool changed = visibleLoaderCount_ != safeCount;
	visibleLoaderCount_ = safeCount;
	firstVisibleLoaderIndex_ = juce::jlimit (0,
	                                         kCompactMaxVisibleLoaders - visibleLoaderCount_,
	                                         firstVisibleLoaderIndex_);

	if (requestResize)
	{
		const int targetW = getCompactTargetWidthForLoaderCount (safeCount);
		if (getWidth() != targetW || getHeight() != kCompactFixedHeightPx)
			setSize (targetW, kCompactFixedHeightPx);
	}
	else if (changed)
	{
		resized();
	}

	if (changed || requestResize)
		repaint();
}

void SATTRAudioProcessorEditor::setFirstVisibleLoaderIndex (int loaderIndex)
{
	const int maxFirst = kCompactMaxVisibleLoaders - visibleLoaderCount_;
	const int safeIndex = juce::jlimit (0, juce::jmax (0, maxFirst), loaderIndex);
	if (firstVisibleLoaderIndex_ == safeIndex)
		return;

	firstVisibleLoaderIndex_ = safeIndex;
	resized();
	repaint();
}

void SATTRAudioProcessorEditor::setFooterExpanded (bool shouldBeExpanded)
{
	if (footerExpanded_ == shouldBeExpanded)
		return;

	footerExpanded_ = shouldBeExpanded;
	resized();
	repaint();
}

void SATTRAudioProcessorEditor::hideLoaderSection (int loaderIndex)
{
	auto refs = getLoaderRefs (loaderIndex);

	juce::Component* components[] = {
		&refs.enableBtn,
		&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt,
		&refs.series, &refs.pan, &refs.fred, &refs.pos,
		&refs.inv, &refs.chaos, &refs.chaosFilter, &refs.chaosDisp,
		&refs.exp, &refs.expDisp,
		&refs.modeIn, &refs.modeOut, &refs.sumBus, &refs.filterPos,
		&refs.filterBar, &refs.mix,
		&refs.satType, &refs.raw,
		&refs.satDrive, &refs.satGirth, &refs.satMod, &refs.satBias, &refs.satSag,
		&refs.detail, &refs.instability, &refs.delay
	};

	for (auto* component : components)
	{
		component->setVisible (false);
		component->setBounds (0, 0, 0, 0);
	}

	if (loaderIndex >= 0 && loaderIndex < 3)
	{
		columnLeft_[loaderIndex] = 0;
		columnRight_[loaderIndex] = 0;
		for (int i = 0; i < kNumCachedParams; ++i)
			cachedValueAreas_[(size_t) (loaderIndex * kNumCachedParams + i)] = {};
	}

	if (loaderIndex == 0)
		cachedToggleBarAreaA_ = {};
	else if (loaderIndex == 1)
		cachedToggleBarAreaB_ = {};
	else if (loaderIndex == 2)
		cachedToggleBarAreaC_ = {};
}

void SATTRAudioProcessorEditor::hideFooterControls()
{
	juce::Component* components[] = {
		&globalMixSlider, &dualMixBar_, &globalOutputSlider, &limThresholdSlider,
		&matchCombo, &routeCombo, &mixModeCombo, &limModeCombo, &invPolCombo, &invStrCombo, &trimCombo
	};

	for (auto* component : components)
	{
		component->setVisible (false);
		component->setBounds (0, 0, 0, 0);
	}
}

void SATTRAudioProcessorEditor::layoutFooterControls (juce::Rectangle<int> area)
{
	trimCombo.setVisible (false);
	trimCombo.setBounds (0, 0, 0, 0);

	const int contentX = juce::jmax (0, cachedHeaderTitleX_);
	const int availableW = juce::jmax (1, area.getWidth() - contentX * 2);
	const int panelW = juce::jmin (kCompactFooterPanelWidthPx, availableW);
	const int panelX = area.getX() + (area.getWidth() - panelW) / 2;
	auto panel = area.withX (panelX).withWidth (panelW).reduced (0, 28);
	cachedFooterPanelArea_ = panel;
	const int barH = 30;
	const int comboH = 34;
	const int labelReserve = 24;
	const int barGap = 18;
	const int comboGap = 8;
	const int sliderW = juce::jmin (260, juce::jmax (180, (int) std::round ((double) panelW * 0.52)));
	auto fitControlHeight = [] (juce::Rectangle<int> r, int h)
	{
		return r.withSizeKeepingCentre (r.getWidth(), juce::jmin (h, r.getHeight()));
	};

	auto setMixBarBounds = [&] (juce::Rectangle<int> barArea)
	{
		const bool isSendMode = mixModeCombo.getSelectedId() == 2;
		if (isSendMode)
		{
			globalMixSlider.setVisible (false);
			globalMixSlider.setBounds (0, 0, 0, 0);
			dualMixBar_.setBounds (barArea);
			dualMixBar_.setVisible (true);
		}
		else
		{
			globalMixSlider.setBounds (barArea);
			globalMixSlider.setVisible (true);
			dualMixBar_.setBounds (0, 0, 0, 0);
			dualMixBar_.setVisible (false);
		}
	};

	auto placeBar = [&] (juce::Component& component)
	{
		auto row = panel.removeFromTop (barH);
		component.setBounds (fitControlHeight (row.removeFromLeft (sliderW), 28));
		component.setVisible (true);
		panel.removeFromTop (barGap);
	};

	cachedFooterTitleArea_ = panel.removeFromTop (30);
	panel.removeFromTop (22);

	auto mixRow = panel.removeFromTop (barH);
	setMixBarBounds (fitControlHeight (mixRow.removeFromLeft (sliderW), 28));
	panel.removeFromTop (barGap);

	placeBar (globalOutputSlider);
	placeBar (limThresholdSlider);

	panel.removeFromTop (12);

	juce::ComboBox* combos[] = { &matchCombo, &routeCombo, &mixModeCombo, &limModeCombo, &invPolCombo, &invStrCombo };
	for (auto* combo : combos)
	{
		panel.removeFromTop (labelReserve);
		combo->setBounds (panel.removeFromTop (comboH));
		combo->setVisible (true);
		panel.removeFromTop (comboGap);
	}
}

bool SATTRAudioProcessorEditor::refreshLegendTextCache()
{
	using namespace TR;

	auto formatFreq = [] (double hz) {
		if (hz < 1000.0)
			return juce::String (hz, 1) + " Hz";
		return juce::String (hz / 1000.0, 2) + " kHz";
	};

	auto formatDb = [] (float db) -> juce::String { return formatGainFaderDb (db); };

	auto formatPan = [] (float pan01) -> juce::String {
		const int pct = juce::roundToInt ((pan01 - 0.5f) * 200.0f);
		if (pct == 0)  return "C";
		if (pct < 0)   return "L" + juce::String (-pct);
		return "R" + juce::String (pct);
	};

	// Labels and format types: 0=freq, 1=dB, 2=ms, 3=percent, 4=pan, 5=tilt(dB), 6=bipolar%, 7=intX(series)
	struct ParamFmt { int type; const char* label; const char* shortLabel = nullptr; };
	// Dynamic legend labels depend on current algorithm per loader
	const char* driveLabels[3];
	const char* girthLabels[3];
	const char* modLabels[3];
	const char* biasLabels[3];
	const char* reactLabels[3];
	for (int l = 0; l < 3; ++l)
	{
		auto lr = getLoaderRefs (l);
		const int satModel = getSelectedSatTypeModelIndex (lr.satType);
		driveLabels[l] = "DRIVE";
		girthLabels[l] = "GIRTH";
		modLabels[l]   = "MOD";
		biasLabels[l]  = "BIAS";
		switch (satModel)
		{
            case 1:
                girthLabels[l] = "BODY";
                modLabels[l]   = "FORM";
                reactLabels[l] = "COMP";
                break; // Tape
            case 2:
                girthLabels[l] = "BODY";
                modLabels[l]   = "TYPE";
                reactLabels[l] = "SAG";
                break; // Tube
			case 3:
				driveLabels[l] = "GAIN";
				girthLabels[l] = "BODY";
				modLabels[l]   = "TYPE";
				biasLabels[l]  = "BIAS";
				reactLabels[l] = "COMP";
				break;
			case 4:
				girthLabels[l] = "COND";
				modLabels[l]   = "TOPO";
				biasLabels[l]  = "SYM";
				reactLabels[l] = "COMP";
				break;
			case 5:
				driveLabels[l] = "DRIVE";
				girthLabels[l] = "KNEE";
				modLabels[l]   = "VOICE";
				biasLabels[l]  = "SYM";
				reactLabels[l] = "PEAK";
				break;
			default:                         reactLabels[l] = "DYN";   break; // Clean/unknown
		}
	}

	ParamFmt fmts[kNumCachedParams] = {
		{0,"HP"}, {0,"LP"}, {1,"IN"}, {1,"OUT"}, {5,"TILT"}, {7,"SERIES"},
		{4,"PAN"}, {3,"ANGLE"}, {3,"DIST"}, {3,"MIX"},
		{3,"DRIVE"}, {3,"GIRTH"}, {3,"MOD"}, {6,"BIAS"}, {3,"DYN"}, {3,"DETAIL","DTL"}, {3,"INST"}, {8,"DELAY","DLY"}
	};

	for (int loader = 0; loader < 3; ++loader)
	{
		fmts[10].label = driveLabels[loader];
		fmts[11].label = girthLabels[loader];
		fmts[12].label = modLabels[loader];
		fmts[13].label = biasLabels[loader];
		fmts[14].label = reactLabels[loader];

		auto refs = getLoaderRefs (loader);
		const bool loaderEnabled = refs.enableBtn.getToggleState();
		const bool cleanModel = (getSelectedSatTypeModelIndex (refs.satType) == static_cast<int> (SatEngine::Model::Clean));
		juce::Slider* loaderSliders[kNumCachedParams] = {
			&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt, &refs.series,
			&refs.pan, &refs.fred, &refs.pos, &refs.mix,
			&refs.satDrive, &refs.satGirth, &refs.satMod, &refs.satBias, &refs.satSag, &refs.detail, &refs.instability, &refs.delay
		};

		auto setLabelOnly = [] (CachedParamText& text, const char* label)
		{
			text.full = label;
			text.short_ = label;
			text.intOnly = label;
		};

		for (int p = 0; p < kNumCachedParams; ++p)
		{
			auto& ct = cachedTexts[loader][p];
			const double val = loaderSliders[p]->getValue();
			const auto& fmt = fmts[p];

			switch (fmt.type)
			{
				case 0: // Frequency
					ct.full    = formatFreq (val) + " " + fmt.label;
					ct.short_  = formatFreq (val);
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 1: // dB
					ct.full    = formatDb ((float) val) + " " + fmt.label;
					ct.short_  = formatDb ((float) val);
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 2: // ms
					ct.full    = juce::String (juce::roundToInt (val)) + " ms " + fmt.label;
					ct.short_  = juce::String (juce::roundToInt (val)) + " ms";
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 3: // Percent (value is 0..1 range -> display as %)
				{
					const int pct = juce::roundToInt (val * 100.0);
					const auto shortLabel = fmt.shortLabel != nullptr ? fmt.shortLabel : "";
					ct.full    = juce::String (pct) + "% " + fmt.label;
					ct.short_  = fmt.shortLabel != nullptr ? juce::String (pct) + "% " + shortLabel
					                                       : juce::String (pct) + "%";
					ct.intOnly = juce::String (pct);
					break;
				}
				case 4: // Pan
					ct.full    = formatPan ((float) val) + " " + fmt.label;
					ct.short_  = formatPan ((float) val);
					ct.intOnly = formatPan ((float) val);
					break;
				case 5: // dB (Tilt)
					ct.full    = juce::String (val, 1) + " dB " + fmt.label;
					ct.short_  = juce::String (val, 1) + " dB";
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 6: // Bipolar percent (-1..1 -> -100..100%)
				{
					const int pct = juce::roundToInt (val * 100.0);
					ct.full    = juce::String (pct) + "% " + fmt.label;
					ct.short_  = juce::String (pct) + "%";
					ct.intOnly = juce::String (pct);
					break;
				}
				case 7: // Integer with "x" suffix (series count)
				{
					const int n = juce::roundToInt (val);
					ct.full    = juce::String (n) + "x " + fmt.label;
					ct.short_  = juce::String (n) + "x";
					ct.intOnly = juce::String (n);
					break;
				}
				case 8: // Delay ms (decimal)
				{
					const auto timeText = formatSatDelayMsForUi (val);
					ct.full    = timeText + " " + fmt.label;
					ct.short_  = timeText + " " + (fmt.shortLabel != nullptr ? fmt.shortLabel : fmt.label);
					ct.intOnly = formatSatDelayMsNumberForUi (val);
					break;
				}
			}

			const bool satParameterIrrelevant = cleanModel && (p == 5 || (p >= 10 && p <= 16));
			if (! loaderEnabled || satParameterIrrelevant)
				setLabelOnly (ct, fmt.label);
		}
	}

	// Global mix SEND-mode legend
	if (mixModeCombo.getSelectedId() == 2)
	{
		const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
		const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
		const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
		if (dB <= -100.0f)
			cachedMixIntOnly = "-INF";
		else if (std::abs (dB) < 0.05f)
			cachedMixIntOnly = "0.0 dB";
		else
			cachedMixIntOnly = juce::String (dB, 1) + " dB";
	}
	else
	{
		const int pct = juce::roundToInt (globalMixSlider.getValue() * 100.0);
		cachedMixIntOnly = juce::String (pct) + "%";
	}

	return false;
}

juce::String SATTRAudioProcessorEditor::getMixText() const
{
	if (mixModeCombo.getSelectedId() == 2)
	{
		const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
		const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
		const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
		if (dB <= -100.0f) return "-INF dB";
		if (std::abs (dB) < 0.05f) return "0.0 dB";
		return juce::String (dB, 1) + " dB";
	}
	const int pct = juce::roundToInt (globalMixSlider.getValue() * 100.0);
	return juce::String (pct) + "% MIX";
}

juce::String SATTRAudioProcessorEditor::getMixTextShort() const
{
	if (mixModeCombo.getSelectedId() == 2)
	{
		const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
		const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
		const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
		if (dB <= -100.0f) return "-INF";
		if (std::abs (dB) < 0.05f) return "0.0 dB";
		return juce::String (dB, 1) + " dB";
	}
	const int pct = juce::roundToInt (globalMixSlider.getValue() * 100.0);
	return juce::String (pct) + "% MX";
}

juce::Rectangle<int> SATTRAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds,
                                                                  int columnRight) const
{
	constexpr int valuePadPx    = 8;
	constexpr int valueHeightPx = 24;
	constexpr int rightMarginPx = 6;

	const int valueX = barBounds.getRight() + valuePadPx;
	const int maxW   = juce::jmax (0, columnRight - valueX - rightMarginPx);
	const int y      = barBounds.getCentreY() - (valueHeightPx / 2);

	return { valueX, y, maxW, valueHeightPx };
}

juce::Slider* SATTRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
	for (int i = 0; i < 3; ++i)
	{
		auto r = getLoaderRefs (i);
		const int colR = columnRight_[i];

		BarSlider* sliders[] = { &r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		                         &r.pan, &r.fred, &r.pos,
		                         &r.satDrive, &r.satGirth, &r.satMod, &r.satBias, &r.satSag,
		                         &r.detail, &r.instability, &r.delay };

		for (auto* s : sliders)
			if (s->isVisible() && s->isEnabled() && getValueAreaFor (s->getBounds(), colR).contains (p))
				return s;

		if (r.mix.isVisible() && r.mix.isEnabled() && getValueAreaFor (r.mix.getBounds(), colR).contains (p))
			return &r.mix;
	}

	if (globalMixSlider.isVisible()
		&& (footerExpanded_ ? makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), globalMixSlider.getBounds())
		                    : makeFooterValueArea (globalMixSlider.getBounds(), kFooterMixValueWidthPx)).contains (p))
		return &globalMixSlider;

	if (globalOutputSlider.isVisible()
		&& (footerExpanded_ ? makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), globalOutputSlider.getBounds())
		                    : makeFooterValueArea (globalOutputSlider.getBounds(), kFooterDbValueWidthPx)).contains (p))
		return &globalOutputSlider;

	if (limThresholdSlider.isVisible()
		&& (footerExpanded_ ? makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), limThresholdSlider.getBounds())
		                    : makeFooterValueArea (limThresholdSlider.getBounds(), kFooterDbValueWidthPx)).contains (p))
		return &limThresholdSlider;

	return nullptr;
}

juce::Rectangle<int> SATTRAudioProcessorEditor::getInfoIconArea() const
{
	constexpr int size = 32;
	constexpr int margin = 10;
	const int x = getWidth() - size - margin;
	const int y = margin;
	return { x, y, size, size };
}

void SATTRAudioProcessorEditor::updateInfoIconCache()
{
	const auto iconArea = getInfoIconArea();
	const auto iconF = iconArea.toFloat();
	const auto center = iconF.getCentre();
	const float toothTipR = (float) iconArea.getWidth() * 0.47f;
	const float toothRootR = toothTipR * 0.78f;
	const float holeR = toothTipR * 0.40f;
	constexpr int teeth = 8;

	cachedInfoGearPath.clear();
	for (int i = 0; i < teeth * 2; ++i)
	{
		const float a = -juce::MathConstants<float>::halfPi
		              + (juce::MathConstants<float>::pi * (float) i / (float) teeth);
		const float r = (i % 2 == 0) ? toothTipR : toothRootR;
		const float x = center.x + std::cos (a) * r;
		const float y = center.y + std::sin (a) * r;

		if (i == 0)
			cachedInfoGearPath.startNewSubPath (x, y);
		else
			cachedInfoGearPath.lineTo (x, y);
	}
	cachedInfoGearPath.closeSubPath();
	cachedInfoGearHole = { center.x - holeR, center.y - holeR, holeR * 2.0f, holeR * 2.0f };
}

// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
	if (promptOverlayActive == shouldBeActive)
		return;

	promptOverlayActive = shouldBeActive;

	promptOverlay.setBounds (getLocalBounds());
	promptOverlay.setVisible (shouldBeActive);
	if (shouldBeActive)
		promptOverlay.toFront (false);

	// promptOverlay intercepts mouse input while the modal prompt is open. Do not disable
	// the underlying controls here, otherwise the overlay dimming stacks with disabled alpha.
	if (! shouldBeActive)
	{
		updateLoaderEnabledState (0);
		updateLoaderEnabledState (1);
		updateLoaderEnabledState (2);
	}

	repaint();

	if (promptOverlayActive)
		promptOverlay.toFront (false);

	TR::anchorEditorOwnedPromptWindows (*this, lnf);
}

void SATTRAudioProcessorEditor::moved()
{
	if (promptOverlayActive)
		promptOverlay.toFront (false);

	TR::anchorEditorOwnedPromptWindows (*this, lnf);
}

void SATTRAudioProcessorEditor::parentHierarchyChanged()
{
	if (promptOverlayActive)
		promptOverlay.toFront (false);
}

// ----------------------------------------------------------------
//  Prompts
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	// -- Suffix determination via slider type --
	juce::String suffix;
	juce::String suffixShort;
	auto* bar = dynamic_cast<BarSlider*> (&s);
	const auto stype = bar ? bar->getType() : BarSlider::Type::Unknown;

	const bool isHp    = (stype == BarSlider::Type::HpFreq);
	const bool isLp    = (stype == BarSlider::Type::LpFreq);
	const bool isHpLp  = (isHp || isLp);
	const bool isIn    = (stype == BarSlider::Type::Input);
	const bool isOut   = (stype == BarSlider::Type::Output || stype == BarSlider::Type::GlobalOutput);
	const bool isLimThresh = (stype == BarSlider::Type::LimThreshold);
	const bool isTilt  = (stype == BarSlider::Type::Tilt);
	const bool isSeries = (stype == BarSlider::Type::Series);
	const bool isDetail = (stype == BarSlider::Type::Detail);
	const bool isInstability   = (stype == BarSlider::Type::Instability);
	const bool isDelay = (stype == BarSlider::Type::Delay);
	const bool isPan   = (stype == BarSlider::Type::Pan);
	const bool isFred  = (stype == BarSlider::Type::Fred);
	const bool isPos   = (stype == BarSlider::Type::Pos);
	const bool isMix   = (stype == BarSlider::Type::Mix || stype == BarSlider::Type::GlobalMix);
	const bool isSatDrive = (stype == BarSlider::Type::SatDrive);
	const bool isSatGirth = (stype == BarSlider::Type::SatGirth);
	const bool isSatMod   = (stype == BarSlider::Type::SatMod);
	const bool isSatBias  = (stype == BarSlider::Type::SatBias);
	const bool isSatSag   = (stype == BarSlider::Type::SatSag);
	const bool isSatPct   = (isSatDrive || isSatGirth || isSatMod || isSatSag);
	const bool isSatBiPct = isSatBias;

	if (isSeries)
		return;

	auto getSatPromptLabel = [this, &s, stype]() -> juce::String
	{
		int loaderIndex = -1;
		for (int i = 0; i < 3 && loaderIndex < 0; ++i)
		{
			auto refs = getLoaderRefs (i);
			if ((stype == BarSlider::Type::SatDrive && &refs.satDrive == &s)
			 || (stype == BarSlider::Type::SatGirth && &refs.satGirth == &s)
			 || (stype == BarSlider::Type::SatMod   && &refs.satMod   == &s)
			 || (stype == BarSlider::Type::SatBias  && &refs.satBias  == &s)
			 || (stype == BarSlider::Type::SatSag   && &refs.satSag   == &s))
				loaderIndex = i;
		}

		if (loaderIndex < 0)
		{
			switch (stype)
			{
				case BarSlider::Type::SatDrive: return "DRIVE";
				case BarSlider::Type::SatGirth: return "GIRTH";
				case BarSlider::Type::SatMod:   return "MOD";
				case BarSlider::Type::SatBias:  return "BIAS";
				case BarSlider::Type::SatSag:   return "DYN";
				default:                        return {};
			}
		}

		auto lr = getLoaderRefs (loaderIndex);
		const int satModel = getSelectedSatTypeModelIndex (lr.satType);
		juce::String driveLabel = "DRIVE";
		juce::String girthLabel = "GIRTH";
		juce::String modLabel   = "MOD";
		juce::String biasLabel  = "BIAS";
		juce::String reactLabel = "DYN";

		switch (satModel)
		{
			case 1: girthLabel = "BODY"; modLabel = "FORM"; reactLabel = "COMP"; break;
			case 2: girthLabel = "BODY"; modLabel = "TYPE"; reactLabel = "SAG";  break;
			case 3: driveLabel = "GAIN"; girthLabel = "BODY"; modLabel = "TYPE"; reactLabel = "COMP"; break;
			case 4: girthLabel = "COND"; modLabel = "TOPO"; biasLabel = "SYM"; reactLabel = "COMP"; break;
			case 5: driveLabel = "DRIVE"; girthLabel = "KNEE"; modLabel = "VOICE"; biasLabel = "SYM"; reactLabel = "PEAK"; break;
			default: break;
		}

		switch (stype)
		{
			case BarSlider::Type::SatDrive: return driveLabel;
			case BarSlider::Type::SatGirth: return girthLabel;
			case BarSlider::Type::SatMod:   return modLabel;
			case BarSlider::Type::SatBias:  return biasLabel;
			case BarSlider::Type::SatSag:   return reactLabel;
			default:                        return {};
		}
	};

	const juce::String satLabel = getSatPromptLabel();

	if (isHp)               { suffix = " Hz HP";       suffixShort = " Hz HP"; }
	else if (isLp)          { suffix = " Hz LP";       suffixShort = " Hz LP"; }
	else if (isIn)          { suffix = " dB INPUT";    suffixShort = " dB IN"; }
	else if (isOut)         { suffix = " dB OUTPUT";   suffixShort = " dB OUT"; }
	else if (isLimThresh)   { suffix = " dB LIM";      suffixShort = " dB LIM"; }
	else if (isTilt)        { suffix = " dB TILT";     suffixShort = " dB TILT"; }
	else if (isDetail)      { suffix = " % DETAIL";    suffixShort = " % DTL"; }
	else if (isInstability) { suffix = " % INST";      suffixShort = " % INST"; }
	else if (isDelay)       { suffix = " ms";          suffixShort = " ms"; }
	else if (isPan)         { suffix = " % PAN";       suffixShort = " % PAN"; }
	else if (isFred)        { suffix = " % ANGLE";     suffixShort = " % ANGLE"; }
	else if (isPos)         { suffix = " % DIST";      suffixShort = " % DIST"; }
	else if (isMix)         { suffix = " % MIX";       suffixShort = " % MIX"; }
	else if (isSatPct || isSatBiPct)
	{
		suffix = " % " + satLabel;
		suffixShort = suffix;
	}

	const juce::String suffixText      = suffix.trimStart();
	const juce::String suffixTextShort = suffixShort.trimStart();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// -- Initial display value --
	juce::String currentDisplay;
	if (isHpLp)
		currentDisplay = formatFilterPromptFrequency ((float) s.getValue());
	else if (isIn || isOut || isLimThresh)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isTilt)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isDetail || isInstability)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else if (isDelay)
		currentDisplay = formatTimeMsForPromptValue (juce::jlimit (0.0, (double) SATTRAudioProcessor::kDelayMax, s.getValue()));
	else if (isPan)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
	else if (isSatBias)
		currentDisplay = juce::String (juce::jlimit (-100.0, 100.0, s.getValue() * 100.0), 2);
	else if (isSatPct)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else if (isFred || isPos || isMix)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else
		currentDisplay = s.getTextFromValue (s.getValue());

	aw->addTextEditor ("val", currentDisplay, juce::String());

	juce::Label* suffixLabel = nullptr;
	juce::Rectangle<int> editorBaseBounds;
	std::function<void()> layoutValueAndSuffix;

	if (auto* te = aw->getTextEditor ("val"))
	{
		const auto& f = kBoldFont40();
		te->setFont (f);
		te->applyFontToAllText (f);

		auto r = te->getBounds();
		r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
		r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));
		editorBaseBounds = r;

		suffixLabel = new juce::Label ("suffix", suffixText);
		suffixLabel->setComponentID (kPromptSuffixLabelId);
		suffixLabel->setJustificationType (juce::Justification::centredLeft);
		applyLabelTextColour (*suffixLabel, scheme.text);
		suffixLabel->setBorderSize (juce::BorderSize<int> (0));
		suffixLabel->setFont (f);
		aw->addAndMakeVisible (suffixLabel);

		// Worst-case widths for layout stability
		juce::String worstCaseText;
		if (isHpLp)              worstCaseText = "20000.000";
		else if (isIn)           worstCaseText = "-144.0";
		else if (isOut)          worstCaseText = "-144.0";
		else if (isLimThresh)    worstCaseText = "-36.0";
		else if (isTilt)         worstCaseText = "-6.0";
		else if (isDetail || isInstability)  worstCaseText = "100.00";
		else if (isDelay)        worstCaseText = "5.000";
		else if (isPan)          worstCaseText = "100";
		else if (isSatBias)      worstCaseText = "-100.00";
		else if (isSatPct)       worstCaseText = "100.00";
		else if (isFred||isPos)  worstCaseText = "100.00";
		else if (isMix)          worstCaseText = "100.00";
		else                     worstCaseText = "999.99";

		const int maxInputTextW = juce::jmax (1, stringWidth (f, worstCaseText));

		layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds,
		                        suffixText, suffixTextShort, maxInputTextW]()
		{
			const int contentPad = kPromptInlineContentPadPx;
			const int contentLeft = contentPad;
			const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
			const int availableW = contentRight - contentLeft;
			const int contentCenter = (contentLeft + contentRight) / 2;

			const int fullLabelW = stringWidth (suffixLabel->getFont(), suffixText) + 2;
			const bool stickPercentFull = suffixText.containsChar ('%');
			const int spaceWFull = stickPercentFull ? 0 : juce::jmax (2, stringWidth (suffixLabel->getFont(), " "));
			const int worstCaseFullW = maxInputTextW + spaceWFull + fullLabelW;

			const bool useShort = (worstCaseFullW > availableW) && suffixTextShort != suffixText;
			const juce::String& activeSuffix = useShort ? suffixTextShort : suffixText;
			suffixLabel->setText (activeSuffix, juce::dontSendNotification);

			const auto txt = te->getText();
			const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));
			int labelW = stringWidth (suffixLabel->getFont(), activeSuffix) + 2;

			const bool stickPercent = activeSuffix.containsChar ('%');
			const int spaceW = stickPercent ? 0 : juce::jmax (2, stringWidth (te->getFont(), " "));
			const int minGapPx = juce::jmax (1, spaceW);

			constexpr int kEditorTextPadPx = 12;
			constexpr int kMinEditorWidthPx = 24;
			const int editorW = juce::jlimit (kMinEditorWidthPx,
			                                  editorBaseBounds.getWidth(),
			                                  textW + kEditorTextPadPx * 2);
			auto er = te->getBounds();
			er.setWidth (editorW);

			const int combinedW = textW + minGapPx + labelW;
			int blockLeft = contentCenter - combinedW / 2;
			blockLeft = juce::jlimit (contentLeft,
			                          juce::jmax (contentLeft, contentRight - combinedW),
			                          blockLeft);

			int teX = blockLeft - (editorW - textW) / 2;
			teX = juce::jlimit (contentLeft,
			                    juce::jmax (contentLeft, contentRight - editorW), teX);
			er.setX (teX);
			te->setBounds (er);

			const int textLeftActual = er.getX() + (er.getWidth() - textW) / 2;
			int labelX = textLeftActual + textW + minGapPx;
			labelX = juce::jlimit (contentLeft,
			                       juce::jmax (contentLeft, contentRight - labelW), labelX);
			suffixLabel->setBounds (labelX, er.getY(), labelW, juce::jmax (1, er.getHeight()));
		};

		te->setBounds (editorBaseBounds);
		int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
		suffixLabel->setBounds (r.getRight() + 2, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

		if (layoutValueAndSuffix)
			layoutValueAndSuffix();

		// -- Per-slider input constraints --
		double minVal = 0.0, maxVal = 1.0;
		int maxLen = 0, maxDecs = 4;

		if (isHpLp)
		{
			minVal = 20.0;   maxVal = 20000.0;
			maxDecs = 2;     maxLen = 8;     // "20000.00"
		}
		else if (isIn)
		{
			minVal = SATTRAudioProcessor::kGainFloorDb;
			maxVal = SATTRAudioProcessor::kGainMaxDb;
			maxDecs = 1;     maxLen = 6;     // "-144.0"
		}
		else if (isOut)
		{
			minVal = SATTRAudioProcessor::kGainFloorDb;
			maxVal = SATTRAudioProcessor::kGainMaxDb;
			maxDecs = 1;     maxLen = 6;     // "-144.0"
		}
		else if (isLimThresh)
		{
			minVal = SATTRAudioProcessor::kLimThresholdMin;
			maxVal = SATTRAudioProcessor::kLimThresholdMax;
			maxDecs = 1;     maxLen = 5;     // "-36.0"
		}
		else if (isTilt)
		{
			minVal = -6.0;   maxVal = 6.0;
			maxDecs = 1;     maxLen = 4;     // "-6.0"
		}
		else if (isDetail || isInstability)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 2;     maxLen = 6;     // "100.00"
		}
		else if (isDelay)
		{
			minVal = SATTRAudioProcessor::kDelayMin;
			maxVal = SATTRAudioProcessor::kDelayMax;
			maxDecs = 3;     maxLen = 5;     // "5.000"
		}
		else if (isPan)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 0;     maxLen = 3;     // "100"
		}
		else if (isFred || isPos || isMix)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 2;     maxLen = 6;     // "100.00"
		}
		else if (isSatBias)
		{
			minVal = -100.0; maxVal = 100.0;
			maxDecs = 2;     maxLen = 7;     // "-100.00"
		}
		else if (isSatPct)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 2;     maxLen = 6;     // "100.00"
		}

		te->setInputFilter (new NumericInputFilter (minVal, maxVal, maxLen, maxDecs), true);

		const int localMaxDecs = maxDecs;
		te->onTextChange = [te, layoutValueAndSuffix, localMaxDecs]() mutable
		{
			auto txt = te->getText();
			int dot = txt.indexOfChar ('.');
			if (dot >= 0)
			{
				int decimals = txt.length() - dot - 1;
				if (decimals > localMaxDecs)
					te->setText (txt.substring (0, dot + 1 + localMaxDecs), juce::dontSendNotification);
			}
			if (layoutValueAndSuffix)
				layoutValueAndSuffix();
		};
	}

	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);
	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);

	const juce::Font& kPromptFont = kBoldFont40();

	preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);

	if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
	{
		if (auto* te = aw->getTextEditor ("val"))
			suffixLabel->setFont (te->getFont());
		if (layoutValueAndSuffix)
			layoutValueAndSuffix();
	}

	styleAlertButtons (*aw, lnf);

	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);
	juce::Slider* sliderPtr = &s;

	setPromptOverlayActive (true);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutValueAndSuffix, scheme, &kPromptFont] (juce::AlertWindow& a)
		{
			if (layoutValueAndSuffix)
				layoutValueAndSuffix();
			layoutAlertWindowButtons (a);
			preparePromptTextEditor (a, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	// Final styling pass
	{
		preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
		if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
		{
			if (auto* te = aw->getTextEditor ("val"))
				suffixLbl->setFont (te->getFont());
		}
		if (layoutValueAndSuffix)
			layoutValueAndSuffix();

		juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
		juce::MessageManager::callAsync ([safeAw]()
		{
			if (safeAw == nullptr) return;
			bringPromptWindowToFront (*safeAw);
			safeAw->repaint();
		});
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis, sliderPtr, aw] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);

			if (safeThis == nullptr || sliderPtr == nullptr)
				return;

			if (result != 1)
				return;

			const auto txt = aw->getTextEditorContents ("val").trim();
			auto normalised = txt.replaceCharacter (',', '.');

			juce::String t = normalised.trimStart();
			while (t.startsWithChar ('+'))
				t = t.substring (1).trimStart();
			const juce::String numericToken = t.initialSectionContainingOnly ("0123456789.,-");

			// Percent-based sliders: user typed 0x100/200, slider stores 0x1/2
			auto* barPtr = dynamic_cast<BarSlider*> (sliderPtr);
			const auto st = barPtr ? barPtr->getType() : BarSlider::Type::Unknown;
			const bool isGainFader = (st == BarSlider::Type::Input ||
			                          st == BarSlider::Type::Output ||
			                          st == BarSlider::Type::GlobalOutput);
			double v = (isGainFader && t.containsIgnoreCase ("inf"))
				? (double) SATTRAudioProcessor::kGainFloorDb
				: numericToken.getDoubleValue();

			const bool needsPercentConvert = (st == BarSlider::Type::Pan ||
			                                  st == BarSlider::Type::Fred  || st == BarSlider::Type::Pos  ||
			                                  st == BarSlider::Type::Mix  ||
			                                  st == BarSlider::Type::SatDrive || st == BarSlider::Type::SatGirth ||
			                                  st == BarSlider::Type::SatMod   || st == BarSlider::Type::SatBias  ||
			                                  st == BarSlider::Type::SatSag   ||
			                                  st == BarSlider::Type::Detail ||
			                                  st == BarSlider::Type::Instability ||
			                                  st == BarSlider::Type::GlobalMix);

			if (needsPercentConvert)
				v *= 0.01;

			const auto range = sliderPtr->getRange();
			double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);

			sliderPtr->setValue (clamped, juce::sendNotificationSync);
		}));
}

// ----------------------------------------------------------------
//  MIX SEND prompt (DRY + WET levels)
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::openMixSendPrompt()
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	auto& proc = audioProcessor;
	const float curDry = proc.getValueTreeState().getRawParameterValue (SATTRAudioProcessor::kParamDryLevel)->load();
	const float curWet = proc.getValueTreeState().getRawParameterValue (SATTRAudioProcessor::kParamWetLevel)->load();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// -- Inline PromptBar (horizontal fill bar with draggable value) --
	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float  value01   = 1.0f;
		float  default01 = 1.0f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial, float def)
			: colours (s), value01 (initial), default01 (def) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);
			const float pad = 7.0f;
			auto inner = r.reduced (pad);
			g.setColour (colours.bg);
			g.fillRect (inner);
			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value01);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (default01); }

		void setValue (float v)
		{
			value01 = juce::jlimit (0.0f, 1.0f, v);
			repaint();
			if (onValueChanged)
				onValueChanged (value01);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerW = (float) getWidth() - pad * 2.0f;
			setValue (innerW > 0.0f ? ((float) e.x - pad) / innerW : 0.0f);
		}
	};

	// dB helpers
	auto linearToDb = [] (float g) -> float
	{
		return (g <= 0.0001f) ? -100.0f : 20.0f * std::log10 (g);
	};
	auto dbToLinear = [] (float dB) -> float
	{
		return (dB <= -100.0f) ? 0.0f : std::pow (10.0f, dB / 20.0f);
	};
	auto dbString = [&linearToDb] (float g) -> juce::String
	{
		const float dB = linearToDb (g);
		if (dB <= -100.0f) return "-INF";
		if (std::abs (dB) < 0.05f) return "0";
		return juce::String (dB, 1);
	};

	// DRY section
	aw->addTextEditor ("dryLevel", dbString (curDry), juce::String());
	auto* dryBar = new PromptBar (scheme, curDry, SATTRAudioProcessor::kDryLevelDefault);
	aw->addAndMakeVisible (dryBar);

	// WET section
	aw->addTextEditor ("wetLevel", dbString (curWet), juce::String());
	auto* wetBar = new PromptBar (scheme, curWet, SATTRAudioProcessor::kWetLevelDefault);
	aw->addAndMakeVisible (wetBar);

	// Shared sync flag
	auto syncing  = std::make_shared<bool> (false);
	auto layoutFn = std::make_shared<std::function<void()>> ([] {});

	// -- Real-time parameter setter --
	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);

	auto pushParams = [safeThis, aw, dbToLinear] ()
	{
		if (safeThis == nullptr) return;
		auto& p = safeThis->audioProcessor;
		auto setP = [&p] (const char* id, float plain)
		{
			if (auto* param = p.getValueTreeState().getParameter (id))
				param->setValueNotifyingHost (param->convertTo0to1 (plain));
		};

		auto* dryTe = aw->getTextEditor ("dryLevel");
		auto* wetTe = aw->getTextEditor ("wetLevel");
		const float dryLin = dryTe ? juce::jlimit (0.0f, 1.0f, dbToLinear (dryTe->getText().getFloatValue())) : 1.0f;
		const float wetLin = wetTe ? juce::jlimit (0.0f, 1.0f, dbToLinear (wetTe->getText().getFloatValue())) : 1.0f;
		setP (SATTRAudioProcessor::kParamDryLevel, dryLin);
		setP (SATTRAudioProcessor::kParamWetLevel, wetLin);

		safeThis->dualMixBar_.updateFromProcessor();
	};

	// Wire bar -> text sync
	auto barToText = [aw, syncing, pushParams, dbString] (const char* editorId, float v01)
	{
		if (*syncing) return;
		*syncing = true;
		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setText (dbString (v01), juce::sendNotification);
			te->selectAll();
		}
		*syncing = false;
		pushParams();
	};

	dryBar->onValueChanged = [barToText] (float v) { barToText ("dryLevel", v); };
	wetBar->onValueChanged = [barToText] (float v) { barToText ("wetLevel", v); };

	// Wire text -> bar sync
	auto textToBar = [syncing, pushParams, dbToLinear] (juce::TextEditor* te, PromptBar* bar)
	{
		if (*syncing || te == nullptr || bar == nullptr) return;
		*syncing = true;
		const float dB  = te->getText().getFloatValue();
		const float lin = juce::jlimit (0.0f, 1.0f, dbToLinear (dB));
		bar->value01 = lin;
		bar->repaint();
		*syncing = false;
		pushParams();
	};

	auto* dryTe = aw->getTextEditor ("dryLevel");
	auto* wetTe = aw->getTextEditor ("wetLevel");

	if (dryTe != nullptr)
		dryTe->onTextChange = [syncing, textToBar, dryTe, dryBar, layoutFn] () { textToBar (dryTe, dryBar); if (*layoutFn) (*layoutFn)(); };
	if (wetTe != nullptr)
		wetTe->onTextChange = [syncing, textToBar, wetTe, wetBar, layoutFn] () { textToBar (wetTe, wetBar); if (*layoutFn) (*layoutFn)(); };

	// Buttons
	aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);

	const int margin = kPromptInnerMargin;
	const juce::Font promptFont (juce::FontOptions (34.0f).withStyle ("Bold"));

	// -- Create persistent labels --
	auto* dryNameLabel = new juce::Label ("", "DRY");
	dryNameLabel->setJustificationType (juce::Justification::centredLeft);
	dryNameLabel->setColour (juce::Label::textColourId, scheme.text);
	dryNameLabel->setBorderSize (juce::BorderSize<int> (0));
	dryNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (dryNameLabel);

	auto* wetNameLabel = new juce::Label ("", "WET");
	wetNameLabel->setJustificationType (juce::Justification::centredLeft);
	wetNameLabel->setColour (juce::Label::textColourId, scheme.text);
	wetNameLabel->setBorderSize (juce::BorderSize<int> (0));
	wetNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (wetNameLabel);

	auto* dryDbLabel = new juce::Label ("", "dB");
	dryDbLabel->setJustificationType (juce::Justification::centredLeft);
	dryDbLabel->setColour (juce::Label::textColourId, scheme.text);
	dryDbLabel->setBorderSize (juce::BorderSize<int> (0));
	dryDbLabel->setFont (promptFont);
	aw->addAndMakeVisible (dryDbLabel);

	auto* wetDbLabel = new juce::Label ("", "dB");
	wetDbLabel->setJustificationType (juce::Justification::centredLeft);
	wetDbLabel->setColour (juce::Label::textColourId, scheme.text);
	wetDbLabel->setBorderSize (juce::BorderSize<int> (0));
	wetDbLabel->setFont (promptFont);
	aw->addAndMakeVisible (wetDbLabel);

	// -- Prepare TextEditors --
	preparePromptTextEditor (*aw, "dryLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "wetLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);

	// -- Re-callable layout --
	auto layoutRows = [aw, dryNameLabel, wetNameLabel, dryDbLabel, wetDbLabel,
	                    dryBar, wetBar, promptFont, margin] ()
	{
		auto* dryTe = aw->getTextEditor ("dryLevel");
		auto* wetTe = aw->getTextEditor ("wetLevel");
		if (dryTe == nullptr || wetTe == nullptr) return;

		const int buttonsTop = getAlertButtonsTop (*aw);
		const int rowH       = dryTe->getHeight();
		const int barH       = juce::jmax (10, rowH / 2);
		const int barGap     = juce::jmax (2, rowH / 6);
		const int gap        = juce::jmax (4, rowH / 3);
		const int rowTotal   = rowH + barGap + barH;
		const int totalH     = rowTotal * 2 + gap;
		const int startY     = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

		const int barX = margin;
		const int barR = aw->getWidth() - margin;

		const int nameW = stringWidth (promptFont, "WET") + 6;
		const int hzGap = 2;
		const int dbW   = stringWidth (promptFont, "dB") + 2;

		auto placeRow = [&] (juce::Label* nameLabel, juce::TextEditor* te,
		                     juce::Label* dbLabel, PromptBar* bar, int y)
		{
			nameLabel->setFont (promptFont);
			dbLabel->setFont (promptFont);

			nameLabel->setBounds (barX, y, nameW, rowH);

			const int midL = barX + nameW;
			const int midR = barR;
			const int midW = midR - midL;

			const auto txt   = te->getText();
			const int textW  = juce::jmax (1, stringWidth (promptFont, txt));
			constexpr int kEditorPad = 6;
			const int editorW = textW + kEditorPad * 2;
			const int groupW  = editorW + hzGap + dbW;

			const int groupX = midL + juce::jmax (0, (midW - groupW) / 2);

			te->setBounds (groupX, y, editorW, rowH);
			dbLabel->setBounds (groupX + editorW + hzGap, y, dbW, rowH);

			const int barW = juce::jmax (60, barR - barX);
			bar->setBounds (barX, y + rowH + barGap, barW, barH);
		};

		placeRow (dryNameLabel, dryTe, dryDbLabel, dryBar, startY);
		placeRow (wetNameLabel, wetTe, wetDbLabel, wetBar, startY + rowTotal + gap);
	};

	layoutRows();
	*layoutFn = layoutRows;

	preparePromptTextEditor (*aw, "dryLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "wetLevel", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	layoutRows();

	styleAlertButtons (*aw, lnf);

	// Store originals for CANCEL restore
	const float origDry = curDry;
	const float origWet = curWet;

	fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
	{
		layoutAlertWindowButtons (a);
		layoutRows();
	});

	embedAlertWindowInOverlay (safeThis.getComponent(), aw);

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, origDry, origWet] (int result)
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			if (result != 1)
			{
				// CANCEL - restore original values
				auto& p = safeThis->audioProcessor;
				auto setP = [&p] (const char* id, float plain)
				{
					if (auto* param = p.getValueTreeState().getParameter (id))
						param->setValueNotifyingHost (param->convertTo0to1 (plain));
				};
				setP (SATTRAudioProcessor::kParamDryLevel, origDry);
				setP (SATTRAudioProcessor::kParamWetLevel, origWet);
				safeThis->dualMixBar_.updateFromProcessor();
			}

			safeThis->setPromptOverlayActive (false);
		}),
		false);
}

// ----------------------------------------------------------------
//  FILTER prompt (HP + LP frequencies, on/off, slope)
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::openFilterPrompt (int loaderIndex)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	auto& proc = audioProcessor;
	auto& vts = proc.getValueTreeState();

	auto pickId = [&] (const char* a, const char* b, const char* c) -> const char* {
		return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
	};
	const char* hpFreqId  = pickId (SATTRAudioProcessor::kParamHpFreqA,  SATTRAudioProcessor::kParamHpFreqB,  SATTRAudioProcessor::kParamHpFreqC);
	const char* lpFreqId  = pickId (SATTRAudioProcessor::kParamLpFreqA,  SATTRAudioProcessor::kParamLpFreqB,  SATTRAudioProcessor::kParamLpFreqC);
	const char* hpOnId    = pickId (SATTRAudioProcessor::kParamHpOnA,    SATTRAudioProcessor::kParamHpOnB,    SATTRAudioProcessor::kParamHpOnC);
	const char* lpOnId    = pickId (SATTRAudioProcessor::kParamLpOnA,    SATTRAudioProcessor::kParamLpOnB,    SATTRAudioProcessor::kParamLpOnC);
	const char* hpSlopeId = pickId (SATTRAudioProcessor::kParamHpSlopeA, SATTRAudioProcessor::kParamHpSlopeB, SATTRAudioProcessor::kParamHpSlopeC);
	const char* lpSlopeId = pickId (SATTRAudioProcessor::kParamLpSlopeA, SATTRAudioProcessor::kParamLpSlopeB, SATTRAudioProcessor::kParamLpSlopeC);

	const float hpFreq  = vts.getRawParameterValue (hpFreqId)->load();
	const float lpFreq  = vts.getRawParameterValue (lpFreqId)->load();
	const bool  hpOn    = vts.getRawParameterValue (hpOnId)->load() > 0.5f;
	const bool  lpOn    = vts.getRawParameterValue (lpOnId)->load() > 0.5f;
	const int   hpSlope = juce::jlimit (0, 2, (int) vts.getRawParameterValue (hpSlopeId)->load());
	const int   lpSlope = juce::jlimit (0, 2, (int) vts.getRawParameterValue (lpSlopeId)->load());

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// -- Inline PromptBar for freq dragging --
	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float value01    = 0.5f;
		float default01  = 0.5f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial01, float def01)
			: colours (s), value01 (initial01), default01 (def01) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);
			const float pad = 7.0f;
			auto inner = r.reduced (pad);
			g.setColour (colours.bg);
			g.fillRect (inner);
			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value01);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (default01); }

		void setValue (float v)
		{
			value01 = juce::jlimit (0.0f, 1.0f, v);
			repaint();
			if (onValueChanged)
				onValueChanged (value01);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerW = (float) getWidth() - pad * 2.0f;
			setValue (innerW > 0.0f ? ((float) e.x - pad) / innerW : 0.0f);
		}
	};

	// Freq normalisation helpers (log scale 20..20000)
	auto freqToNorm = [] (float freq) -> float
	{
		constexpr float minF = 20.0f, maxF = 20000.0f;
		return std::log2 (juce::jlimit (minF, maxF, freq) / minF) / std::log2 (maxF / minF);
	};
	auto normToFreq = [] (float n) -> float
	{
		constexpr float minF = 20.0f, maxF = 20000.0f;
		return minF * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, n) * std::log2 (maxF / minF));
	};

	// HP section
	aw->addTextEditor ("hpFreq", juce::String (juce::roundToInt (hpFreq)), juce::String());
	auto* hpBar = new PromptBar (scheme, freqToNorm (hpFreq), freqToNorm (SATTRAudioProcessor::kFilterHpFreqDefault));
	aw->addAndMakeVisible (hpBar);

	// LP section
	aw->addTextEditor ("lpFreq", juce::String (juce::roundToInt (lpFreq)), juce::String());
	auto* lpBar = new PromptBar (scheme, freqToNorm (lpFreq), freqToNorm (SATTRAudioProcessor::kFilterLpFreqDefault));
	aw->addAndMakeVisible (lpBar);

	// HP on/off toggle
	auto* hpToggle = new juce::ToggleButton ("");
	hpToggle->setToggleState (hpOn, juce::dontSendNotification);
	hpToggle->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (hpToggle);

	// LP on/off toggle
	auto* lpToggle = new juce::ToggleButton ("");
	lpToggle->setToggleState (lpOn, juce::dontSendNotification);
	lpToggle->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (lpToggle);

	// -- Clickable slope labels (cycle 6->12->24->6 on click) --
	auto slopeToText = [] (int s) -> juce::String
	{
		if (s == 0) return "6dB";
		if (s == 1) return "12dB";
		return "24dB";
	};

	const juce::Font slopeFont (juce::FontOptions (24.0f).withStyle ("Bold"));

	auto* hpSlopeLabel = new juce::Label ("", slopeToText (hpSlope));
	hpSlopeLabel->setJustificationType (juce::Justification::centredRight);
	hpSlopeLabel->setColour (juce::Label::textColourId, scheme.text);
	hpSlopeLabel->setBorderSize (juce::BorderSize<int> (0));
	hpSlopeLabel->setFont (slopeFont);
	aw->addAndMakeVisible (hpSlopeLabel);

	auto* lpSlopeLabel = new juce::Label ("", slopeToText (lpSlope));
	lpSlopeLabel->setJustificationType (juce::Justification::centredRight);
	lpSlopeLabel->setColour (juce::Label::textColourId, scheme.text);
	lpSlopeLabel->setBorderSize (juce::BorderSize<int> (0));
	lpSlopeLabel->setFont (slopeFont);
	aw->addAndMakeVisible (lpSlopeLabel);

	// Shared state
	auto hpSlopeVal = std::make_shared<int> (hpSlope);
	auto lpSlopeVal = std::make_shared<int> (lpSlope);
	auto syncing    = std::make_shared<bool> (false);
	auto layoutFn   = std::make_shared<std::function<void()>> ([] {});

	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);

	// Capture param IDs for lambda usage
	const juce::String hpFreqIdStr (hpFreqId);
	const juce::String lpFreqIdStr (lpFreqId);
	const juce::String hpOnIdStr (hpOnId);
	const juce::String lpOnIdStr (lpOnId);
	const juce::String hpSlopeIdStr (hpSlopeId);
	const juce::String lpSlopeIdStr (lpSlopeId);

	auto pushParams = [safeThis, hpToggle, lpToggle, hpSlopeVal, lpSlopeVal, normToFreq, aw,
	                    hpFreqIdStr, lpFreqIdStr, hpOnIdStr, lpOnIdStr,
	                    hpSlopeIdStr, lpSlopeIdStr, loaderIndex] ()
	{
		if (safeThis == nullptr) return;
		auto& p = safeThis->audioProcessor;
		auto& vts = p.getValueTreeState();
		auto setP = [&vts] (const juce::String& id, float plain)
		{
			if (auto* param = vts.getParameter (id))
				param->setValueNotifyingHost (param->convertTo0to1 (plain));
		};

		auto* hpTe = aw->getTextEditor ("hpFreq");
		auto* lpTe = aw->getTextEditor ("lpFreq");
		float hpF = hpTe ? juce::jlimit (20.0f, 20000.0f, (float) hpTe->getText().getFloatValue()) : 20.0f;
		float lpF = lpTe ? juce::jlimit (20.0f, 20000.0f, (float) lpTe->getText().getFloatValue()) : 20000.0f;
		if (hpF > lpF) { const float mid = (hpF + lpF) * 0.5f; hpF = mid; lpF = mid; }
		if (hpTe) setP (hpFreqIdStr, hpF);
		if (lpTe) setP (lpFreqIdStr, lpF);
		setP (hpSlopeIdStr, (float) *hpSlopeVal);
		setP (lpSlopeIdStr, (float) *lpSlopeVal);

		if (auto* hpOnParam = vts.getParameter (hpOnIdStr))
			hpOnParam->setValueNotifyingHost (hpToggle->getToggleState() ? 1.0f : 0.0f);
		if (auto* lpOnParam = vts.getParameter (lpOnIdStr))
			lpOnParam->setValueNotifyingHost (lpToggle->getToggleState() ? 1.0f : 0.0f);

		auto& fb = loaderIndex == 0 ? safeThis->filterBarA_ : (loaderIndex == 1 ? safeThis->filterBarB_ : safeThis->filterBarC_);
		fb.updateFromProcessor();
	};

	// Slope label click -> cycle value and push
	struct SlopeCycler : public juce::MouseListener
	{
		std::shared_ptr<int> val;
		juce::Label* label;
		std::function<juce::String (int)> toText;
		std::function<void()> push;
		std::shared_ptr<std::function<void()>> layout;
		void mouseDown (const juce::MouseEvent&) override
		{
			*val = (*val + 1) % 3;
			label->setText (toText (*val), juce::dontSendNotification);
			push();
			if (layout && *layout) (*layout)();
		}
	};

	hpSlopeLabel->setInterceptsMouseClicks (true, false);
	auto* hpCycler = new SlopeCycler();
	hpCycler->val = hpSlopeVal;
	hpCycler->label = hpSlopeLabel;
	hpCycler->toText = slopeToText;
	hpCycler->push = pushParams;
	hpCycler->layout = layoutFn;
	hpSlopeLabel->addMouseListener (hpCycler, false);

	lpSlopeLabel->setInterceptsMouseClicks (true, false);
	auto* lpCycler = new SlopeCycler();
	lpCycler->val = lpSlopeVal;
	lpCycler->label = lpSlopeLabel;
	lpCycler->toText = slopeToText;
	lpCycler->push = pushParams;
	lpCycler->layout = layoutFn;
	lpSlopeLabel->addMouseListener (lpCycler, false);

	auto hpCyclerGuard = std::shared_ptr<SlopeCycler> (hpCycler);
	auto lpCyclerGuard = std::shared_ptr<SlopeCycler> (lpCycler);

	// Wire toggle real-time
	hpToggle->onClick = pushParams;
	lpToggle->onClick = pushParams;

	// Wire bar -> text sync
	auto barToText = [aw, syncing, normToFreq, freqToNorm, pushParams, hpBar, lpBar] (const char* editorId, float v01, bool isHp)
	{
		if (*syncing) return;
		*syncing = true;
		if (isHp)
			v01 = juce::jmin (v01, lpBar->value01);
		else
			v01 = juce::jmax (v01, hpBar->value01);

		if (isHp) { hpBar->value01 = v01; hpBar->repaint(); }
		else      { lpBar->value01 = v01; lpBar->repaint(); }

		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setText (juce::String (juce::roundToInt (normToFreq (v01))), juce::sendNotification);
			te->selectAll();
		}
		*syncing = false;
		pushParams();
	};

	hpBar->onValueChanged = [barToText] (float v) { barToText ("hpFreq", v, true); };
	lpBar->onValueChanged = [barToText] (float v) { barToText ("lpFreq", v, false); };

	auto textToBar = [syncing, freqToNorm, normToFreq, pushParams, aw, hpBar, lpBar] (juce::TextEditor* te, PromptBar* bar, bool isHp)
	{
		if (*syncing || te == nullptr || bar == nullptr) return;
		*syncing = true;
		float freq = juce::jlimit (20.0f, 20000.0f, (float) te->getText().getFloatValue());
		auto* otherTe = aw->getTextEditor (isHp ? "lpFreq" : "hpFreq");
		const float otherFreq = otherTe ? juce::jlimit (20.0f, 20000.0f, (float) otherTe->getText().getFloatValue()) : (isHp ? 20000.0f : 20.0f);
		if (isHp) freq = juce::jmin (freq, otherFreq);
		else      freq = juce::jmax (freq, otherFreq);
		te->setText (juce::String (juce::roundToInt (freq)), juce::dontSendNotification);
		bar->value01 = freqToNorm (freq);
		bar->repaint();
		*syncing = false;
		pushParams();
	};

	auto* hpTe = aw->getTextEditor ("hpFreq");
	auto* lpTe = aw->getTextEditor ("lpFreq");

	if (hpTe != nullptr)
		hpTe->onTextChange = [syncing, textToBar, hpTe, hpBar, layoutFn] () { textToBar (hpTe, hpBar, true); if (*layoutFn) (*layoutFn)(); };
	if (lpTe != nullptr)
		lpTe->onTextChange = [syncing, textToBar, lpTe, lpBar, layoutFn] () { textToBar (lpTe, lpBar, false); if (*layoutFn) (*layoutFn)(); };

	// Buttons
	aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);

	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);

	const int margin     = kPromptInnerMargin;
	const int toggleSide = 26;
	const juce::Font promptFont (juce::FontOptions (34.0f).withStyle ("Bold"));

	// Labels: HP name, LP name, Hz labels
	auto* hpNameLabel = new juce::Label ("", "HP");
	hpNameLabel->setJustificationType (juce::Justification::centredLeft);
	hpNameLabel->setColour (juce::Label::textColourId, scheme.text);
	hpNameLabel->setBorderSize (juce::BorderSize<int> (0));
	hpNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (hpNameLabel);

	auto* lpNameLabel = new juce::Label ("", "LP");
	lpNameLabel->setJustificationType (juce::Justification::centredLeft);
	lpNameLabel->setColour (juce::Label::textColourId, scheme.text);
	lpNameLabel->setBorderSize (juce::BorderSize<int> (0));
	lpNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (lpNameLabel);

	auto* hpHzLabel = new juce::Label ("", "Hz");
	hpHzLabel->setJustificationType (juce::Justification::centredLeft);
	hpHzLabel->setColour (juce::Label::textColourId, scheme.text);
	hpHzLabel->setBorderSize (juce::BorderSize<int> (0));
	hpHzLabel->setFont (promptFont);
	aw->addAndMakeVisible (hpHzLabel);

	auto* lpHzLabel = new juce::Label ("", "Hz");
	lpHzLabel->setJustificationType (juce::Justification::centredLeft);
	lpHzLabel->setColour (juce::Label::textColourId, scheme.text);
	lpHzLabel->setBorderSize (juce::BorderSize<int> (0));
	lpHzLabel->setFont (promptFont);
	aw->addAndMakeVisible (lpHzLabel);

	preparePromptTextEditor (*aw, "hpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "lpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	if (auto* hpFreqTe = aw->getTextEditor ("hpFreq"))
		hpFreqTe->setInputFilter (new NumericInputFilter (20.0, 20000.0, 5, 0), true);
	if (auto* lpFreqTe = aw->getTextEditor ("lpFreq"))
		lpFreqTe->setInputFilter (new NumericInputFilter (20.0, 20000.0, 5, 0), true);

	// Toggle forwarder: clicking HP/LP label toggles checkboxes
	struct ToggleForwarder : public juce::MouseListener
	{
		juce::ToggleButton* toggle = nullptr;
		void mouseDown (const juce::MouseEvent&) override
		{
			if (toggle != nullptr)
				toggle->setToggleState (! toggle->getToggleState(), juce::sendNotification);
		}
	};
	hpNameLabel->setInterceptsMouseClicks (true, false);
	auto* hpFwd = new ToggleForwarder();
	hpFwd->toggle = hpToggle;
	hpNameLabel->addMouseListener (hpFwd, false);

	lpNameLabel->setInterceptsMouseClicks (true, false);
	auto* lpFwd = new ToggleForwarder();
	lpFwd->toggle = lpToggle;
	lpNameLabel->addMouseListener (lpFwd, false);

	auto hpFwdGuard = std::shared_ptr<ToggleForwarder> (hpFwd);
	auto lpFwdGuard = std::shared_ptr<ToggleForwarder> (lpFwd);

	// -- Layout function (with slope labels) --
	auto layoutRows = [aw, hpToggle, lpToggle,
	                    hpNameLabel, lpNameLabel, hpHzLabel, lpHzLabel,
	                    hpSlopeLabel, lpSlopeLabel,
	                    hpBar, lpBar, promptFont, slopeFont, toggleSide, margin] ()
	{
		auto* hpTe = aw->getTextEditor ("hpFreq");
		auto* lpTe = aw->getTextEditor ("lpFreq");
		if (hpTe == nullptr || lpTe == nullptr) return;

		const int buttonsTop = getAlertButtonsTop (*aw);
		const int rowH       = hpTe->getHeight();
		const int barH       = juce::jmax (10, rowH / 2);
		const int barGap     = juce::jmax (2, rowH / 6);
		const int gap        = juce::jmax (4, rowH / 3);
		const int rowTotal   = rowH + barGap + barH;
		const int totalH     = rowTotal * 2 + gap;
		const int startY     = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

		const int barX = margin;
		const int barR = aw->getWidth() - margin;

		constexpr int toggleVisualInsetLeft = 2;
		constexpr int tglGap = 4;
		const int toggleVisualSide = juce::jlimit (14,
		                                           juce::jmax (14, toggleSide - 2),
		                                           (int) std::lround ((double) toggleSide * 0.65));
		const int labelOffset = toggleVisualInsetLeft + toggleVisualSide + tglGap;

		const int nameW  = stringWidth (slopeFont, "LP") + 2;
		const int slopeW = stringWidth (slopeFont, "24dB") + 4;
		const int hzGap  = 2;
		const int hzW    = stringWidth (promptFont, "Hz") + 2;

		auto placeRow = [&] (juce::ToggleButton* toggle, juce::Label* nameLabel,
		                     juce::TextEditor* te, juce::Label* hzLabel,
		                     juce::Label* slopeLabel, PromptBar* bar, int y)
		{
			nameLabel->setFont (slopeFont);
			hzLabel->setFont (promptFont);
			slopeLabel->setFont (slopeFont);

			toggle->setBounds (barX, y + (rowH - toggleSide) / 2, toggleSide, toggleSide);
			const int nameX = barX + labelOffset;
			nameLabel->setBounds (nameX, y, nameW, rowH);

			const int slopeX = barR - slopeW;
			slopeLabel->setBounds (slopeX, y, slopeW, rowH);

			const int midL = nameX + nameW;
			const int midR = slopeX;
			const int midW = midR - midL;

			const auto txt   = te->getText();
			const int textW  = juce::jmax (1, stringWidth (promptFont, txt));
			constexpr int kEditorPad = 2;
			const int desiredEditorW = textW + kEditorPad * 2;
			const int editorW = desiredEditorW;
			const int groupW  = editorW + hzGap + hzW;
			const int groupX  = midL + juce::jmax (0, (midW - groupW) / 2);

			te->setBounds (groupX, y, editorW, rowH);
			hzLabel->setBounds (groupX + editorW + hzGap, y, hzW, rowH);

			const int barW = juce::jmax (60, barR - barX);
			bar->setBounds (barX, y + rowH + barGap, barW, barH);
		};

		placeRow (hpToggle, hpNameLabel, hpTe, hpHzLabel, hpSlopeLabel, hpBar, startY);
		placeRow (lpToggle, lpNameLabel, lpTe, lpHzLabel, lpSlopeLabel, lpBar, startY + rowTotal + gap);
	};

	layoutRows();
	*layoutFn = layoutRows;

	preparePromptTextEditor (*aw, "hpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "lpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	layoutRows();

	styleAlertButtons (*aw, lnf);

	// Original values for CANCEL restore
	const float origHpFreq  = hpFreq;
	const float origLpFreq  = lpFreq;
	const bool  origHpOn    = hpOn;
	const bool  origLpOn    = lpOn;
	const int   origHpSlope = hpSlope;
	const int   origLpSlope = lpSlope;

	setPromptOverlayActive (true);

	fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
	{
		layoutAlertWindowButtons (a);
		layoutRows();
	});

	embedAlertWindowInOverlay (safeThis.getComponent(), aw);

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, origHpFreq, origLpFreq, origHpOn, origLpOn,
			 origHpSlope, origLpSlope,
			 hpCyclerGuard, lpCyclerGuard, hpFwdGuard, lpFwdGuard, loaderIndex,
			 hpFreqIdStr, lpFreqIdStr, hpOnIdStr, lpOnIdStr,
			 hpSlopeIdStr, lpSlopeIdStr] (int result)
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			if (result != 1)
			{
				// CANCEL - restore original values
				auto& vts = safeThis->audioProcessor.getValueTreeState();
				auto setP = [&vts] (const juce::String& id, float plain)
				{
					if (auto* param = vts.getParameter (id))
						param->setValueNotifyingHost (param->convertTo0to1 (plain));
				};
				setP (hpFreqIdStr, origHpFreq);
				setP (lpFreqIdStr, origLpFreq);
				setP (hpSlopeIdStr, (float) origHpSlope);
				setP (lpSlopeIdStr, (float) origLpSlope);
				if (auto* hpOnParam = vts.getParameter (hpOnIdStr))
					hpOnParam->setValueNotifyingHost (origHpOn ? 1.0f : 0.0f);
				if (auto* lpOnParam = vts.getParameter (lpOnIdStr))
					lpOnParam->setValueNotifyingHost (origLpOn ? 1.0f : 0.0f);

				auto& fb = loaderIndex == 0 ? safeThis->filterBarA_ : (loaderIndex == 1 ? safeThis->filterBarB_ : safeThis->filterBarC_);
				fb.updateFromProcessor();
			}

			safeThis->setPromptOverlayActive (false);
		}),
		false);
}

// ----------------------------------------------------------------
//  CHAOS prompt (AMOUNT + SPEED)
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::openChaosPrompt (int loaderIndex, bool isFilter)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	const auto& amtId = isFilter
	    ? (loaderIndex == 0 ? SATTRAudioProcessor::kParamChaosAmtFilterA
	       : (loaderIndex == 1 ? SATTRAudioProcessor::kParamChaosAmtFilterB
	                           : SATTRAudioProcessor::kParamChaosAmtFilterC))
	    : (loaderIndex == 0 ? SATTRAudioProcessor::kParamChaosAmtA
	       : (loaderIndex == 1 ? SATTRAudioProcessor::kParamChaosAmtB
	                           : SATTRAudioProcessor::kParamChaosAmtC));
	const auto& spdId = isFilter
	    ? (loaderIndex == 0 ? SATTRAudioProcessor::kParamChaosSpdFilterA
	       : (loaderIndex == 1 ? SATTRAudioProcessor::kParamChaosSpdFilterB
	                           : SATTRAudioProcessor::kParamChaosSpdFilterC))
	    : (loaderIndex == 0 ? SATTRAudioProcessor::kParamChaosSpdA
	       : (loaderIndex == 1 ? SATTRAudioProcessor::kParamChaosSpdB
	                           : SATTRAudioProcessor::kParamChaosSpdC));

	const float currentAmt = audioProcessor.getValueTreeState().getRawParameterValue (amtId)->load();
	const float currentSpd = audioProcessor.getValueTreeState().getRawParameterValue (spdId)->load();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	aw->addTextEditor ("amt", juce::String (juce::roundToInt (currentAmt)), juce::String());
	aw->addTextEditor ("spd", juce::String (currentSpd, 2), juce::String());

	// -- Inline bar component --
	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float value      = 0.5f;
		float defaultVal = 0.5f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial01, float default01)
			: colours (s), value (initial01), defaultVal (default01) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);

			const float pad = 7.0f;
			auto inner = r.reduced (pad);

			g.setColour (colours.bg);
			g.fillRect (inner);

			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (defaultVal); }

		void setValue (float v01)
		{
			value = juce::jlimit (0.0f, 1.0f, v01);
			repaint();
			if (onValueChanged)
				onValueChanged (value);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerX = pad;
			const float innerW = (float) getWidth() - pad * 2.0f;
			const float v = (innerW > 0.0f) ? ((float) e.x - innerX) / innerW : 0.0f;
			setValue (v);
		}
	};

	struct ResetLabel : public juce::Label
	{
		PromptBar* pairedBar = nullptr;
		void mouseDoubleClick (const juce::MouseEvent&) override
		{
			if (pairedBar != nullptr)
				pairedBar->setValue (pairedBar->defaultVal);
		}
	};

	const auto& f = kBoldFont40();

	ResetLabel* amtSuffix  = nullptr;
	ResetLabel* spdSuffix  = nullptr;
	juce::Label* amtUnitLabel = nullptr;
	juce::Label* spdUnitLabel = nullptr;

	auto setupField = [&] (const char* editorId, const juce::String& suffixText,
	                       const juce::String& unitText, bool useDecimalFilter,
	                       ResetLabel*& suffixOut, juce::Label*& unitOut)
	{
		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setFont (f);
			te->applyFontToAllText (f);

			if (useDecimalFilter)
			{
				// Allow digits and one decimal point, max 6 chars
				te->setInputRestrictions (6, "0123456789.");
			}
			else
			{
				te->setInputFilter (new PctInputFilter(), true);
			}

			auto r = te->getBounds();
			r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
			te->setBounds (r);

			suffixOut = new ResetLabel();
			suffixOut->setText (suffixText, juce::dontSendNotification);
			suffixOut->setJustificationType (juce::Justification::centredLeft);
			applyLabelTextColour (*suffixOut, scheme.text);
			suffixOut->setBorderSize (juce::BorderSize<int> (0));
			suffixOut->setFont (f);
			aw->addAndMakeVisible (suffixOut);

			unitOut = new juce::Label ("", unitText);
			unitOut->setJustificationType (juce::Justification::centredLeft);
			applyLabelTextColour (*unitOut, scheme.text);
			unitOut->setBorderSize (juce::BorderSize<int> (0));
			unitOut->setFont (f);
			aw->addAndMakeVisible (unitOut);
		}
	};

	setupField ("amt", "AMT", "%",  false, amtSuffix, amtUnitLabel);
	setupField ("spd", "SPD", "Hz", true,  spdSuffix, spdUnitLabel);

	// Bars: AMOUNT 0x100 -> 0..1, SPEED 0.01x100 Hz -> 0..1 (logarithmic)
	const float spdLogMin = std::log (SATTRAudioProcessor::kChaosSpdMin);
	const float spdLogMax = std::log (SATTRAudioProcessor::kChaosSpdMax);
	const float spdLogRange = spdLogMax - spdLogMin;

	auto hzToBar = [spdLogMin, spdLogRange] (float hz) -> float
	{
		if (hz <= SATTRAudioProcessor::kChaosSpdMin) return 0.0f;
		if (hz >= SATTRAudioProcessor::kChaosSpdMax) return 1.0f;
		return (std::log (hz) - spdLogMin) / spdLogRange;
	};

	auto barToHz = [spdLogMin, spdLogRange] (float v01) -> float
	{
		return std::exp (spdLogMin + v01 * spdLogRange);
	};

	auto* amtBar = new PromptBar (scheme, currentAmt * 0.01f,
	                              SATTRAudioProcessor::kChaosAmtDefault * 0.01f);
	auto* spdBar = new PromptBar (scheme,
	                              hzToBar (currentSpd),
	                              hzToBar (SATTRAudioProcessor::kChaosSpdDefault));
	aw->addAndMakeVisible (amtBar);
	aw->addAndMakeVisible (spdBar);

	if (amtSuffix != nullptr) amtSuffix->pairedBar = amtBar;
	if (spdSuffix != nullptr) spdSuffix->pairedBar = spdBar;

	auto syncing = std::make_shared<bool> (false);

	auto* amtApvts = audioProcessor.getValueTreeState().getParameter (amtId);
	auto* spdApvts = audioProcessor.getValueTreeState().getParameter (spdId);

	// Bar -> text + APVTS
	auto barToTextAmt = [aw, syncing, amtApvts] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		if (auto* te = aw->getTextEditor ("amt"))
		{
			te->setText (juce::String (juce::roundToInt (v01 * 100.0f)), juce::sendNotification);
			te->selectAll();
		}
		if (amtApvts != nullptr)
			amtApvts->setValueNotifyingHost (amtApvts->convertTo0to1 (v01 * 100.0f));
		*syncing = false;
	};

	auto barToTextSpd = [aw, syncing, spdApvts, barToHz] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float hz = juce::jlimit (SATTRAudioProcessor::kChaosSpdMin,
		                               SATTRAudioProcessor::kChaosSpdMax, barToHz (v01));
		if (auto* te = aw->getTextEditor ("spd"))
		{
			te->setText (juce::String (hz, 2), juce::sendNotification);
			te->selectAll();
		}
		if (spdApvts != nullptr)
			spdApvts->setValueNotifyingHost (spdApvts->convertTo0to1 (hz));
		*syncing = false;
	};

	amtBar->onValueChanged = barToTextAmt;
	spdBar->onValueChanged = barToTextSpd;

	// Layout helper
	auto layoutRows = [aw, amtSuffix, spdSuffix, amtUnitLabel, spdUnitLabel, amtBar, spdBar] ()
	{
		auto* amtTe = aw->getTextEditor ("amt");
		auto* spdTe = aw->getTextEditor ("spd");
		if (amtTe == nullptr || spdTe == nullptr)
			return;

		const int buttonsTop = getAlertButtonsTop (*aw);
		const int rowH = amtTe->getHeight();
		const int barH = juce::jmax (10, rowH / 2);
		const int barGap = juce::jmax (2, rowH / 6);
		const int rowTotal = rowH + barGap + barH;
		const int gap = juce::jmax (4, rowH / 3);
		const int totalH = rowTotal * 2 + gap;
		const int startY = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

		const int contentPad = kPromptInlineContentPadPx;
		const int contentW = aw->getWidth() - contentPad * 2;
		const auto& font = amtTe->getFont();
		const int spaceW = juce::jmax (2, stringWidth (font, " "));

		auto placeRow = [&] (juce::TextEditor* te, juce::Label* suffix,
		                     juce::Label* unitLabel, PromptBar* bar, int y)
		{
			if (te == nullptr || suffix == nullptr || bar == nullptr)
				return;

			const int labelW  = stringWidth (suffix->getFont(), suffix->getText()) + 2;
			const auto txt    = te->getText();
			const int textW   = juce::jmax (1, stringWidth (font, txt));
			const int unitW   = (unitLabel != nullptr)
			                  ? stringWidth (font, unitLabel->getText()) + 2 : 0;

			constexpr int kEditorTextPadPx = 12;
			constexpr int kMinEditorWidthPx = 24;
			const int maxEditorWidthPx = (unitLabel != nullptr && unitLabel->getText() == "Hz")
				? juce::jmax (80, stringWidth (font, "100.00") + kEditorTextPadPx * 2)
				: 80;
			const int editorW = juce::jlimit (kMinEditorWidthPx, maxEditorWidthPx,
			                                  textW + kEditorTextPadPx * 2);

			const int visualW = labelW + spaceW + textW + unitW;
			const int centerX = contentPad + contentW / 2;
			int blockLeft = juce::jlimit (contentPad,
			                              juce::jmax (contentPad, contentPad + contentW - visualW),
			                              centerX - visualW / 2);

			suffix->setBounds (blockLeft, y, labelW, rowH);

			int teX = blockLeft + labelW + spaceW - (editorW - textW) / 2;
			teX = juce::jlimit (contentPad,
			                    juce::jmax (contentPad, contentPad + contentW - editorW), teX);
			te->setBounds (teX, y, editorW, rowH);

			if (unitLabel != nullptr)
			{
				const int textRightX = blockLeft + labelW + spaceW + textW;
				unitLabel->setBounds (textRightX, y, unitW, rowH);
			}

			const int barX = kPromptInnerMargin;
			const int barW = juce::jmax (60, aw->getWidth() - kPromptInnerMargin * 2);
			bar->setBounds (barX, y + rowH + barGap, barW, barH);
		};

		placeRow (amtTe, amtSuffix, amtUnitLabel, amtBar, startY);
		placeRow (spdTe, spdSuffix, spdUnitLabel, spdBar, startY + rowTotal + gap);
	};

	// Text -> bar + APVTS
	auto textToBar = [syncing, hzToBar] (juce::TextEditor* te, PromptBar* bar,
	                            juce::RangedAudioParameter* param, bool isSpeed)
	{
		if (*syncing || te == nullptr || bar == nullptr) return;
		*syncing = true;
		const float raw = juce::jlimit (0.0f, 100.0f, te->getText().getFloatValue());
		if (isSpeed)
		{
			const float hz = juce::jlimit (SATTRAudioProcessor::kChaosSpdMin,
			                               SATTRAudioProcessor::kChaosSpdMax, raw);
			bar->value = hzToBar (hz);
			if (param != nullptr)
				param->setValueNotifyingHost (param->convertTo0to1 (hz));
		}
		else
		{
			bar->value = raw * 0.01f;
			if (param != nullptr)
				param->setValueNotifyingHost (param->convertTo0to1 (raw));
		}
		bar->repaint();
		*syncing = false;
	};

	if (auto* amtTe = aw->getTextEditor ("amt"))
		amtTe->onTextChange = [layoutRows, amtTe, amtBar, textToBar, amtApvts] () mutable
		{
			textToBar (amtTe, amtBar, amtApvts, false);
			layoutRows();
		};
	if (auto* spdTe = aw->getTextEditor ("spd"))
		spdTe->onTextChange = [layoutRows, spdTe, spdBar, textToBar, spdApvts] () mutable
		{
			textToBar (spdTe, spdBar, spdApvts, true);
			layoutRows();
		};

	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);
	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);
	layoutRows();

	const auto& kChaosFont = kBoldFont40();
	preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
	preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
	layoutRows();

	styleAlertButtons (*aw, lnf);

	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
		{
			juce::ignoreUnused (a);
			layoutAlertWindowButtons (a);
			layoutRows();
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
	}

	// Final styling pass
	{
		preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
		preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
		layoutRows();

		if (amtSuffix != nullptr)
		{
			if (auto* te = aw->getTextEditor ("amt"))
			{
				amtSuffix->setFont (te->getFont());
				if (amtUnitLabel != nullptr) amtUnitLabel->setFont (te->getFont());
			}
		}
		if (spdSuffix != nullptr)
		{
			if (auto* te = aw->getTextEditor ("spd"))
			{
				spdSuffix->setFont (te->getFont());
				if (spdUnitLabel != nullptr) spdUnitLabel->setFont (te->getFont());
			}
		}

		layoutRows();

		juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
		juce::MessageManager::callAsync ([safeAw]()
		{
			if (safeAw == nullptr) return;
			bringPromptWindowToFront (*safeAw);
			safeAw->repaint();
		});
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, amtBar, spdBar,
			 savedAmt = currentAmt, savedSpd = currentSpd,
			 amtId, spdId, loaderIndex, isFilter, spdLogMin, spdLogRange] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);

			if (safeThis == nullptr)
				return;

			if (result != 1)
			{
				// CANCEL: revert to original values
				if (auto* p = safeThis->audioProcessor.getValueTreeState().getParameter (amtId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedAmt));
				if (auto* p = safeThis->audioProcessor.getValueTreeState().getParameter (spdId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedSpd));
				return;
			}

			// OK: update tooltip
			const float newAmt = juce::jlimit (0.0f, 100.0f, amtBar->value * 100.0f);
			const float newSpd = juce::jlimit (SATTRAudioProcessor::kChaosSpdMin,
			                                    SATTRAudioProcessor::kChaosSpdMax,
			                                    std::exp (spdLogMin + juce::jlimit (0.0f, 1.0f, spdBar->value) * spdLogRange));
			auto tip = formatChaosTooltip (newAmt, newSpd);
			auto& button = isFilter
			             ? (loaderIndex == 0 ? safeThis->chaosFilterButtonA : (loaderIndex == 1 ? safeThis->chaosFilterButtonB : safeThis->chaosFilterButtonC))
			             : (loaderIndex == 0 ? safeThis->chaosButtonA       : (loaderIndex == 1 ? safeThis->chaosButtonB       : safeThis->chaosButtonC));
			button.setTooltip (tip);
		}),
		false);
}

// ----------------------------------------------------------------
//  EXP Prompt - right-click on EXP button opens ORDER/RATIO/THRESHOLD
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::openExpPrompt (int loaderIndex)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	setPromptOverlayActive (true);

	const auto& orderParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpOrderA
	                         : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpOrderB
	                                            : SATTRAudioProcessor::kParamExpOrderC;
	const auto& ratioParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpRatioA
	                         : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpRatioB
	                                            : SATTRAudioProcessor::kParamExpRatioC;
	const auto& threshParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpThreshA
	                          : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpThreshB
	                                             : SATTRAudioProcessor::kParamExpThreshC;
	const auto& kneeParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpKneeA
	                        : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpKneeB
	                                           : SATTRAudioProcessor::kParamExpKneeC;
	const auto& atkParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpAtkA
	                       : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpAtkB
	                                          : SATTRAudioProcessor::kParamExpAtkC;
	const auto& relParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpRelA
	                       : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpRelB
	                                          : SATTRAudioProcessor::kParamExpRelC;
	const auto& scHpParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScHpA
	                        : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScHpB
	                                           : SATTRAudioProcessor::kParamExpScHpC;
	const auto& scLpParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScLpA
	                        : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScLpB
	                                           : SATTRAudioProcessor::kParamExpScLpC;
	const auto& scHpOnParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScHpOnA
	                          : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScHpOnB
	                                             : SATTRAudioProcessor::kParamExpScHpOnC;
	const auto& scLpOnParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScLpOnA
	                          : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScLpOnB
	                                             : SATTRAudioProcessor::kParamExpScLpOnC;
	const auto& scHpSlopeParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScHpSlopeA
	                             : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScHpSlopeB
	                                                : SATTRAudioProcessor::kParamExpScHpSlopeC;
	const auto& scLpSlopeParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScLpSlopeA
	                             : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScLpSlopeB
	                                                : SATTRAudioProcessor::kParamExpScLpSlopeC;
	const auto& scGainParamId = loaderIndex == 0 ? SATTRAudioProcessor::kParamExpScGainA
	                          : loaderIndex == 1 ? SATTRAudioProcessor::kParamExpScGainB
	                                             : SATTRAudioProcessor::kParamExpScGainC;

	const bool currentOrder = audioProcessor.getValueTreeState().getRawParameterValue (orderParamId)->load() >= 0.5f;
	const float currentRatio = audioProcessor.getValueTreeState().getRawParameterValue (ratioParamId)->load();
	const float currentThresh = audioProcessor.getValueTreeState().getRawParameterValue (threshParamId)->load();
	const float currentKnee = audioProcessor.getValueTreeState().getRawParameterValue (kneeParamId)->load();
	const float currentAtk = audioProcessor.getValueTreeState().getRawParameterValue (atkParamId)->load();
	const float currentRel = audioProcessor.getValueTreeState().getRawParameterValue (relParamId)->load();
	const float currentScHp = audioProcessor.getValueTreeState().getRawParameterValue (scHpParamId)->load();
	const float currentScLp = audioProcessor.getValueTreeState().getRawParameterValue (scLpParamId)->load();
	const bool currentScHpOn = audioProcessor.getValueTreeState().getRawParameterValue (scHpOnParamId)->load() >= 0.5f;
	const bool currentScLpOn = audioProcessor.getValueTreeState().getRawParameterValue (scLpOnParamId)->load() >= 0.5f;
	const int currentScHpSlope = juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin,
	                                           SATTRAudioProcessor::kFilterSlopeMax,
	                                           (int) std::lround (audioProcessor.getValueTreeState().getRawParameterValue (scHpSlopeParamId)->load()));
	const int currentScLpSlope = juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin,
	                                           SATTRAudioProcessor::kFilterSlopeMax,
	                                           (int) std::lround (audioProcessor.getValueTreeState().getRawParameterValue (scLpSlopeParamId)->load()));
	const float currentScGain = audioProcessor.getValueTreeState().getRawParameterValue (scGainParamId)->load();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float value = 0.5f;
		float defaultVal = 0.5f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial01, float default01)
			: colours (s), value (initial01), defaultVal (default01) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);
			const float pad = 7.0f;
			auto inner = r.reduced (pad);
			g.setColour (colours.bg);
			g.fillRect (inner);
			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (defaultVal); }

		void setValue (float v01)
		{
			value = juce::jlimit (0.0f, 1.0f, v01);
			repaint();
			if (onValueChanged) onValueChanged (value);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerX = pad;
			const float innerW = (float) getWidth() - pad * 2.0f;
			const float v = (innerW > 0.0f) ? ((float) e.x - innerX) / innerW : 0.0f;
			setValue (v);
		}
	};

	struct ResetLabel : public juce::Label
	{
		PromptBar* pairedBar = nullptr;
		void mouseDoubleClick (const juce::MouseEvent&) override
		{
			if (pairedBar != nullptr)
				pairedBar->setValue (pairedBar->defaultVal);
		}
	};

	const auto& f = kBoldFont40();

	auto* bodyContent = new juce::Component();
	bodyContent->setComponentID ("expBody");

	auto* orderLegend = new juce::Label ("", "ORDER:");
	orderLegend->setJustificationType (juce::Justification::centredRight);
	applyLabelTextColour (*orderLegend, scheme.text);
	orderLegend->setFont (f);
	orderLegend->setBorderSize (juce::BorderSize<int> (0));
	bodyContent->addAndMakeVisible (orderLegend);

	auto* orderLabel = new juce::Label ("", currentOrder ? "POST" : "PRE");
	orderLabel->setJustificationType (juce::Justification::centredLeft);
	applyLabelTextColour (*orderLabel, scheme.text);
	orderLabel->setFont (f);
	orderLabel->setInterceptsMouseClicks (true, false);
	bodyContent->addAndMakeVisible (orderLabel);

	aw->addTextEditor ("thresh", juce::String (currentThresh, 1), juce::String());
	aw->addTextEditor ("ratio", formatExpRatioDisplay (currentRatio), juce::String());
	aw->addTextEditor ("knee", juce::String (currentKnee, 1), juce::String());
	aw->addTextEditor ("atk", juce::String (currentAtk, 2), juce::String());
	aw->addTextEditor ("rel", juce::String (currentRel, 2), juce::String());
	aw->addTextEditor ("scGain", juce::String (currentScGain, 1), juce::String());
	aw->addTextEditor ("scHp", juce::String (juce::roundToInt (currentScHp)), juce::String());
	aw->addTextEditor ("scLp", juce::String (juce::roundToInt (currentScLp)), juce::String());

	for (auto* edId : { "thresh", "ratio", "knee", "atk", "rel", "scGain", "scHp", "scLp" })
		if (auto* te = aw->getTextEditor (edId))
			bodyContent->addChildComponent (te);

	ResetLabel* ratioSuffix = nullptr; juce::Label* ratioUnit = nullptr;
	ResetLabel* threshSuffix = nullptr; juce::Label* threshUnit = nullptr;
	ResetLabel* kneeSuffix = nullptr; juce::Label* kneeUnit = nullptr;
	ResetLabel* atkSuffix = nullptr; juce::Label* atkUnit = nullptr;
	ResetLabel* relSuffix = nullptr; juce::Label* relUnit = nullptr;
	ResetLabel* scGainSuffix = nullptr; juce::Label* scGainUnit = nullptr;

	auto setupField = [&] (const char* editorId, const juce::String& suffixText,
	                       const juce::String& unitText,
	                       ResetLabel*& suffixOut, juce::Label*& unitOut)
	{
		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setFont (f);
			te->applyFontToAllText (f);
			const juce::String id (editorId);
			const int maxChars = (id == "thresh" ? 5
			                    : id == "ratio"  ? 4
			                    : id == "knee"   ? 4
			                    : id == "atk"    ? 6
			                    : id == "scGain" ? 6
			                                       : 7);
			const juce::String allowed = (id == "thresh" || id == "scGain") ? "0123456789.-"
			                                                                : "0123456789.";
			te->setInputRestrictions (maxChars, allowed);
			auto r = te->getBounds();
			r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
			te->setBounds (r);
			te->setVisible (true);

			suffixOut = new ResetLabel();
			suffixOut->setText (suffixText, juce::dontSendNotification);
			suffixOut->setJustificationType (juce::Justification::centredRight);
			applyLabelTextColour (*suffixOut, scheme.text);
			suffixOut->setBorderSize (juce::BorderSize<int> (0));
			suffixOut->setFont (f);
			bodyContent->addAndMakeVisible (suffixOut);

			unitOut = new juce::Label ("", unitText);
			unitOut->setJustificationType (juce::Justification::centredLeft);
			applyLabelTextColour (*unitOut, scheme.text);
			unitOut->setBorderSize (juce::BorderSize<int> (0));
			unitOut->setFont (f);
			bodyContent->addAndMakeVisible (unitOut);
		}
	};

	setupField ("thresh", "THRESH", "dB", threshSuffix, threshUnit);
	setupField ("ratio", "RATIO 1", ":", ratioSuffix, ratioUnit);
	setupField ("knee", "KNEE", "dB", kneeSuffix, kneeUnit);
	setupField ("atk", "ATK", "ms", atkSuffix, atkUnit);
	setupField ("rel", "REL", "ms", relSuffix, relUnit);
	setupField ("scGain", "GAIN", "dB", scGainSuffix, scGainUnit);

	for (auto* editorId : { "scHp", "scLp" })
	{
		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setFont (f);
			te->applyFontToAllText (f);
			te->setInputRestrictions (5, "0123456789");
			auto r = te->getBounds();
			r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
			te->setBounds (r);
			te->setVisible (true);
		}
	}

	auto* sidechainLabel = new juce::Label ("", "SIDECHAIN");
	sidechainLabel->setJustificationType (juce::Justification::centred);
	applyLabelTextColour (*sidechainLabel, scheme.text);
	sidechainLabel->setBorderSize (juce::BorderSize<int> (0));
	sidechainLabel->setFont (f);
	bodyContent->addAndMakeVisible (sidechainLabel);

	auto* scFilterLabel = new juce::Label ("", "FILTER");
	scFilterLabel->setJustificationType (juce::Justification::centred);
	applyLabelTextColour (*scFilterLabel, scheme.text);
	scFilterLabel->setBorderSize (juce::BorderSize<int> (0));
	scFilterLabel->setFont (f);
	bodyContent->addAndMakeVisible (scFilterLabel);

	auto* scHpToggle = new juce::ToggleButton ("");
	scHpToggle->setToggleState (currentScHpOn, juce::dontSendNotification);
	scHpToggle->setLookAndFeel (&lnf);
	bodyContent->addAndMakeVisible (scHpToggle);

	auto* scLpToggle = new juce::ToggleButton ("");
	scLpToggle->setToggleState (currentScLpOn, juce::dontSendNotification);
	scLpToggle->setLookAndFeel (&lnf);
	bodyContent->addAndMakeVisible (scLpToggle);

	auto makePromptLabel = [&] (const juce::String& text, juce::Justification justification)
	{
		auto* label = new juce::Label ("", text);
		label->setJustificationType (justification);
		applyLabelTextColour (*label, scheme.text);
		label->setBorderSize (juce::BorderSize<int> (0));
		label->setFont (f);
		bodyContent->addAndMakeVisible (label);
		return label;
	};

	auto* scHpNameLabel = makePromptLabel ("HP", juce::Justification::centredLeft);
	auto* scLpNameLabel = makePromptLabel ("LP", juce::Justification::centredLeft);
	auto* scHpUnitLabel = makePromptLabel ("Hz", juce::Justification::centredLeft);
	auto* scLpUnitLabel = makePromptLabel ("Hz", juce::Justification::centredLeft);
	auto* scHpSlopeLabel = makePromptLabel (filterSlopeToText (currentScHpSlope), juce::Justification::centredRight);
	auto* scLpSlopeLabel = makePromptLabel (filterSlopeToText (currentScLpSlope), juce::Justification::centredRight);
	scHpNameLabel->setInterceptsMouseClicks (true, false);
	scLpNameLabel->setInterceptsMouseClicks (true, false);
	scHpSlopeLabel->setInterceptsMouseClicks (true, false);
	scLpSlopeLabel->setInterceptsMouseClicks (true, false);

	const float threshNorm = (currentThresh - SATTRAudioProcessor::kExpThreshMin)
	                       / (SATTRAudioProcessor::kExpThreshMax - SATTRAudioProcessor::kExpThreshMin);
	const float ratioNorm = expRatioDisplayToNorm (currentRatio);
	const float kneeNorm = (currentKnee - SATTRAudioProcessor::kExpKneeMin)
	                     / (SATTRAudioProcessor::kExpKneeMax - SATTRAudioProcessor::kExpKneeMin);

	auto atkNormRange = juce::NormalisableRange<float> (SATTRAudioProcessor::kExpAtkMin, SATTRAudioProcessor::kExpAtkMax, 0.01f, 0.3f);
	auto relNormRange = juce::NormalisableRange<float> (SATTRAudioProcessor::kExpRelMin, SATTRAudioProcessor::kExpRelMax, 0.01f, 0.3f);
	const float atkNorm = atkNormRange.convertTo0to1 (currentAtk);
	const float relNorm = relNormRange.convertTo0to1 (currentRel);
	auto scGainNormRange = juce::NormalisableRange<float> (SATTRAudioProcessor::kExpScGainMin,
	                                                       SATTRAudioProcessor::kExpScGainMax,
	                                                       0.0f,
	                                                       SATTRAudioProcessor::kGainSkew);

	auto freqToNorm = [] (float freq) -> float
	{
		constexpr float minF = SATTRAudioProcessor::kExpScFreqMin;
		constexpr float maxF = SATTRAudioProcessor::kExpScFreqMax;
		return std::log2 (juce::jlimit (minF, maxF, freq) / minF) / std::log2 (maxF / minF);
	};

	auto normToFreq = [] (float n) -> float
	{
		constexpr float minF = SATTRAudioProcessor::kExpScFreqMin;
		constexpr float maxF = SATTRAudioProcessor::kExpScFreqMax;
		return minF * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, n) * std::log2 (maxF / minF));
	};

	const float scHpNorm = freqToNorm (currentScHp);
	const float scLpNorm = freqToNorm (currentScLp);
	const float scGainNorm = scGainNormRange.convertTo0to1 (currentScGain);
	auto scHpSlopeState = std::make_shared<int> (currentScHpSlope);
	auto scLpSlopeState = std::make_shared<int> (currentScLpSlope);

	auto* threshBar = new PromptBar (scheme, threshNorm, 1.0f);
	auto* ratioBar = new PromptBar (scheme, ratioNorm, expRatioDisplayToNorm (SATTRAudioProcessor::kExpRatioDefault));
	auto* kneeBar = new PromptBar (scheme, kneeNorm, 0.0f);
	auto* atkBar = new PromptBar (scheme, atkNorm, atkNormRange.convertTo0to1 (SATTRAudioProcessor::kExpAtkDefault));
	auto* relBar = new PromptBar (scheme, relNorm, relNormRange.convertTo0to1 (SATTRAudioProcessor::kExpRelDefault));
	auto* scGainBar = new PromptBar (scheme, scGainNorm, scGainNormRange.convertTo0to1 (SATTRAudioProcessor::kExpScGainDefault));
	auto* scHpBar = new PromptBar (scheme, scHpNorm, freqToNorm (SATTRAudioProcessor::kExpScHpDefault));
	auto* scLpBar = new PromptBar (scheme, scLpNorm, freqToNorm (SATTRAudioProcessor::kExpScLpDefault));
	bodyContent->addAndMakeVisible (threshBar);
	bodyContent->addAndMakeVisible (ratioBar);
	bodyContent->addAndMakeVisible (kneeBar);
	bodyContent->addAndMakeVisible (atkBar);
	bodyContent->addAndMakeVisible (relBar);
	bodyContent->addAndMakeVisible (scGainBar);
	bodyContent->addAndMakeVisible (scHpBar);
	bodyContent->addAndMakeVisible (scLpBar);

	if (threshSuffix != nullptr) threshSuffix->pairedBar = threshBar;
	if (ratioSuffix != nullptr) ratioSuffix->pairedBar = ratioBar;
	if (kneeSuffix != nullptr) kneeSuffix->pairedBar = kneeBar;
	if (atkSuffix != nullptr) atkSuffix->pairedBar = atkBar;
	if (relSuffix != nullptr) relSuffix->pairedBar = relBar;
	if (scGainSuffix != nullptr) scGainSuffix->pairedBar = scGainBar;

	auto* viewport = new juce::Viewport();
	viewport->setComponentID ("expViewport");
	viewport->setViewedComponent (bodyContent, true);
	viewport->setScrollBarsShown (true, false);
	viewport->setScrollBarThickness (8);
	viewport->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (viewport);

	auto syncing = std::make_shared<bool> (false);

	struct EnvPromptNumericSpec
	{
		bool allowNegative = false;
		int maxIntegerDigits = 1;
		int maxFractionDigits = 0;
		float minValue = 0.0f;
		float maxValue = 0.0f;
		int formatDecimals = 0;
	};

	const EnvPromptNumericSpec threshSpec { true, 2, 1, SATTRAudioProcessor::kExpThreshMin, SATTRAudioProcessor::kExpThreshMax, 1 };
	const EnvPromptNumericSpec ratioSpec { false, 2, 1, SATTRAudioProcessor::kExpRatioMin, SATTRAudioProcessor::kExpRatioMax, 1 };
	const EnvPromptNumericSpec kneeSpec { false, 2, 1, SATTRAudioProcessor::kExpKneeMin, SATTRAudioProcessor::kExpKneeMax, 1 };
	const EnvPromptNumericSpec atkSpec { false, 3, 2, SATTRAudioProcessor::kExpAtkMin, SATTRAudioProcessor::kExpAtkMax, 2 };
	const EnvPromptNumericSpec relSpec { false, 4, 2, SATTRAudioProcessor::kExpRelMin, SATTRAudioProcessor::kExpRelMax, 2 };
	const EnvPromptNumericSpec scGainSpec { true, 3, 1, SATTRAudioProcessor::kExpScGainMin, SATTRAudioProcessor::kExpScGainMax, 1 };
	const EnvPromptNumericSpec scHpSpec { false, 5, 0, SATTRAudioProcessor::kExpScFreqMin, SATTRAudioProcessor::kExpScFreqMax, 0 };
	const EnvPromptNumericSpec scLpSpec { false, 5, 0, SATTRAudioProcessor::kExpScFreqMin, SATTRAudioProcessor::kExpScFreqMax, 0 };

	auto sanitiseEnvPromptText = [] (juce::String text, const EnvPromptNumericSpec& spec,
	                                 bool& incompleteOut) -> juce::String
	{
		juce::String sign, integer, fraction;
		bool sawDot = false;

		for (int i = 0; i < text.length(); ++i)
		{
			const auto ch = text[i];
			if (ch >= '0' && ch <= '9')
			{
				if (! sawDot)
				{
					if (integer.length() < spec.maxIntegerDigits)
						integer += ch;
				}
				else if (fraction.length() < spec.maxFractionDigits)
				{
					fraction += ch;
				}
			}
			else if (ch == '-' && spec.allowNegative && sign.isEmpty() && integer.isEmpty() && ! sawDot && fraction.isEmpty())
			{
				sign = "-";
			}
			else if (ch == '.' && ! sawDot && spec.maxFractionDigits > 0)
			{
				sawDot = true;
			}
		}

		if (integer.isEmpty() && sawDot)
			integer = "0";

		juce::String out = sign + integer;
		if (sawDot)
			out += "." + fraction;

		incompleteOut = (out.isEmpty() || out == "-" || out.endsWithChar ('.'));

		return out;
	};

	auto parseEnvPromptValue = [sanitiseEnvPromptText] (juce::TextEditor* te,
	                                                    const EnvPromptNumericSpec& spec,
	                                                    float fallback) -> float
	{
		if (te == nullptr)
			return fallback;

		bool incomplete = false;
		auto sanitised = sanitiseEnvPromptText (te->getText(), spec, incomplete);
		if (sanitised.isEmpty() || sanitised == "-" || incomplete)
			return fallback;

		if (sanitised.endsWithChar ('.'))
			sanitised = sanitised.dropLastCharacters (1);

		if (sanitised.isEmpty() || sanitised == "-")
			return fallback;

		return juce::jlimit (spec.minValue, spec.maxValue, (float) sanitised.getDoubleValue());
	};

	auto* threshApvts = audioProcessor.getValueTreeState().getParameter (threshParamId);
	auto* ratioApvts = audioProcessor.getValueTreeState().getParameter (ratioParamId);
	auto* kneeApvts = audioProcessor.getValueTreeState().getParameter (kneeParamId);
	auto* orderApvts = audioProcessor.getValueTreeState().getParameter (orderParamId);
	auto* atkApvts = audioProcessor.getValueTreeState().getParameter (atkParamId);
	auto* relApvts = audioProcessor.getValueTreeState().getParameter (relParamId);
	auto* scHpApvts = audioProcessor.getValueTreeState().getParameter (scHpParamId);
	auto* scLpApvts = audioProcessor.getValueTreeState().getParameter (scLpParamId);
	auto* scHpOnApvts = audioProcessor.getValueTreeState().getParameter (scHpOnParamId);
	auto* scLpOnApvts = audioProcessor.getValueTreeState().getParameter (scLpOnParamId);
	auto* scHpSlopeApvts = audioProcessor.getValueTreeState().getParameter (scHpSlopeParamId);
	auto* scLpSlopeApvts = audioProcessor.getValueTreeState().getParameter (scLpSlopeParamId);
	auto* scGainApvts = audioProcessor.getValueTreeState().getParameter (scGainParamId);

	auto orderState = std::make_shared<bool> (currentOrder);
	struct OrderClickHandler : public juce::MouseListener
	{
		juce::Label* label = nullptr;
		std::shared_ptr<bool> state;
		juce::RangedAudioParameter* param = nullptr;
		void mouseDown (const juce::MouseEvent&) override
		{
			*state = !(*state);
			label->setText (*state ? "POST" : "PRE", juce::dontSendNotification);
			if (param != nullptr) param->setValueNotifyingHost (*state ? 1.0f : 0.0f);
		}
	};
	auto* orderHandler = new OrderClickHandler();
	orderHandler->label = orderLabel;
	orderHandler->state = orderState;
	orderHandler->param = orderApvts;
	orderLabel->addMouseListener (orderHandler, false);

	struct ToggleLabelForwarder : public juce::MouseListener
	{
		juce::ToggleButton* toggle = nullptr;
		void mouseDown (const juce::MouseEvent&) override
		{
			if (toggle != nullptr)
			{
				toggle->setToggleState (! toggle->getToggleState(), juce::dontSendNotification);
				if (toggle->onClick)
					toggle->onClick();
			}
		}
	};

	auto* scHpToggleHandler = new ToggleLabelForwarder();
	scHpToggleHandler->toggle = scHpToggle;
	scHpNameLabel->addMouseListener (scHpToggleHandler, false);

	auto* scLpToggleHandler = new ToggleLabelForwarder();
	scLpToggleHandler->toggle = scLpToggle;
	scLpNameLabel->addMouseListener (scLpToggleHandler, false);

	auto pushScToggle = [] (juce::RangedAudioParameter* param, bool enabled)
	{
		if (param != nullptr)
			param->setValueNotifyingHost (enabled ? 1.0f : 0.0f);
	};

	scHpToggle->onClick = [scHpToggle, scHpOnApvts, pushScToggle] { pushScToggle (scHpOnApvts, scHpToggle->getToggleState()); };
	scLpToggle->onClick = [scLpToggle, scLpOnApvts, pushScToggle] { pushScToggle (scLpOnApvts, scLpToggle->getToggleState()); };

	struct SlopeClickHandler : public juce::MouseListener
	{
		std::shared_ptr<int> state;
		juce::Label* label = nullptr;
		juce::RangedAudioParameter* param = nullptr;
		void mouseDown (const juce::MouseEvent&) override
		{
			*state = (*state + 1) % 3;
			if (label != nullptr)
				label->setText (filterSlopeToText (*state), juce::dontSendNotification);
			if (param != nullptr)
				param->setValueNotifyingHost (param->convertTo0to1 ((float) *state));
		}
	};

	auto* scHpSlopeHandler = new SlopeClickHandler();
	scHpSlopeHandler->state = scHpSlopeState;
	scHpSlopeHandler->label = scHpSlopeLabel;
	scHpSlopeHandler->param = scHpSlopeApvts;
	scHpSlopeLabel->addMouseListener (scHpSlopeHandler, false);

	auto* scLpSlopeHandler = new SlopeClickHandler();
	scLpSlopeHandler->state = scLpSlopeState;
	scLpSlopeHandler->label = scLpSlopeLabel;
	scLpSlopeHandler->param = scLpSlopeApvts;
	scLpSlopeLabel->addMouseListener (scLpSlopeHandler, false);

	ratioBar->onValueChanged = [aw, syncing, ratioApvts] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float ratio = expRatioNormToDisplay (v01);
		if (auto* te = aw->getTextEditor ("ratio"))
		{
			te->setText (formatExpRatioDisplay (ratio), juce::sendNotification);
			te->selectAll();
		}
		if (ratioApvts) ratioApvts->setValueNotifyingHost (ratioApvts->convertTo0to1 (ratio));
		*syncing = false;
	};

	threshBar->onValueChanged = [aw, syncing, threshApvts] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float thresh = SATTRAudioProcessor::kExpThreshMin
		                   + v01 * (SATTRAudioProcessor::kExpThreshMax - SATTRAudioProcessor::kExpThreshMin);
		if (auto* te = aw->getTextEditor ("thresh"))
		{
			te->setText (juce::String (thresh, 1), juce::sendNotification);
			te->selectAll();
		}
		if (threshApvts) threshApvts->setValueNotifyingHost (threshApvts->convertTo0to1 (thresh));
		*syncing = false;
	};

	kneeBar->onValueChanged = [aw, syncing, kneeApvts] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float knee = SATTRAudioProcessor::kExpKneeMin
		                 + v01 * (SATTRAudioProcessor::kExpKneeMax - SATTRAudioProcessor::kExpKneeMin);
		if (auto* te = aw->getTextEditor ("knee"))
		{
			te->setText (juce::String (knee, 1), juce::sendNotification);
			te->selectAll();
		}
		if (kneeApvts) kneeApvts->setValueNotifyingHost (kneeApvts->convertTo0to1 (knee));
		*syncing = false;
	};

	atkBar->onValueChanged = [aw, syncing, atkApvts, atkNormRange] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float val = atkNormRange.convertFrom0to1 (v01);
		if (auto* te = aw->getTextEditor ("atk"))
		{
			te->setText (juce::String (val, 2), juce::sendNotification);
			te->selectAll();
		}
		if (atkApvts) atkApvts->setValueNotifyingHost (atkApvts->convertTo0to1 (val));
		*syncing = false;
	};

	relBar->onValueChanged = [aw, syncing, relApvts, relNormRange] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float val = relNormRange.convertFrom0to1 (v01);
		if (auto* te = aw->getTextEditor ("rel"))
		{
			te->setText (juce::String (val, 2), juce::sendNotification);
			te->selectAll();
		}
		if (relApvts) relApvts->setValueNotifyingHost (relApvts->convertTo0to1 (val));
		*syncing = false;
	};

	scHpBar->onValueChanged = [aw, syncing, scHpApvts, normToFreq, scHpBar, scLpBar] (float v01) mutable
	{
		if (*syncing) return;
		*syncing = true;
		v01 = juce::jmin (v01, scLpBar->value);
		scHpBar->value = v01;
		scHpBar->repaint();
		const float val = normToFreq (v01);
		if (auto* te = aw->getTextEditor ("scHp"))
		{
			te->setText (juce::String (juce::roundToInt (val)), juce::sendNotification);
			te->selectAll();
		}
		if (scHpApvts) scHpApvts->setValueNotifyingHost (scHpApvts->convertTo0to1 ((float) juce::roundToInt (val)));
		*syncing = false;
	};

	scLpBar->onValueChanged = [aw, syncing, scLpApvts, normToFreq, scHpBar, scLpBar] (float v01) mutable
	{
		if (*syncing) return;
		*syncing = true;
		v01 = juce::jmax (v01, scHpBar->value);
		scLpBar->value = v01;
		scLpBar->repaint();
		const float val = normToFreq (v01);
		if (auto* te = aw->getTextEditor ("scLp"))
		{
			te->setText (juce::String (juce::roundToInt (val)), juce::sendNotification);
			te->selectAll();
		}
		if (scLpApvts) scLpApvts->setValueNotifyingHost (scLpApvts->convertTo0to1 ((float) juce::roundToInt (val)));
		*syncing = false;
	};

	scGainBar->onValueChanged = [aw, syncing, scGainApvts, scGainNormRange] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float val = scGainNormRange.convertFrom0to1 (v01);
		if (auto* te = aw->getTextEditor ("scGain"))
		{
			te->setText (juce::String (val, 1), juce::sendNotification);
			te->selectAll();
		}
		if (scGainApvts) scGainApvts->setValueNotifyingHost (scGainApvts->convertTo0to1 (val));
		*syncing = false;
	};

	auto layoutBody = [aw, viewport, bodyContent,
	                   orderLegend, orderLabel,
	                   sidechainLabel, scFilterLabel,
	                   scHpToggle, scLpToggle,
	                   scHpNameLabel, scLpNameLabel,
	                   scHpUnitLabel, scLpUnitLabel,
	                   scHpSlopeLabel, scLpSlopeLabel,
	                   threshSuffix, ratioSuffix, kneeSuffix, atkSuffix, relSuffix, scGainSuffix,
	                   threshUnit, ratioUnit, kneeUnit, atkUnit, relUnit, scGainUnit,
	                   threshBar, ratioBar, kneeBar, atkBar, relBar, scGainBar, scHpBar, scLpBar] ()
	{
		auto* threshTe = aw->getTextEditor ("thresh");
		auto* ratioTe = aw->getTextEditor ("ratio");
		auto* kneeTe = aw->getTextEditor ("knee");
		auto* atkTe = aw->getTextEditor ("atk");
		auto* relTe = aw->getTextEditor ("rel");
		auto* scGainTe = aw->getTextEditor ("scGain");
		auto* scHpTe = aw->getTextEditor ("scHp");
		auto* scLpTe = aw->getTextEditor ("scLp");
		if (! threshTe || ! ratioTe || ! kneeTe || ! atkTe || ! relTe || ! scGainTe || ! scHpTe || ! scLpTe) return;

		layoutAlertWindowButtons (*aw);
		const int vpTop = kPromptBodyTopPad;
		const int vpBot = getAlertButtonsTop (*aw) - kPromptBodyBottomPad;
		const int vpH = juce::jmax (40, vpBot - vpTop);
		const int vpX = kPromptInnerMargin;
		const int vpW = juce::jmax (60, aw->getWidth() - kPromptInnerMargin * 2);
		viewport->setBounds (vpX, vpTop, vpW, vpH);

		const int rowH = threshTe->getHeight();
		const int barH = juce::jmax (8, rowH / 3);
		const int barGap = juce::jmax (2, rowH / 8);
		const int rowTotal = rowH + barGap + barH;
		const int gap = juce::jmax (3, rowH / 5);
		constexpr int kBodyInsetX = 5;
		const int contentW = vpW;
		const int scrollReserve = viewport->getScrollBarThickness() + 8;
		const int contentLeft = kBodyInsetX;
		const int contentRight = juce::jmax (contentLeft + 40, contentW - kBodyInsetX - scrollReserve);
		const int innerW = juce::jmax (40, contentRight - contentLeft);
		const auto& font = ratioTe->getFont();
		const int spaceW = juce::jmax (2, stringWidth (font, " "));

		const int barX = contentLeft;
		const int barW = innerW;
		const auto scValueFont = scHpTe->getFont();
		const juce::Font scSideFont (juce::FontOptions (24.0f).withStyle ("Bold"));
		const int scRowH = scHpTe->getHeight();
		const int scBarH = juce::jmax (10, scRowH / 2);
		const int scBarGap = juce::jmax (2, scRowH / 6);
		const int scRowTotal = scRowH + scBarGap + scBarH;
		const int threshEditorW = juce::jlimit (24, 160, juce::jmax (stringWidth (font, juce::String (SATTRAudioProcessor::kExpThreshMin, 1)),
		                                                             stringWidth (font, juce::String (SATTRAudioProcessor::kExpThreshMax, 1))) + 16);
		const int ratioEditorW = juce::jlimit (36, 180, juce::jmax (stringWidth (font, formatExpRatioDisplay (SATTRAudioProcessor::kExpRatioMin)),
		                                                            stringWidth (font, formatExpRatioDisplay (SATTRAudioProcessor::kExpRatioMax))) + 24);
		const int kneeEditorW = juce::jlimit (24, 160, stringWidth (font, juce::String (SATTRAudioProcessor::kExpKneeMax, 1)) + 16);
		const int atkEditorW = juce::jlimit (24, 160, juce::jmax (stringWidth (font, juce::String (SATTRAudioProcessor::kExpAtkMin, 2)),
		                                                          stringWidth (font, juce::String (SATTRAudioProcessor::kExpAtkMax, 2))) + 16);
		const int relEditorW = juce::jlimit (24, 160, juce::jmax (stringWidth (font, juce::String (SATTRAudioProcessor::kExpRelMin, 2)),
		                                                          stringWidth (font, juce::String (SATTRAudioProcessor::kExpRelMax, 2))) + 16);
		const int scGainEditorW = juce::jlimit (24, 160, juce::jmax (stringWidth (font, juce::String (SATTRAudioProcessor::kExpScGainMin, 1)),
		                                                             stringWidth (font, juce::String (SATTRAudioProcessor::kExpScGainMax, 1))) + 16);
		const int labelGap = juce::jmax (spaceW, 12);
		const int unitGapPx = juce::jmax (spaceW, 12);

		int y = 0;

		{
			const int legendW = stringWidth (font, "ORDER:") + 4;
			const int valueW = stringWidth (font, "POST") + 8;
			const int pairW = legendW + spaceW + valueW;
			const int pairX = contentLeft + juce::jmax (0, (innerW - pairW) / 2);
			orderLegend->setBounds (pairX, y, legendW, rowH);
			orderLabel->setBounds (pairX + legendW + spaceW, y, valueW, rowH);
		}
		y += rowH + gap;

		auto placeRow = [&] (juce::TextEditor* te, juce::Label* suffix,
		                     juce::Label* unitLabel, PromptBar* bar, int rowY, int editorW)
		{
			if (! te || ! suffix || ! bar) return;

			const int textW = juce::jmax (1, stringWidth (font, te->getText()));
			editorW = juce::jlimit (24, editorW, textW + 16);
			const int labelW = stringWidth (suffix->getFont(), suffix->getText()) + 2;
			const int unitW = (unitLabel != nullptr) ? stringWidth (font, unitLabel->getText()) + 2 : 0;
			const bool isRatioRow = (te == ratioTe && unitLabel == ratioUnit);

			if (isRatioRow)
			{
				const int ratioGap = juce::jmax (2, spaceW / 2);
				const int groupW = labelW + ratioGap + unitW + ratioGap + editorW;
				const int blockLeft = contentLeft + juce::jmax (0, (innerW - groupW) / 2);
				const int colonX = blockLeft + labelW + ratioGap;
				const int teX = colonX + unitW + ratioGap;

				suffix->setBounds (blockLeft, rowY, labelW, rowH);

				if (unitLabel != nullptr)
				{
					unitLabel->setJustificationType (juce::Justification::centred);
					unitLabel->setBounds (colonX, rowY, unitW, rowH);
				}

				te->setBounds (teX, rowY, editorW, rowH);
			}
			else
			{
				const int maxFittedEditorW = juce::jmax (24, innerW - labelW - labelGap - (unitLabel != nullptr ? unitGapPx + unitW : 0));
				editorW = juce::jmin (editorW, maxFittedEditorW);
				const int groupW = labelW + labelGap + editorW + (unitLabel != nullptr ? unitGapPx + unitW : 0);
				const int blockLeft = contentLeft + juce::jmax (0, (innerW - groupW) / 2);

				suffix->setBounds (blockLeft, rowY, labelW, rowH);
				const int teX = blockLeft + labelW + labelGap;
				te->setBounds (teX, rowY, editorW, rowH);

				if (unitLabel != nullptr)
				{
					unitLabel->setJustificationType (juce::Justification::centredLeft);
					unitLabel->setBounds (teX + editorW + unitGapPx, rowY, unitW, rowH);
				}
			}

			bar->setBounds (barX, rowY + rowH + barGap, barW, barH);
		};

		auto placeScFilterRow = [&] (juce::ToggleButton* toggle,
		                             juce::Label* name,
		                             juce::TextEditor* te,
		                             juce::Label* unit,
		                             juce::Label* slope,
		                             PromptBar* bar,
		                             int rowY) -> int
		{
			constexpr int toggleSide = 26;
			constexpr int toggleVisualInsetLeft = 2;
			constexpr int tglGap = 4;
			const int toggleVisualSide = juce::jlimit (14,
			                                           juce::jmax (14, toggleSide - 2),
			                                           (int) std::lround ((double) toggleSide * 0.65));
			const int labelOffset = toggleVisualInsetLeft + toggleVisualSide + tglGap;

			name->setFont (scSideFont);
			unit->setFont (scSideFont);
			slope->setFont (scSideFont);

			toggle->setBounds (barX, rowY + (scRowH - toggleSide) / 2, toggleSide, toggleSide);

			const int nameX = barX + labelOffset;
			const int nameW = stringWidth (scSideFont, "LP") + 2;
			name->setBounds (nameX, rowY, nameW, scRowH);

			const int slopeW = stringWidth (scSideFont, "24dB") + 4;
			const int slopeX = contentRight - slopeW;
			slope->setBounds (slopeX, rowY, slopeW, scRowH);

			const int kNameToValueGap = juce::jmax (spaceW, 4);
			const int midL = nameX + nameW + kNameToValueGap;
			const int kSlopeGap = juce::jmax (spaceW, 4);
			const int midR = slopeX - kSlopeGap;
			const int midW = midR - midL;

			const int unitW = stringWidth (scSideFont, "Hz") + 2;
			const int textW = juce::jmax (1, stringWidth (scValueFont, te->getText()));
			constexpr int kEditorPad = 2;
			constexpr int hzGap = 1;
			const int desiredEditorW = textW + kEditorPad * 2;
			const int editorW = desiredEditorW;
			const int groupW = editorW + hzGap + unitW;
			const int groupX = juce::jmin (midL + juce::jmax (0, (midW - groupW) / 2),
			                               juce::jmax (midL, midR - groupW));

			te->setBounds (groupX, rowY, editorW, scRowH);
			unit->setBounds (groupX + editorW + hzGap, rowY, unitW, scRowH);
			bar->setBounds (barX, rowY + scRowH + scBarGap, barW, scBarH);

			return scRowTotal;
		};

		placeRow (threshTe, threshSuffix, threshUnit, threshBar, y, threshEditorW);
		y += rowTotal + gap;
		placeRow (ratioTe, ratioSuffix, ratioUnit, ratioBar, y, ratioEditorW);
		y += rowTotal + gap;
		placeRow (kneeTe, kneeSuffix, kneeUnit, kneeBar, y, kneeEditorW);
		y += rowTotal + gap;
		placeRow (atkTe, atkSuffix, atkUnit, atkBar, y, atkEditorW);
		y += rowTotal + gap;
		placeRow (relTe, relSuffix, relUnit, relBar, y, relEditorW);
		y += rowTotal + juce::jmax (gap, rowH / 2);

		sidechainLabel->setBounds (contentLeft, y, innerW, rowH);
		y += rowH + gap;

		placeRow (scGainTe, scGainSuffix, scGainUnit, scGainBar, y, scGainEditorW);
		y += rowTotal + gap;

		scFilterLabel->setBounds (contentLeft, y, innerW, rowH);
		y += rowH + gap;

		y += placeScFilterRow (scHpToggle, scHpNameLabel, scHpTe, scHpUnitLabel, scHpSlopeLabel, scHpBar, y);
		y += gap;
		y += placeScFilterRow (scLpToggle, scLpNameLabel, scLpTe, scLpUnitLabel, scLpSlopeLabel, scLpBar, y);
		y += juce::jmax (gap * 2, rowH / 2);

		bodyContent->setSize (contentW, y);
	};

	auto textToBarRatio = [syncing, ratioApvts] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		const float internalRatio = expRatioDisplayToInternal (raw);
		bar->value = expRatioDisplayToNorm (internalRatio);
		if (ratioApvts) ratioApvts->setValueNotifyingHost (ratioApvts->convertTo0to1 (internalRatio));
		bar->repaint();
	};

	auto textToBarThresh = [syncing, threshApvts] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = (raw - SATTRAudioProcessor::kExpThreshMin) / (SATTRAudioProcessor::kExpThreshMax - SATTRAudioProcessor::kExpThreshMin);
		if (threshApvts) threshApvts->setValueNotifyingHost (threshApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarKnee = [syncing, kneeApvts] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = (raw - SATTRAudioProcessor::kExpKneeMin) / (SATTRAudioProcessor::kExpKneeMax - SATTRAudioProcessor::kExpKneeMin);
		if (kneeApvts) kneeApvts->setValueNotifyingHost (kneeApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarAtk = [syncing, atkApvts, atkNormRange] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = atkNormRange.convertTo0to1 (raw);
		if (atkApvts) atkApvts->setValueNotifyingHost (atkApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarRel = [syncing, relApvts, relNormRange] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = relNormRange.convertTo0to1 (raw);
		if (relApvts) relApvts->setValueNotifyingHost (relApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarScGain = [syncing, scGainApvts, scGainNormRange] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = scGainNormRange.convertTo0to1 (raw);
		if (scGainApvts) scGainApvts->setValueNotifyingHost (scGainApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarScHp = [syncing, scHpApvts, freqToNorm] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = freqToNorm (raw);
		if (scHpApvts) scHpApvts->setValueNotifyingHost (scHpApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto textToBarScLp = [syncing, scLpApvts, freqToNorm] (float raw, PromptBar* bar)
	{
		if (*syncing || ! bar) return;
		bar->value = freqToNorm (raw);
		if (scLpApvts) scLpApvts->setValueNotifyingHost (scLpApvts->convertTo0to1 (raw));
		bar->repaint();
	};

	auto handleEnvPromptText = [syncing, layoutBody, sanitiseEnvPromptText]
		(juce::TextEditor* te, PromptBar* bar, const EnvPromptNumericSpec& spec, auto&& pushValue) mutable
	{
		if (*syncing || te == nullptr || bar == nullptr)
			return;

		const auto original = te->getText();
		bool incomplete = false;
		const auto sanitised = sanitiseEnvPromptText (original, spec, incomplete);
		if (sanitised != original)
		{
			*syncing = true;
			te->setText (sanitised, juce::dontSendNotification);
			*syncing = false;
		}

		auto parseText = sanitised;
		if (parseText.endsWithChar ('.'))
			parseText = parseText.dropLastCharacters (1);

		if (! incomplete && parseText.isNotEmpty() && parseText != "-")
		{
			const float raw = (float) parseText.getDoubleValue();
			if (raw >= spec.minValue && raw <= spec.maxValue)
				pushValue (raw, bar);
		}

		layoutBody();
	};

	auto handleScFreqText = [syncing, layoutBody, sanitiseEnvPromptText, parseEnvPromptValue,
	                         scHpSpec, scLpSpec, textToBarScHp, textToBarScLp,
	                         aw, scHpBar, scLpBar] (bool isHp) mutable
	{
		auto* te = aw->getTextEditor (isHp ? "scHp" : "scLp");
		auto* otherTe = aw->getTextEditor (isHp ? "scLp" : "scHp");
		auto* bar = isHp ? scHpBar : scLpBar;
		const auto& spec = isHp ? scHpSpec : scLpSpec;
		if (*syncing || te == nullptr || bar == nullptr)
			return;

		const auto original = te->getText();
		bool incomplete = false;
		const auto sanitised = sanitiseEnvPromptText (original, spec, incomplete);
		if (sanitised != original)
		{
			*syncing = true;
			te->setText (sanitised, juce::dontSendNotification);
			*syncing = false;
		}

		auto parseText = sanitised;
		if (parseText.endsWithChar ('.'))
			parseText = parseText.dropLastCharacters (1);

		if (! incomplete && parseText.isNotEmpty())
		{
			float raw = (float) juce::roundToInt (juce::jlimit (spec.minValue, spec.maxValue, (float) parseText.getDoubleValue()));
			const float other = parseEnvPromptValue (otherTe, isHp ? scLpSpec : scHpSpec,
			                                        isHp ? SATTRAudioProcessor::kExpScLpDefault
			                                             : SATTRAudioProcessor::kExpScHpDefault);
			raw = isHp ? juce::jmin (raw, (float) juce::roundToInt (other)) : juce::jmax (raw, (float) juce::roundToInt (other));
			te->setText (juce::String (juce::roundToInt (raw)), juce::dontSendNotification);
			if (isHp)
				textToBarScHp (raw, bar);
			else
				textToBarScLp (raw, bar);
		}

		layoutBody();
	};

	if (auto* te = aw->getTextEditor ("thresh"))
		te->onTextChange = [te, threshBar, threshSpec, handleEnvPromptText, textToBarThresh] () mutable
		{ handleEnvPromptText (te, threshBar, threshSpec, textToBarThresh); };
	if (auto* te = aw->getTextEditor ("ratio"))
		te->onTextChange = [te, ratioBar, ratioSpec, handleEnvPromptText, textToBarRatio] () mutable
		{ handleEnvPromptText (te, ratioBar, ratioSpec, textToBarRatio); };
	if (auto* te = aw->getTextEditor ("knee"))
		te->onTextChange = [te, kneeBar, kneeSpec, handleEnvPromptText, textToBarKnee] () mutable
		{ handleEnvPromptText (te, kneeBar, kneeSpec, textToBarKnee); };
	if (auto* te = aw->getTextEditor ("atk"))
		te->onTextChange = [te, atkBar, atkSpec, handleEnvPromptText, textToBarAtk] () mutable
		{ handleEnvPromptText (te, atkBar, atkSpec, textToBarAtk); };
	if (auto* te = aw->getTextEditor ("rel"))
		te->onTextChange = [te, relBar, relSpec, handleEnvPromptText, textToBarRel] () mutable
		{ handleEnvPromptText (te, relBar, relSpec, textToBarRel); };
	if (auto* te = aw->getTextEditor ("scGain"))
		te->onTextChange = [te, scGainBar, scGainSpec, handleEnvPromptText, textToBarScGain] () mutable
		{ handleEnvPromptText (te, scGainBar, scGainSpec, textToBarScGain); };
	if (auto* te = aw->getTextEditor ("scHp"))
		te->onTextChange = [handleScFreqText] () mutable { handleScFreqText (true); };
	if (auto* te = aw->getTextEditor ("scLp"))
		te->onTextChange = [handleScFreqText] () mutable { handleScFreqText (false); };

	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);
	applyPromptShellSize (*aw);
	layoutBody();

	const auto& kExpFont = kBoldFont40();
	const juce::Font kExpFilterFont (juce::FontOptions (34.0f).withStyle ("Bold"));
	preparePromptTextEditor (*aw, "thresh", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "ratio", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "knee", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "atk", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "rel", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "scGain", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
	preparePromptTextEditor (*aw, "scHp", scheme.bg, scheme.text, scheme.fg, kExpFilterFont, false);
	preparePromptTextEditor (*aw, "scLp", scheme.bg, scheme.text, scheme.fg, kExpFilterFont, false);
	layoutBody();

	styleAlertButtons (*aw, lnf);

	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutBody] (juce::AlertWindow& a)
		{
			juce::ignoreUnused (a);
			layoutBody();
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
	}

	{
		preparePromptTextEditor (*aw, "thresh", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "ratio", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "knee", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "atk", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "rel", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "scGain", scheme.bg, scheme.text, scheme.fg, kExpFont, false);
		preparePromptTextEditor (*aw, "scHp", scheme.bg, scheme.text, scheme.fg, kExpFilterFont, false);
		preparePromptTextEditor (*aw, "scLp", scheme.bg, scheme.text, scheme.fg, kExpFilterFont, false);
		layoutBody();

		auto syncFonts = [&] (ResetLabel* suffix, juce::Label* unit, const char* edId)
		{
			if (suffix != nullptr)
				if (auto* te = aw->getTextEditor (edId))
				{
					suffix->setFont (te->getFont());
					if (unit != nullptr) unit->setFont (te->getFont());
				}
		};
		syncFonts (threshSuffix, threshUnit, "thresh");
		syncFonts (ratioSuffix, ratioUnit, "ratio");
		syncFonts (kneeSuffix, kneeUnit, "knee");
		syncFonts (atkSuffix, atkUnit, "atk");
		syncFonts (relSuffix, relUnit, "rel");
		syncFonts (scGainSuffix, scGainUnit, "scGain");
		for (auto* label : { scFilterLabel })
			if (label != nullptr)
				label->setFont (kExpFont);
		for (auto* label : { scHpNameLabel, scLpNameLabel, scHpSlopeLabel, scLpSlopeLabel })
			if (label != nullptr)
				label->setFont (juce::Font (juce::FontOptions (24.0f).withStyle ("Bold")));
		for (auto* label : { scHpUnitLabel, scLpUnitLabel })
			if (label != nullptr)
				label->setFont (kExpFilterFont);
		layoutBody();

		juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
		juce::MessageManager::callAsync ([safeAw]()
		{
			if (safeAw == nullptr) return;
			bringPromptWindowToFront (*safeAw);
			safeAw->repaint();
		});
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, orderState, threshBar, ratioBar, kneeBar, atkBar, relBar, scGainBar, scHpBar, scLpBar,
			 scHpToggle, scLpToggle, scHpSlopeState, scLpSlopeState,
			 parseEnvPromptValue, threshSpec, ratioSpec, kneeSpec, atkSpec, relSpec, scGainSpec, normToFreq,
			 viewport, orderHandler, scHpToggleHandler, scLpToggleHandler, scHpSlopeHandler, scLpSlopeHandler,
			 savedOrder = currentOrder, savedThresh = currentThresh, savedRatio = currentRatio, savedKnee = currentKnee,
			 savedAtk = currentAtk, savedRel = currentRel, savedScHp = currentScHp, savedScLp = currentScLp,
			 savedScHpOn = currentScHpOn, savedScLpOn = currentScLpOn,
			 savedScHpSlope = currentScHpSlope, savedScLpSlope = currentScLpSlope,
			 savedScGain = currentScGain,
			 orderParamId, threshParamId, ratioParamId, kneeParamId, atkParamId, relParamId,
			 scHpParamId, scLpParamId, scHpOnParamId, scLpOnParamId, scHpSlopeParamId, scLpSlopeParamId, scGainParamId,
			 loaderIndex] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);

			if (safeThis == nullptr)
			{
				delete viewport;
				delete orderHandler;
				delete scHpToggleHandler;
				delete scLpToggleHandler;
				delete scHpSlopeHandler;
				delete scLpSlopeHandler;
				return;
			}

			if (result != 1)
			{
				auto& vts = safeThis->audioProcessor.getValueTreeState();
				if (auto* p = vts.getParameter (orderParamId))
					p->setValueNotifyingHost (savedOrder ? 1.0f : 0.0f);
				if (auto* p = vts.getParameter (threshParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedThresh));
				if (auto* p = vts.getParameter (ratioParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedRatio));
				if (auto* p = vts.getParameter (kneeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedKnee));
				if (auto* p = vts.getParameter (atkParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedAtk));
				if (auto* p = vts.getParameter (relParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedRel));
				if (auto* p = vts.getParameter (scHpParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedScHp));
				if (auto* p = vts.getParameter (scLpParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedScLp));
				if (auto* p = vts.getParameter (scHpOnParamId))
					p->setValueNotifyingHost (savedScHpOn ? 1.0f : 0.0f);
				if (auto* p = vts.getParameter (scLpOnParamId))
					p->setValueNotifyingHost (savedScLpOn ? 1.0f : 0.0f);
				if (auto* p = vts.getParameter (scHpSlopeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 ((float) savedScHpSlope));
				if (auto* p = vts.getParameter (scLpSlopeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 ((float) savedScLpSlope));
				if (auto* p = vts.getParameter (scGainParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedScGain));
			}
			else
			{
				auto& vts = safeThis->audioProcessor.getValueTreeState();
				const float newThresh = parseEnvPromptValue (aw->getTextEditor ("thresh"), threshSpec, savedThresh);
				const float newRatio  = parseEnvPromptValue (aw->getTextEditor ("ratio"),  ratioSpec,  savedRatio);
				const float newKnee   = parseEnvPromptValue (aw->getTextEditor ("knee"),   kneeSpec,   savedKnee);
				const float newAtk    = parseEnvPromptValue (aw->getTextEditor ("atk"),    atkSpec,    savedAtk);
				const float newRel    = parseEnvPromptValue (aw->getTextEditor ("rel"),    relSpec,    savedRel);
				float newScHp         = (float) juce::roundToInt (normToFreq (scHpBar->value));
				float newScLp         = (float) juce::roundToInt (normToFreq (scLpBar->value));
				const bool newScHpOn  = scHpToggle->getToggleState();
				const bool newScLpOn  = scLpToggle->getToggleState();
				const int newScHpSlope = juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin,
				                                       SATTRAudioProcessor::kFilterSlopeMax,
				                                       *scHpSlopeState);
				const int newScLpSlope = juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin,
				                                       SATTRAudioProcessor::kFilterSlopeMax,
				                                       *scLpSlopeState);
				const float newScGain = parseEnvPromptValue (aw->getTextEditor ("scGain"), scGainSpec, savedScGain);
				if (newScHp > newScLp)
					std::swap (newScHp, newScLp);

				if (auto* p = vts.getParameter (threshParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newThresh));
				if (auto* p = vts.getParameter (ratioParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newRatio));
				if (auto* p = vts.getParameter (kneeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newKnee));
				if (auto* p = vts.getParameter (atkParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newAtk));
				if (auto* p = vts.getParameter (relParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newRel));
				if (auto* p = vts.getParameter (scHpParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newScHp));
				if (auto* p = vts.getParameter (scLpParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newScLp));
				if (auto* p = vts.getParameter (scHpOnParamId))
					p->setValueNotifyingHost (newScHpOn ? 1.0f : 0.0f);
				if (auto* p = vts.getParameter (scLpOnParamId))
					p->setValueNotifyingHost (newScLpOn ? 1.0f : 0.0f);
				if (auto* p = vts.getParameter (scHpSlopeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 ((float) newScHpSlope));
				if (auto* p = vts.getParameter (scLpSlopeParamId))
					p->setValueNotifyingHost (p->convertTo0to1 ((float) newScLpSlope));
				if (auto* p = vts.getParameter (scGainParamId))
					p->setValueNotifyingHost (p->convertTo0to1 (newScGain));

				auto& button = loaderIndex == 0 ? safeThis->expButtonA
				             : (loaderIndex == 1 ? safeThis->expButtonB : safeThis->expButtonC);
				button.setTooltip (formatExpTooltip (*orderState, newRatio));
			}

			delete viewport;
			delete orderHandler;
			delete scHpToggleHandler;
			delete scLpToggleHandler;
			delete scHpSlopeHandler;
			delete scLpSlopeHandler;
		}),
		false);
}

//==============================================================================
//  Info Prompt
//==============================================================================
void SATTRAudioProcessorEditor::openInfoPopup()
{
	lnf.setScheme (activeScheme);

	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);
	aw->setLookAndFeel (&lnf);
	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("GRAPHICS", 2);
	aw->setEscapeKeyCancels (true);

	applyPromptShellSize (*aw);

	// Body content: parsed from InfoContent.h XML
	auto* bodyContent = new juce::Component();
	bodyContent->setComponentID ("bodyContent");

	auto infoFont = lnf.getAlertWindowMessageFont();
	infoFont.setHeight (infoFont.getHeight() * 1.45f);

	auto headingFont = infoFont;
	headingFont.setBold (true);
	headingFont.setHeight (infoFont.getHeight() * 1.25f);

	auto linkFont = infoFont;
	linkFont.setHeight (infoFont.getHeight() * 1.08f);

	auto poemFont = infoFont;
	poemFont.setItalic (true);

	auto xmlDoc = juce::XmlDocument::parse (InfoContent::xml);
	auto* contentNode = xmlDoc != nullptr ? xmlDoc->getChildByName ("content") : nullptr;

	if (contentNode != nullptr)
	{
		int elemIdx = 0;
		for (auto* node : contentNode->getChildIterator())
		{
			const auto tag  = node->getTagName();
			const auto text = node->getAllSubText().trim();
			const auto id   = tag + juce::String (elemIdx++);

			if (tag == "heading")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (headingFont);
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "text" || tag == "separator")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (infoFont);
				l->setBorderSize (juce::BorderSize<int> (0));
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "link")
			{
				const auto url = node->getStringAttribute ("url");
				auto* lnk = new juce::HyperlinkButton (text, juce::URL (url));
				lnk->setComponentID (id);
				lnk->setJustificationType (juce::Justification::centred);
				lnk->setColour (juce::HyperlinkButton::textColourId, activeScheme.text);
				lnk->setFont (linkFont, false, juce::Justification::centred);
				lnk->setTooltip ("");
				bodyContent->addAndMakeVisible (lnk);
			}
			else if (tag == "poem")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (poemFont);
				l->setBorderSize (juce::BorderSize<int> (0, 0, 0, 0));
				l->getProperties().set ("poemPadFraction", 0.12f);
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "spacer")
			{
				auto* l = new juce::Label (id, "");
				l->setComponentID (id);
				l->setFont (infoFont);
				l->setBorderSize (juce::BorderSize<int> (0));
				bodyContent->addAndMakeVisible (l);
			}
		}
	}

	auto* viewport = new juce::Viewport();
	viewport->setComponentID ("bodyViewport");
	viewport->setViewedComponent (bodyContent, true);
	viewport->setScrollBarsShown (true, false);
	viewport->setScrollBarThickness (8);
	viewport->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (viewport);

	layoutInfoPopupContent (*aw);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [] (juce::AlertWindow& a)
		{
			layoutInfoPopupContent (a);
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	juce::MessageManager::callAsync ([safeAw, safeThis]()
	{
		if (safeAw == nullptr || safeThis == nullptr)
			return;

		bringPromptWindowToFront (*safeAw);
		safeAw->repaint();
	});

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<SATTRAudioProcessorEditor> (this), aw] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			if (result == 2)
			{
				safeThis->openGraphicsPopup();
				return;
			}

			safeThis->setPromptOverlayActive (false);
		}));
}

void SATTRAudioProcessorEditor::openGraphicsPopup()
{
	lnf.setScheme (activeScheme);

	useCustomPalette = audioProcessor.getUiUseCustomPalette();
	applyCrtState (audioProcessor.getUiFxTailEnabled());
	applyActivePalette();

	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);
	juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
	aw->setLookAndFeel (&lnf);
	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->setEscapeKeyCancels (true);

	auto labelFont = lnf.getAlertWindowMessageFont();
	labelFont.setHeight (labelFont.getHeight() * 1.20f);

	auto addPopupLabel = [this, aw] (const juce::String& id,
	                                 const juce::String& text,
	                                 juce::Font font,
	                                 juce::Justification justification = juce::Justification::centredLeft)
	{
		auto* label = new PopupClickableLabel (id, text);
		label->setComponentID (id);
		label->setJustificationType (justification);
		applyLabelTextColour (*label, activeScheme.text);
		label->setBorderSize (juce::BorderSize<int> (0));
		label->setFont (font);
		label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
		aw->addAndMakeVisible (label);
		return label;
	};

	auto* defaultToggle = new juce::ToggleButton ("");
	defaultToggle->setComponentID ("paletteDefaultToggle");
	aw->addAndMakeVisible (defaultToggle);

	auto* defaultLabel = addPopupLabel ("paletteDefaultLabel", "DFLT", labelFont);

	auto* customToggle = new juce::ToggleButton ("");
	customToggle->setComponentID ("paletteCustomToggle");
	aw->addAndMakeVisible (customToggle);

	auto* customLabel = addPopupLabel ("paletteCustomLabel", "CSTM", labelFont);

	auto paletteTitleFont = labelFont;
	paletteTitleFont.setHeight (paletteTitleFont.getHeight() * 1.30f);
	addPopupLabel ("paletteTitle", "PALETTE", paletteTitleFont, juce::Justification::centredLeft);

	for (int i = 0; i < 2; ++i)
	{
		auto* dflt = new juce::TextButton();
		dflt->setComponentID ("defaultSwatch" + juce::String (i));
		dflt->setTooltip ("Default palette colour " + juce::String (i + 1));
		aw->addAndMakeVisible (dflt);

		auto* custom = new PopupSwatchButton();
		custom->setComponentID ("customSwatch" + juce::String (i));
		custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
		aw->addAndMakeVisible (custom);
	}

	auto* fxToggle = new juce::ToggleButton ("");
	fxToggle->setComponentID ("fxToggle");
	fxToggle->setToggleState (crtEnabled, juce::dontSendNotification);
	fxToggle->onClick = [safeThis, fxToggle]()
	{
		if (safeThis == nullptr || fxToggle == nullptr)
			return;

		safeThis->applyCrtState (fxToggle->getToggleState());
		safeThis->audioProcessor.setUiFxTailEnabled (safeThis->crtEnabled);
		safeThis->repaint();
	};
	aw->addAndMakeVisible (fxToggle);

	auto* fxLabel = addPopupLabel ("fxLabel", "GRAPHIC FX", labelFont);

	auto syncAndRepaintPopup = [safeThis, safeAw]()
	{
		if (safeThis == nullptr || safeAw == nullptr)
			return;

		syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
		layoutGraphicsPopupContent (*safeAw);
		safeAw->repaint();
	};

	auto applyPaletteAndRepaint = [safeThis]()
	{
		if (safeThis == nullptr)
			return;

		safeThis->applyActivePalette();
		safeThis->repaint();
	};

	defaultToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
	{
		if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
			return;

		safeThis->useCustomPalette = false;
		safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
		defaultToggle->setToggleState (true, juce::dontSendNotification);
		customToggle->setToggleState (false, juce::dontSendNotification);
		applyPaletteAndRepaint();
		syncAndRepaintPopup();
	};

	customToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
	{
		if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
			return;

		safeThis->useCustomPalette = true;
		safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
		defaultToggle->setToggleState (false, juce::dontSendNotification);
		customToggle->setToggleState (true, juce::dontSendNotification);
		applyPaletteAndRepaint();
		syncAndRepaintPopup();
	};

	if (defaultLabel != nullptr && defaultToggle != nullptr)
		defaultLabel->onClick = [defaultToggle]() { defaultToggle->triggerClick(); };

	if (customLabel != nullptr && customToggle != nullptr)
		customLabel->onClick = [customToggle]() { customToggle->triggerClick(); };

	if (fxLabel != nullptr && fxToggle != nullptr)
		fxLabel->onClick = [fxToggle]() { fxToggle->triggerClick(); };

	for (int i = 0; i < 2; ++i)
	{
		if (auto* customSwatch = dynamic_cast<PopupSwatchButton*> (aw->findChildWithID ("customSwatch" + juce::String (i))))
		{
			customSwatch->onLeftClick = [safeThis, safeAw, i]()
			{
				if (safeThis == nullptr)
					return;

				auto& rng = juce::Random::getSystemRandom();
				const auto randomColour = juce::Colour::fromRGB ((juce::uint8) rng.nextInt (256),
				                                                 (juce::uint8) rng.nextInt (256),
				                                                 (juce::uint8) rng.nextInt (256));

				safeThis->customPalette[(size_t) i] = randomColour;
				safeThis->audioProcessor.setUiCustomPaletteColour (i, randomColour);
				if (safeThis->useCustomPalette)
				{
					safeThis->applyActivePalette();
					safeThis->repaint();
				}

				if (safeAw != nullptr)
				{
					syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
					layoutGraphicsPopupContent (*safeAw);
					safeAw->repaint();
				}
			};

			customSwatch->onRightClick = [safeThis, safeAw, i]()
			{
				if (safeThis == nullptr)
					return;

				const auto scheme = safeThis->activeScheme;

				auto* colorAw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
				colorAw->setLookAndFeel (&safeThis->lnf);
				colorAw->addTextEditor ("hex", colourToHexRgb (safeThis->customPalette[(size_t) i]), juce::String());

				if (auto* te = colorAw->getTextEditor ("hex"))
					te->setInputFilter (new HexInputFilter(), true);

				colorAw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
				colorAw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

				finalizePromptButtons (*colorAw, safeThis->lnf,
				                       [] (juce::AlertWindow& a) { applyPromptShellSize (a); });

				const juce::Font& kHexPromptFont = kBoldFont40();

				preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);

				if (safeThis != nullptr)
				{
					fitAlertWindowToEditor (*colorAw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
					{
						layoutAlertWindowButtons (a);
						preparePromptTextEditor (a, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);
					});

					embedAlertWindowInOverlay (safeThis.getComponent(), colorAw, true);
				}
				else
				{
					colorAw->centreAroundComponent (safeThis.getComponent(), colorAw->getWidth(), colorAw->getHeight());
					bringPromptWindowToFront (*colorAw);
					if (safeThis != nullptr && safeThis->tooltipWindow)
						safeThis->tooltipWindow->toFront (true);
					colorAw->repaint();
				}

				preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);

				juce::Component::SafePointer<juce::AlertWindow> safeColorAw (colorAw);
				juce::MessageManager::callAsync ([safeColorAw]()
				{
					if (safeColorAw == nullptr)
						return;
					bringPromptWindowToFront (*safeColorAw);
					safeColorAw->repaint();
				});

				colorAw->enterModalState (true,
					juce::ModalCallbackFunction::create ([safeThis, safeAw, colorAw, i] (int result) mutable
					{
						std::unique_ptr<juce::AlertWindow> killer (colorAw);
						if (safeThis == nullptr)
							return;

						if (result != 1)
							return;

						juce::Colour parsed;
						if (! tryParseHexColour (killer->getTextEditorContents ("hex"), parsed))
							return;

						safeThis->customPalette[(size_t) i] = parsed;
						safeThis->audioProcessor.setUiCustomPaletteColour (i, parsed);
						if (safeThis->useCustomPalette)
						{
							safeThis->applyActivePalette();
							safeThis->repaint();
						}

						if (safeAw != nullptr)
						{
							syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
							layoutGraphicsPopupContent (*safeAw);
							safeAw->repaint();
						}
					}));
			};
		}
	}

	applyPromptShellSize (*aw);
	syncGraphicsPopupState (*aw, defaultPalette, customPalette, useCustomPalette);
	layoutGraphicsPopupContent (*aw);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
		{
			syncGraphicsPopupState (a, defaultPalette, customPalette, useCustomPalette);
			layoutGraphicsPopupContent (a);
		});
	}
	if (safeThis != nullptr)
	{
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);

		juce::MessageManager::callAsync ([safeAw, safeThis]()
		{
			if (safeAw == nullptr || safeThis == nullptr)
				return;

			safeAw->toFront (false);
			safeAw->repaint();
		});
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis, aw] (int) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);
			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);
		}));
}

void SATTRAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
	label.setColour (juce::Label::textColourId, colour);
}

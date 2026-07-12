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
static constexpr int kMeterTimerHz = 24;
static constexpr int kIdleTimerHz  = 4;
static constexpr float kSilenceDb  = -80.0f;

// ----------------------------------------------------------------
//  Parameter listener IDs
// ----------------------------------------------------------------
static constexpr std::array<const char*, 13> kUiMirrorParamIds {
	SATTRAudioProcessor::kParamUiPalette,
	SATTRAudioProcessor::kParamUiFxTail,
	SATTRAudioProcessor::kParamUiIoFx,
	SATTRAudioProcessor::kParamUiColor0,
	SATTRAudioProcessor::kParamUiColor1,
	SATTRAudioProcessor::kParamUiColor2,
	SATTRAudioProcessor::kParamUiColor3,
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

	juce::String formatSidechainFilterText (bool on, float hz, int slope)
	{
		if (! on)
			return "OFF";
		const float clamped = juce::jlimit (SATTRAudioProcessor::kSidechainFilterFreqMin,
		                                    SATTRAudioProcessor::kSidechainFilterFreqMax, hz);
		juce::String freq;
		if (clamped >= 10000.0f)
			freq = juce::String (juce::roundToInt (clamped / 1000.0f)) + "k";
		else if (clamped >= 1000.0f)
			freq = juce::String (clamped / 1000.0f, 1) + "k";
		else
			freq = juce::String (juce::roundToInt (clamped));
		return freq + "@" + juce::String ((juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin,
		                                                 SATTRAudioProcessor::kFilterSlopeMax, slope) + 1) * 6);
	}

	juce::String formatSidechainTooltip (float gainDb, float smooth,
	                                     bool hpOn, float hp, int hpSlope,
	                                     bool lpOn, float lp, int lpSlope)
	{
		return "GAIN " + formatGainFaderDb (gainDb)
		     + " | SMOOTH " + juce::String (juce::roundToInt (
		            juce::jlimit (SATTRAudioProcessor::kSidechainSmoothMin,
		                          SATTRAudioProcessor::kSidechainSmoothMax,
		                          smooth) * 100.0f)) + "%"
		     + " | HP " + formatSidechainFilterText (hpOn, hp, hpSlope)
		     + " | LP " + formatSidechainFilterText (lpOn, lp, lpSlope);
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

	juce::String formatSatOffsetMsNumberForUi (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);

		if (safeMs >= 1000.0)
			return juce::String (safeMs / 1000.0, 2);
		if (safeMs >= 100.0)
			return juce::String (safeMs, 1);
		return juce::String (safeMs, 2);
	}

	juce::String formatSatOffsetMsForUi (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);
		return formatSatOffsetMsNumberForUi (safeMs) + (safeMs >= 1000.0 ? "s" : "ms");
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

	juce::String formatExpTimeMsForPromptValue (double ms)
	{
		const double safeMs = juce::jmax (0.0, ms);

		if (safeMs >= 1000.0)
			return juce::String (juce::roundToInt (safeMs));
		if (safeMs >= 100.0)
			return juce::String (safeMs, 1);
		if (safeMs >= 1.0)
			return juce::String (safeMs, 2);
		return juce::String (safeMs, 3);
	}

	constexpr int kFooterMixValueWidthPx = TR::kLoaderFooterMixValueWidthPx;
	constexpr int kFooterDbValueWidthPx = TR::kLoaderFooterDbValueWidthPx;
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
		return TR::makeLoaderFooterValueArea (barBounds, valueWidthPx);
	}

	juce::Rectangle<int> makeExpandedFooterValueArea (int panelRight, const juce::Rectangle<int>& barBounds)
	{
		return TR::makeExpandedLoaderFooterValueArea (panelRight, barBounds);
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
			case 5: return 6;  // OVERDRIVE A
			case 8: return 7;  // OVERDRIVE B
			case 6: return 8;  // CLIPPER
			case 7: return 9;  // NAM
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
			case 6: return 5; // OVERDRIVE A
			case 7: return 8; // OVERDRIVE B
			case 8: return 6; // CLIPPER
			case 9: return 7; // NAM
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


}

// ----------------------------------------------------------------
//  Popup static layout helpers
// ----------------------------------------------------------------
// Static loader param-ID table
// ----------------------------------------------------------------
// BarSlider type constants (shared template uses int)
namespace BarSliderType { constexpr int Unknown=0,HpFreq=1,LpFreq=2,Input=3,Output=4,Tilt=5,Series=6,Pan=7,Fred=8,Pos=9,Mix=10,GlobalMix=11,GlobalOutput=12,LimThreshold=13,SatDrive=14,NamSlim=15,SatChar=16,SatTypeCtrl=17,SatBias=18,SatSag=19,Detail=20,Instability=21,Offset=22; }

const SATTRAudioProcessorEditor::LoaderParamIds SATTRAudioProcessorEditor::kLoaderParams[3] =
{
	{ // A
		SATTRAudioProcessor::kParamEnableA,
		SATTRAudioProcessor::kParamHpFreqA, SATTRAudioProcessor::kParamLpFreqA, SATTRAudioProcessor::kParamInA, SATTRAudioProcessor::kParamOutA, SATTRAudioProcessor::kParamTiltA,
		SATTRAudioProcessor::kParamSeriesA, SATTRAudioProcessor::kParamPanA,    SATTRAudioProcessor::kParamFredA, SATTRAudioProcessor::kParamPosA, SATTRAudioProcessor::kParamResoA,
		SATTRAudioProcessor::kParamInvA,    SATTRAudioProcessor::kParamChaosA, SATTRAudioProcessor::kParamChaosFilterA, SATTRAudioProcessor::kParamSidechainA,
		SATTRAudioProcessor::kParamSidechainGainA, SATTRAudioProcessor::kParamSidechainSmoothA,
		SATTRAudioProcessor::kParamSidechainHpA, SATTRAudioProcessor::kParamSidechainLpA, SATTRAudioProcessor::kParamSidechainHpOnA, SATTRAudioProcessor::kParamSidechainLpOnA,
		SATTRAudioProcessor::kParamSidechainHpSlopeA, SATTRAudioProcessor::kParamSidechainLpSlopeA,
		SATTRAudioProcessor::kParamChaosAmtA, SATTRAudioProcessor::kParamChaosSpdA,
		SATTRAudioProcessor::kParamChaosAmtFilterA, SATTRAudioProcessor::kParamChaosSpdFilterA,
		SATTRAudioProcessor::kParamModeInA, SATTRAudioProcessor::kParamModeOutA, SATTRAudioProcessor::kParamSumBusA, SATTRAudioProcessor::kParamFilterPosA, SATTRAudioProcessor::kParamMixA,
		SATTRAudioProcessor::kParamSatTypeA, SATTRAudioProcessor::kParamSatRawA, SATTRAudioProcessor::kParamSatDriveA, SATTRAudioProcessor::kParamNamSlimA, SATTRAudioProcessor::kParamSatCharA,
		SATTRAudioProcessor::kParamSatTypeCtrlA, SATTRAudioProcessor::kParamSatBiasA, SATTRAudioProcessor::kParamSatSagA,
		SATTRAudioProcessor::kParamDetailA,
		SATTRAudioProcessor::kParamInstabilityA,
		SATTRAudioProcessor::kParamOffsetA,
		SATTRAudioProcessor::kParamExpA
	},
	{ // B
		SATTRAudioProcessor::kParamEnableB,
		SATTRAudioProcessor::kParamHpFreqB, SATTRAudioProcessor::kParamLpFreqB, SATTRAudioProcessor::kParamInB, SATTRAudioProcessor::kParamOutB, SATTRAudioProcessor::kParamTiltB,
		SATTRAudioProcessor::kParamSeriesB, SATTRAudioProcessor::kParamPanB,    SATTRAudioProcessor::kParamFredB, SATTRAudioProcessor::kParamPosB, SATTRAudioProcessor::kParamResoB,
		SATTRAudioProcessor::kParamInvB,    SATTRAudioProcessor::kParamChaosB, SATTRAudioProcessor::kParamChaosFilterB, SATTRAudioProcessor::kParamSidechainB,
		SATTRAudioProcessor::kParamSidechainGainB, SATTRAudioProcessor::kParamSidechainSmoothB,
		SATTRAudioProcessor::kParamSidechainHpB, SATTRAudioProcessor::kParamSidechainLpB, SATTRAudioProcessor::kParamSidechainHpOnB, SATTRAudioProcessor::kParamSidechainLpOnB,
		SATTRAudioProcessor::kParamSidechainHpSlopeB, SATTRAudioProcessor::kParamSidechainLpSlopeB,
		SATTRAudioProcessor::kParamChaosAmtB, SATTRAudioProcessor::kParamChaosSpdB,
		SATTRAudioProcessor::kParamChaosAmtFilterB, SATTRAudioProcessor::kParamChaosSpdFilterB,
		SATTRAudioProcessor::kParamModeInB, SATTRAudioProcessor::kParamModeOutB, SATTRAudioProcessor::kParamSumBusB, SATTRAudioProcessor::kParamFilterPosB, SATTRAudioProcessor::kParamMixB,
		SATTRAudioProcessor::kParamSatTypeB, SATTRAudioProcessor::kParamSatRawB, SATTRAudioProcessor::kParamSatDriveB, SATTRAudioProcessor::kParamNamSlimB, SATTRAudioProcessor::kParamSatCharB,
		SATTRAudioProcessor::kParamSatTypeCtrlB, SATTRAudioProcessor::kParamSatBiasB, SATTRAudioProcessor::kParamSatSagB,
		SATTRAudioProcessor::kParamDetailB,
		SATTRAudioProcessor::kParamInstabilityB,
		SATTRAudioProcessor::kParamOffsetB,
		SATTRAudioProcessor::kParamExpB
	},
	{ // C
		SATTRAudioProcessor::kParamEnableC,
		SATTRAudioProcessor::kParamHpFreqC, SATTRAudioProcessor::kParamLpFreqC, SATTRAudioProcessor::kParamInC, SATTRAudioProcessor::kParamOutC, SATTRAudioProcessor::kParamTiltC,
		SATTRAudioProcessor::kParamSeriesC, SATTRAudioProcessor::kParamPanC,    SATTRAudioProcessor::kParamFredC, SATTRAudioProcessor::kParamPosC, SATTRAudioProcessor::kParamResoC,
		SATTRAudioProcessor::kParamInvC,    SATTRAudioProcessor::kParamChaosC, SATTRAudioProcessor::kParamChaosFilterC, SATTRAudioProcessor::kParamSidechainC,
		SATTRAudioProcessor::kParamSidechainGainC, SATTRAudioProcessor::kParamSidechainSmoothC,
		SATTRAudioProcessor::kParamSidechainHpC, SATTRAudioProcessor::kParamSidechainLpC, SATTRAudioProcessor::kParamSidechainHpOnC, SATTRAudioProcessor::kParamSidechainLpOnC,
		SATTRAudioProcessor::kParamSidechainHpSlopeC, SATTRAudioProcessor::kParamSidechainLpSlopeC,
		SATTRAudioProcessor::kParamChaosAmtC, SATTRAudioProcessor::kParamChaosSpdC,
		SATTRAudioProcessor::kParamChaosAmtFilterC, SATTRAudioProcessor::kParamChaosSpdFilterC,
		SATTRAudioProcessor::kParamModeInC, SATTRAudioProcessor::kParamModeOutC, SATTRAudioProcessor::kParamSumBusC, SATTRAudioProcessor::kParamFilterPosC, SATTRAudioProcessor::kParamMixC,
		SATTRAudioProcessor::kParamSatTypeC, SATTRAudioProcessor::kParamSatRawC, SATTRAudioProcessor::kParamSatDriveC, SATTRAudioProcessor::kParamNamSlimC, SATTRAudioProcessor::kParamSatCharC,
		SATTRAudioProcessor::kParamSatTypeCtrlC, SATTRAudioProcessor::kParamSatBiasC, SATTRAudioProcessor::kParamSatSagC,
		SATTRAudioProcessor::kParamDetailC,
		SATTRAudioProcessor::kParamInstabilityC,
		SATTRAudioProcessor::kParamOffsetC,
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
		                 invButtonB, chaosButtonB, chaosFilterButtonB, sidechainButtonB, chaosDisplayB, chaosFilterDisplayB, sidechainDisplayB,
		                 expButtonB, expDisplayB,
		                 modeInComboB, modeOutComboB, sumBusComboB, filterPosComboB, filterBarB_, mixSliderB,
		                 satTypeComboB, rawButtonB, namBrowseButtonB, namFileDisplayB,
		                 satDriveSliderB, namSlimSliderB, satCharSliderB, satTypeCtrlSliderB, satBiasSliderB, satSagSliderB, detailSliderB, instabilitySliderB, offsetSliderB };
		case 2: return { enableButtonC,
		                 hpFreqSliderC, lpFreqSliderC, inSliderC, outSliderC, tiltSliderC, seriesSliderC, panSliderC, fredSliderC, posSliderC,
		                 invButtonC, chaosButtonC, chaosFilterButtonC, sidechainButtonC, chaosDisplayC, chaosFilterDisplayC, sidechainDisplayC,
		                 expButtonC, expDisplayC,
		                 modeInComboC, modeOutComboC, sumBusComboC, filterPosComboC, filterBarC_, mixSliderC,
		                 satTypeComboC, rawButtonC, namBrowseButtonC, namFileDisplayC,
		                 satDriveSliderC, namSlimSliderC, satCharSliderC, satTypeCtrlSliderC, satBiasSliderC, satSagSliderC, detailSliderC, instabilitySliderC, offsetSliderC };
		default: return { enableButtonA,
		                  hpFreqSliderA, lpFreqSliderA, inSliderA, outSliderA, tiltSliderA, seriesSliderA, panSliderA, fredSliderA, posSliderA,
		                  invButtonA, chaosButtonA, chaosFilterButtonA, sidechainButtonA, chaosDisplayA, chaosFilterDisplayA, sidechainDisplayA,
		                  expButtonA, expDisplayA,
		                  modeInComboA, modeOutComboA, sumBusComboA, filterPosComboA, filterBarA_, mixSliderA,
		                  satTypeComboA, rawButtonA, namBrowseButtonA, namFileDisplayA,
		                  satDriveSliderA, namSlimSliderA, satCharSliderA, satTypeCtrlSliderA, satBiasSliderA, satSagSliderA, detailSliderA, instabilitySliderA, offsetSliderA };
	}
}

TR::LoaderPanelSpec SATTRAudioProcessorEditor::describeLoaderPanelSpec (int index)
{
	auto r = getLoaderRefs (index);
	auto& clear = index == 1 ? namClearButtonB : (index == 2 ? namClearButtonC : namClearButtonA);

	TR::LoaderPanelSpec spec;
	spec.kind = TR::LoaderPanelKind::Sat;
	spec.loaderIndex = juce::jlimit (0, 2, index);

	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::header, "sat-main-header",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::enable, r.enableBtn, "enable"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::type, r.satType, "satType"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::raw, r.raw, "raw"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::clear, clear, "namClear"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::fileBrowse, r.namBrowse, "namBrowse"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::fileDisplay, r.namDisplay, "namDisplay") }));

	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::sliderSingle, "sat-drive",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::drive, r.satDrive, "drive"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::namSlim, r.namSlim, "namSlim") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::sliderSingle, "sat-character",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::character, r.satChar, "character") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::sliderSingle, "sat-type-bias-sag",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::topology, r.satTypeCtrl, "type"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::bias, r.satBias, "bias"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::sag, r.satSag, "sag") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::sliderSingle, "sat-series-detail-instability-offset",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::series, r.series, "series"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::detail, r.detail, "detail"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::instability, r.instability, "instability"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::offset, r.offset, "offset") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::collapsedMain, TR::LoaderPanelRowKind::toggleRow, "sat-main-toggles",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::inv, r.inv, "inv"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::exp, r.exp, "exp"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::expLabel, r.expDisp, "expLabel") }));

	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::expandedIo, TR::LoaderPanelRowKind::header, "sat-io-header",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::type, r.satType, "satType"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::raw, r.raw, "raw"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::clear, clear, "namClear"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::fileBrowse, r.namBrowse, "namBrowse"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::fileDisplay, r.namDisplay, "namDisplay") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::expandedIo, TR::LoaderPanelRowKind::sliderSingle, "sat-io-gain-filter-pan-mix",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::in, r.in, "in"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::out, r.out, "out"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::tilt, r.tilt, "tilt"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::filter, r.filterBar, "filter"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::pan, r.pan, "pan"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::mix, r.mix, "mix") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::expandedIo, TR::LoaderPanelRowKind::comboGrid, "sat-io-routing",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::modeIn, r.modeIn, "modeIn"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::modeOut, r.modeOut, "modeOut"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::filterPos, r.filterPos, "filterPos"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::sumBus, r.sumBus, "sumBus") }));
	spec.rows.push_back (TR::loaderPanelRow (TR::LoaderPanelView::expandedIo, TR::LoaderPanelRowKind::toggleRow, "sat-io-toggles",
		{ TR::loaderPanelComponent (TR::LoaderPanelComponentRole::chaosFilter, r.chaosFilter, "chaosFilter"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::chaos, r.chaos, "chaos"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::sidechain, r.sidechain, "sidechain"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::chaosFilterLabel, r.chaosFilterDisp, "chaosFilterLabel"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::chaosLabel, r.chaosDisp, "chaosLabel"),
		  TR::loaderPanelComponent (TR::LoaderPanelComponentRole::sidechainLabel, r.sidechainDisp, "sidechainLabel") }));

	return spec;
}

SATTRAudioProcessorEditor::AttachRefs SATTRAudioProcessorEditor::getAttachRefs (int i)
{
	switch (i)
	{
		case 1: return { enableAttachB,
		                 hpFreqAttachB, lpFreqAttachB, inAttachB, outAttachB, tiltAttachB, seriesAttachB, panAttachB, fredAttachB, posAttachB,
		                 invAttachB, chaosAttachB, chaosFilterAttachB, sidechainAttachB, expAttachB,
		                 modeInAttachB, modeOutAttachB, sumBusAttachB, filterPosAttachB, mixAttachB,
		                 satTypeAttachB, rawAttachB, satDriveAttachB, namSlimAttachB, satCharAttachB, satTypeCtrlAttachB, satBiasAttachB, satSagAttachB, detailAttachB, instabilityAttachB, offsetAttachB };
		case 2: return { enableAttachC,
		                 hpFreqAttachC, lpFreqAttachC, inAttachC, outAttachC, tiltAttachC, seriesAttachC, panAttachC, fredAttachC, posAttachC,
		                 invAttachC, chaosAttachC, chaosFilterAttachC, sidechainAttachC, expAttachC,
		                 modeInAttachC, modeOutAttachC, sumBusAttachC, filterPosAttachC, mixAttachC,
		                 satTypeAttachC, rawAttachC, satDriveAttachC, namSlimAttachC, satCharAttachC, satTypeCtrlAttachC, satBiasAttachC, satSagAttachC, detailAttachC, instabilityAttachC, offsetAttachC };
		default: return { enableAttachA,
		                  hpFreqAttachA, lpFreqAttachA, inAttachA, outAttachA, tiltAttachA, seriesAttachA, panAttachA, fredAttachA, posAttachA,
		                  invAttachA, chaosAttachA, chaosFilterAttachA, sidechainAttachA, expAttachA,
		                  modeInAttachA, modeOutAttachA, sumBusAttachA, filterPosAttachA, mixAttachA,
		                  satTypeAttachA, rawAttachA, satDriveAttachA, namSlimAttachA, satCharAttachA, satTypeCtrlAttachA, satBiasAttachA, satSagAttachA, detailAttachA, instabilityAttachA, offsetAttachA };
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

	using ST = int;
	auto setupSlider = [this] (BarSlider& slider, const juce::String& /*tooltip*/, ST type) {
		addAndMakeVisible (slider);
		slider.setOwner (this);
		slider.setType (type);
		setupBar (slider);
		slider.addListener (this);
	};

	setupSlider (r.hp,    "HP Filter " + suffix,                       BarSliderType::HpFreq);
	setupSlider (r.lp,    "LP Filter " + suffix,                       BarSliderType::LpFreq);
	setupSlider (r.in,    "Input Gain " + suffix,                      BarSliderType::Input);
	setupSlider (r.out,   "Output Gain " + suffix,                     BarSliderType::Output);
	setupSlider (r.tilt,  "Tilt EQ " + suffix + " (-6/+6 dB)",    BarSliderType::Tilt);
	setupSlider (r.detail, "Detail " + suffix + " (0-100%)",           BarSliderType::Detail);
	setupSlider (r.series, "Series " + suffix + " (1-6x cascade)",      BarSliderType::Series);
	r.series.setAllowNumericPopup (false);
	setupSlider (r.instability,   "Instability " + suffix + " (0-100%)",         BarSliderType::Instability);
	setupSlider (r.offset, "Offset " + suffix + " (0-1000ms)",         BarSliderType::Offset);
	r.offset.setEnabled (false);  // read-only, set by ALIGN
	setupSlider (r.pan,   "Pan " + suffix + " (L-R)",                  BarSliderType::Pan);
	setupSlider (r.fred,  "Angle " + suffix + " (off-axis mic simulation)", BarSliderType::Fred);
	setupSlider (r.pos,   "Distance " + suffix + " (proximity/distance)",   BarSliderType::Pos);

	addAndMakeVisible (r.inv);   r.inv.setButtonText ("INV");         r.inv.addListener (this);
	r.inv.setTooltip ({});
	addAndMakeVisible (r.chaos); r.chaos.setButtonText ("CHSD"); r.chaos.addListener (this);
	r.chaos.addMouseListener (this, false);
	addAndMakeVisible (r.chaosFilter); r.chaosFilter.setButtonText ("CHSF"); r.chaosFilter.addListener (this);
	r.chaosFilter.addMouseListener (this, false);
	addAndMakeVisible (r.sidechain); r.sidechain.setButtonText ("SIDECHAIN"); r.sidechain.addListener (this);
	r.sidechain.setTooltip ({});

	{
		const auto& ids = kLoaderParams[juce::jlimit (0, 2, loaderIndex)];
		const float savedAmtD = audioProcessor.getValueTreeState().getRawParameterValue (chaosAmtId)->load();
		const float savedSpdD = audioProcessor.getValueTreeState().getRawParameterValue (chaosSpdId)->load();
		const float savedAmtF = audioProcessor.getValueTreeState().getRawParameterValue (ids.chaosAmtFilter)->load();
		const float savedSpdF = audioProcessor.getValueTreeState().getRawParameterValue (ids.chaosSpdFilter)->load();
		const float savedScGain = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainGain)->load();
		const float savedScSmooth = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainSmooth)->load();
		const float savedScHp = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainHp)->load();
		const float savedScLp = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainLp)->load();
		const bool savedScHpOn = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainHpOn)->load() >= 0.5f;
		const bool savedScLpOn = audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainLpOn)->load() >= 0.5f;
		const int savedScHpSlope = juce::roundToInt (audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainHpSlope)->load());
		const int savedScLpSlope = juce::roundToInt (audioProcessor.getValueTreeState().getRawParameterValue (ids.sidechainLpSlope)->load());
		r.chaos.setTooltip ({});
		r.chaosFilter.setTooltip ({});
		r.chaosDisp.setText ("", juce::dontSendNotification);
		r.chaosDisp.setInterceptsMouseClicks (true, false);
		r.chaosDisp.addMouseListener (this, false);
		r.chaosDisp.setTooltip (formatChaosTooltip (savedAmtD, savedSpdD));
		r.chaosDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setOpaque (false);
		addAndMakeVisible (r.chaosDisp);
		r.chaosFilterDisp.setText ("", juce::dontSendNotification);
		r.chaosFilterDisp.setInterceptsMouseClicks (true, false);
		r.chaosFilterDisp.addMouseListener (this, false);
		r.chaosFilterDisp.setTooltip (formatChaosTooltip (savedAmtF, savedSpdF));
		r.chaosFilterDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.chaosFilterDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.chaosFilterDisp.setOpaque (false);
		addAndMakeVisible (r.chaosFilterDisp);
		r.sidechainDisp.setText ("", juce::dontSendNotification);
		r.sidechainDisp.setInterceptsMouseClicks (true, false);
		r.sidechainDisp.addMouseListener (this, false);
		r.sidechainDisp.setTooltip (formatSidechainTooltip (savedScGain, savedScSmooth, savedScHpOn, savedScHp, savedScHpSlope, savedScLpOn, savedScLp, savedScLpSlope));
		r.sidechainDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.sidechainDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.sidechainDisp.setOpaque (false);
		addAndMakeVisible (r.sidechainDisp);
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
		r.exp.setTooltip ({});

		r.expDisp.setText ("", juce::dontSendNotification);
		r.expDisp.setInterceptsMouseClicks (true, false);
		r.expDisp.addMouseListener (this, false);
		r.expDisp.setTooltip (formatExpTooltip (savedOrder, savedRatio));
		r.expDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.expDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.expDisp.setOpaque (false);
		addAndMakeVisible (r.expDisp);
	}

	auto setupModeCombo = [this] (juce::ComboBox& combo) {
		addAndMakeVisible (combo);
		combo.addItem ("L+R", 1);
		combo.addItem ("M/S", 2);
		combo.addItem ("MID", 3);
		combo.addItem ("SIDE", 4);
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

	setupSlider (r.mix, "Mix " + suffix + " (Dry/Wet)", BarSliderType::Mix);

	// Saturation controls
	{
		addAndMakeVisible (r.satType);
		r.satType.addItem ("CLEAN",     1);
		r.satType.addItem ("TAPE",      2);
		r.satType.addItem ("TUBE",      3);
		r.satType.addItem ("TRANSISTOR", 4);
		r.satType.addItem ("DIODE",     5);
		r.satType.addItem ("OVERDRIVE A", 6);
		r.satType.addItem ("OVERDRIVE B", 7);
		r.satType.addItem ("CLIPPER",   8);
		r.satType.addItem ("NAM",       9);
		r.satType.setJustificationType (juce::Justification::centred);
		r.satType.setLookAndFeel (&lnf);
		r.satType.addListener (this);

		addAndMakeVisible (r.raw);
		r.raw.setButtonText ("RAW");
		r.raw.setLookAndFeel (&lnf);
	}

	addAndMakeVisible (r.namBrowse);
	r.namBrowse.setButtonText ("...");
	r.namBrowse.setOwner (this, loaderIndex);
	r.namBrowse.setLookAndFeel (&lnf);
	r.namBrowse.addListener (this);
	r.namBrowse.setColour (juce::TextButton::buttonColourId, activeScheme.bg);
	r.namBrowse.setTooltip ("No file loaded");

	addAndMakeVisible (r.namDisplay);
	r.namDisplay.setOwner (this, loaderIndex);
	TR::configureLoaderFileLabel (r.namDisplay, activeScheme, "No file loaded", "Drop or browse a .nam model");

	TR::LoaderClearButton* clearButtons[] = { &namClearButtonA, &namClearButtonB, &namClearButtonC };
	auto& namClear = *clearButtons[juce::jlimit (0, 2, loaderIndex)];
	addAndMakeVisible (namClear);
	namClear.setScheme (activeScheme);
	namClear.addListener (this);
	namClear.setTooltip ("Clear NAM model");
	namClear.setVisible (false);

	setupSlider (r.satDrive, "Drive " + suffix + " (0-100%)",         BarSliderType::SatDrive);
	setupSlider (r.namSlim, "NAM Size " + suffix + " (0% lightest, 100% full)", BarSliderType::NamSlim);
	setupSlider (r.satChar, "Char " + suffix + " (low emphasis)",   BarSliderType::SatChar);
	setupSlider (r.satTypeCtrl,   "Type " + suffix + " (model modulation)", BarSliderType::SatTypeCtrl);
	setupSlider (r.satBias,  "Bias " + suffix + " (asymmetry)",       BarSliderType::SatBias);
	setupSlider (r.satSag,   "Dynamics " + suffix + " (model-dependent dynamics)", BarSliderType::SatSag);
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
	a.sidechainAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (params, ids.sidechain, ui.sidechain);
	a.expAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.exp,     ui.exp);
	a.modeInAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeIn,  ui.modeIn);
	a.modeOutAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeOut, ui.modeOut);
	a.sumBusAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.sumBus, ui.sumBus);
	a.filterPosAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.filterPos, ui.filterPos);
	a.mixAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.mix,     ui.mix);

	a.satTypeAtt.reset();
	a.rawAtt      = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.satRaw,   ui.raw);
	a.satDriveAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satDrive, ui.satDrive);
	a.namSlimAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.namSlim,  ui.namSlim);
	a.satCharAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satChar, ui.satChar);
	a.satTypeCtrlAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satTypeCtrl,   ui.satTypeCtrl);
	a.satBiasAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satBias,  ui.satBias);
	a.satSagAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.satSag,   ui.satSag);
	a.detailAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.detail,   ui.detail);
	a.instabilityAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.instability,      ui.instability);
	a.offsetAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.offset,    ui.offset);

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
	globalMixSlider.setType (BarSliderType::GlobalMix);
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
	globalOutputSlider.setType (BarSliderType::GlobalOutput);
	setupBar (globalOutputSlider);
	globalOutputSlider.setSkewFactor (SATTRAudioProcessor::kGainSkew);
	globalOutputSlider.addListener (this);

	// Limiter threshold bar slider (footer)
	addAndMakeVisible (limThresholdSlider);
	limThresholdSlider.setOwner (this);
	limThresholdSlider.setType (BarSliderType::LimThreshold);
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
	alignButton.setRightClickTextOnly (true);
	alignButton.onRightClick = [this]()
	{
		audioProcessor.setDryAlignModeEnabled (! audioProcessor.isDryAlignModeEnabled());
		updateAlignModeUi();
	};
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
	updateAlignModeUi();

	// Initialize per-loader collapse state from processor
	ioExpandedA_ = audioProcessor.getUiIoExpanded (0);
	ioExpandedB_ = audioProcessor.getUiIoExpanded (1);
	ioExpandedC_ = audioProcessor.getUiIoExpanded (2);
	singleLoaderIoExpanded_ = ioExpandedA_;

	// Initialize shared layout spec from processor state
	layoutSpec_.ioExpandedA = ioExpandedA_;
	layoutSpec_.ioExpandedB = ioExpandedB_;
	layoutSpec_.ioExpandedC = ioExpandedC_;
	layoutSpec_.singleLoaderIoExpanded = singleLoaderIoExpanded_;

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

	// Restore persisted UI state from processor (palette, CRT, I/O FX, colors)
	paletteState.useCustom = audioProcessor.getUiUseCustomPalette();
	ioFxEnabled      = audioProcessor.getUiIoFxEnabled();
	crtEnabled       = audioProcessor.getUiFxTailEnabled();
	applyCrtState (crtEnabled);
	for (int i = 0; i < TR::LoaderPaletteState::colourCount; ++i)
		paletteState.customColours[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

	// Initialize palette
	refreshActivePalette();

	// Configure shared BarSlider format callback (replaces old BarSlider::getTextFromValue)
	auto barFormatFn = [this](int type, double v) -> juce::String
	{
		switch (type)
		{
			case BarSliderType::HpFreq:
			case BarSliderType::LpFreq:
				return juce::String (v, 1) + " Hz";
			case BarSliderType::Input:
			case BarSliderType::Output:
			case BarSliderType::GlobalOutput:
				return formatGainFaderDb ((float) v);
			case BarSliderType::LimThreshold:
			case BarSliderType::Tilt:
				return juce::String (v, 1) + " dB";
			case BarSliderType::Series:
				return juce::String (static_cast<int> (std::round (v))) + "x";
			case BarSliderType::Detail:
			case BarSliderType::Instability:
				return juce::String (juce::roundToInt (v * 100.0)) + "%";
			case BarSliderType::Offset:
				return formatSatOffsetMsForUi (v);
			case BarSliderType::Pan:
			{
				double percent = v * 100.0;
				if (std::abs (percent - 50.0) < 1.0) return "C";
				if (percent < 50.0) return "L" + juce::String (50.0 - percent, 0);
				return "R" + juce::String (percent - 50.0, 0);
			}
			case BarSliderType::Fred: case BarSliderType::Pos:
			case BarSliderType::Mix:  case BarSliderType::GlobalMix:
			case BarSliderType::SatDrive: case BarSliderType::NamSlim:
			case BarSliderType::SatChar:  case BarSliderType::SatTypeCtrl:
			case BarSliderType::SatBias:  case BarSliderType::SatSag:
				return juce::String (juce::roundToInt (v * 100.0)) + "%";
			default: break;
		}
		return juce::String (v, 2);
	};

	// Apply format callback to all BarSliders
	for (auto* s : { &hpFreqSliderA, &lpFreqSliderA, &inSliderA, &outSliderA, &tiltSliderA,
	                 &seriesSliderA, &panSliderA, &fredSliderA, &posSliderA,
	                 &mixSliderA, &satDriveSliderA, &namSlimSliderA, &satCharSliderA,
	                 &satTypeCtrlSliderA, &satBiasSliderA, &satSagSliderA,
	                 &detailSliderA, &instabilitySliderA, &offsetSliderA,
	                 &hpFreqSliderB, &lpFreqSliderB, &inSliderB, &outSliderB, &tiltSliderB,
	                 &seriesSliderB, &panSliderB, &fredSliderB, &posSliderB,
	                 &mixSliderB, &satDriveSliderB, &namSlimSliderB, &satCharSliderB,
	                 &satTypeCtrlSliderB, &satBiasSliderB, &satSagSliderB,
	                 &detailSliderB, &instabilitySliderB, &offsetSliderB,
	                 &hpFreqSliderC, &lpFreqSliderC, &inSliderC, &outSliderC, &tiltSliderC,
	                 &seriesSliderC, &panSliderC, &fredSliderC, &posSliderC,
	                 &mixSliderC, &satDriveSliderC, &namSlimSliderC, &satCharSliderC,
	                 &satTypeCtrlSliderC, &satBiasSliderC, &satSagSliderC,
	                 &detailSliderC, &instabilitySliderC, &offsetSliderC,
	                 &globalMixSlider, &globalOutputSlider, &limThresholdSlider })
		s->setFormatFn (barFormatFn);

	// Configure NAM browse button / label callbacks
	auto namExtCheck = [](const juce::String& p) { return p.endsWithIgnoreCase (".nam"); };
	auto namLoad = [this](int idx, const juce::String& path) { loadNamFileFromPath (idx, path); };
	auto namGetFolder = [this](int idx) -> juce::File& {
		return idx == 1 ? currentNamFolderB : (idx == 2 ? currentNamFolderC : currentNamFolderA);
	};
	auto namDblClick = [this](int idx) { openNamFileChooser (idx); };
	for (auto* btn : { &namBrowseButtonA, &namBrowseButtonB, &namBrowseButtonC })
		{ btn->setExtCheckFn (namExtCheck); btn->setLoadFn (namLoad); btn->setGetFolderFn (namGetFolder); }
	for (auto* lbl : { &namFileDisplayA, &namFileDisplayB, &namFileDisplayC })
		{ lbl->setExtCheckFn (namExtCheck); lbl->setLoadFn (namLoad); lbl->setGetFolderFn (namGetFolder); lbl->setDblClickFn (namDblClick); }

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
	firstVisibleLoaderIndex_ = juce::jlimit (0,
	                                         kCompactMaxVisibleLoaders - visibleLoaderCount_,
	                                         audioProcessor.getUiFirstVisibleLoaderIndex());
	if (visibleLoaderCount_ == 1)
	{
		const bool selectedExpanded = firstVisibleLoaderIndex_ == 0 ? ioExpandedA_
		                           : firstVisibleLoaderIndex_ == 1 ? ioExpandedB_
		                           :                                  ioExpandedC_;
		syncSingleLoaderIoExpandedState (selectedExpanded);
	}
	setSize (restoredW, restoredH);
	setResizable (true, true);
	setResizeLimits (getCompactTargetWidthForLoaderCount (kCompactMinVisibleLoaders),
	                 kCompactFixedHeightPx,
	                 getCompactTargetWidthForLoaderCount (kCompactMaxVisibleLoaders),
	                 kCompactFixedHeightPx);

	// Initialize loader enabled/disabled visual state
	updateNamFileDisplays();
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
	audioProcessor.setUiUseCustomPalette (paletteState.useCustom);
	audioProcessor.setUiFxTailEnabled (crtEnabled);
	audioProcessor.setUiIoFxEnabled (ioFxEnabled);

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
	namBrowseButtonA.setLookAndFeel (nullptr);
	namBrowseButtonB.setLookAndFeel (nullptr);
	namBrowseButtonC.setLookAndFeel (nullptr);

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

	// Title & version
	{
		TR::drawLoaderHeaderAndVersion (g, activeScheme, "SAT-TR", InfoContent::version,
		                                cachedHeaderTitleX_, getInfoIconArea());

		TR::paintLoaderFooterGlobalControls (g, activeScheme,
		                                      footerExpanded_,
		                                      cachedFooterPanelArea_,
		                                      cachedFooterTitleArea_,
		                                      routeCombo, matchCombo, mixModeCombo,
		                                      limModeCombo, invPolCombo, invStrCombo,
		                                      globalMixSlider, dualMixBar_,
		                                      globalOutputSlider, limThresholdSlider,
		                                      "OS",
		                                      cachedMixTextFull, cachedMixTextShort, cachedMixIntOnly,
		                                      [this] (juce::ComboBox& combo) { return lnf.getComboBoxFont (combo); },
		                                      [] (float db) { return formatGainFaderDb (db); },
		                                      [] (float db)
		                                      {
			                                      return (db <= -35.9f) ? juce::String ("-36.0 dB")
			                                                             : juce::String (db, 1) + " dB";
		                                      });
	}

	// Per-loader MODE IN / MODE OUT labels (only when that loader is expanded)
	{
		auto drawModeLabels = [&] (juce::ComboBox& modeIn, juce::ComboBox& modeOut, juce::ComboBox& sumBus, juce::ComboBox& filterPos, int loaderIndex)
		{
			if (! modeIn.isVisible()) return;
			const auto refs = getLoaderRefs (loaderIndex);
			const int selectedModel = getSelectedSatTypeModelIndex (refs.satType);
			const bool isNam = selectedModel == static_cast<int> (SatEngine::Model::NAM);
			const bool namReady = ! isNam || audioProcessor.isNamModelLoadedForLoader (loaderIndex);
			const float alpha = (refs.enableBtn.getToggleState() && namReady) ? 1.0f : 0.35f;
			TR::drawLoaderModeRoutingLabels (g, activeScheme, modeIn, modeOut, sumBus, filterPos, alpha);
		};
		if (ioExpandedA_) drawModeLabels (modeInComboA, modeOutComboA, sumBusComboA, filterPosComboA, 0);
		if (ioExpandedB_) drawModeLabels (modeInComboB, modeOutComboB, sumBusComboB, filterPosComboB, 1);
		if (ioExpandedC_) drawModeLabels (modeInComboC, modeOutComboC, sumBusComboC, filterPosComboC, 2);
	}

	// Draw gear icon (in paint, like other TR plugins)
	{
		if (cachedInfoGearPath.isEmpty())
			updateInfoIconCache();

		TR::drawLoaderInfoGear (g, activeScheme, cachedInfoGearPath, cachedInfoGearHole);
	}

	// Draw value legends for all bar sliders
	{
		for (int loader = 0; loader < 3; ++loader)
		{
			auto refs = getLoaderRefs (loader);
			const bool enabled = refs.enableBtn.getToggleState();
			const int colR = columnRight_[loader];

			juce::Slider* loaderSliders[kNumCachedParams] = {
				&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt, &refs.series,
				&refs.pan, &refs.fred, &refs.pos, &refs.mix,
				&refs.satDrive, &refs.namSlim, &refs.satChar, &refs.satTypeCtrl, &refs.satBias, &refs.satSag, &refs.detail, &refs.instability, &refs.offset
			};

			for (int i = 0; i < kNumCachedParams; ++i)
			{
				if (loaderSliders[i]->isVisible())
				{
					const bool sliderEnabled = enabled && loaderSliders[i]->isEnabled();
					const auto valueArea = getValueAreaFor (loaderSliders[i]->getBounds(), colR);
					cachedValueAreas_[(size_t) (loader * kNumCachedParams + i)] = valueArea;
					TR::drawLoaderValueLegend (g, activeScheme, valueArea,
					                           cachedTexts[loader][i].full,
					                           cachedTexts[loader][i].short_,
					                           cachedTexts[loader][i].intOnly,
					                           sliderEnabled ? 1.0f : 0.35f);
				}
			}

			if (refs.filterBar.isVisible())
			{
				const auto filterValueArea = getValueAreaFor (refs.filterBar.getBounds(), colR);
				TR::drawLoaderFilterLegend (g, activeScheme, filterValueArea, enabled ? 1.0f : 0.35f);
			}
		}
	}
}

void SATTRAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
	const bool tooltipVisible = tooltipWindow != nullptr && tooltipWindow->isVisible();
	if (tooltipVisible)
	{
		g.saveState();
		g.excludeClipRegion (tooltipWindow->getBounds().expanded (2));
	}

	const float dimAlpha = promptOverlayActive ? 0.25f : 1.0f;
	auto dimmedScheme = activeScheme;
	if (promptOverlayActive)
	{
		dimmedScheme.fg   = dimmedScheme.fg.withAlpha (dimAlpha);
		dimmedScheme.text = dimmedScheme.text.withAlpha (dimAlpha);
	}

	TR::drawLoaderToggleBar (g, cachedToggleBarAreaA_, ioExpandedA_, dimmedScheme);
	TR::drawLoaderToggleBar (g, cachedToggleBarAreaB_, ioExpandedB_, dimmedScheme);
	TR::drawLoaderToggleBar (g, cachedToggleBarAreaC_, ioExpandedC_, dimmedScheme);

	for (int i = 0; i < cachedLoaderTabCount_; ++i)
	{
		const auto area = cachedLoaderTabAreas_[i];
		if (area.isEmpty()) continue;
		const bool selected = cachedLoaderTabStartIndices_[i] == firstVisibleLoaderIndex_;
		TR::paintLoaderTab (g, area, selected, dimmedScheme,
			TR::makeLoaderTabLabel (cachedLoaderTabStartIndices_[i], visibleLoaderCount_));
	}

	if (! cachedFooterRailArea_.isEmpty())
		TR::paintLoaderFooterRailTab (g, cachedFooterRailArea_, footerExpanded_, dimmedScheme);

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

	// Shared layout: tabs, footer rail, column areas → writes to layoutSpec_
	TR::LoaderLayoutEngine::performLayout (layoutSpec_, getLocalBounds());
	syncLayoutSpecToMembers();

	// Clear plugin-owned caches
	cachedToggleBarAreaA_ = {};
	cachedToggleBarAreaB_ = {};
	cachedToggleBarAreaC_ = {};
	cachedValueAreas_.fill (juce::Rectangle<int>());

	// Header: ALIGN button (plugin-specific)
	{
		TR::layoutHeaderAlignButtonShared (alignButton, cachedHeaderTitleX_, 100);
	}

	if (footerExpanded_)
	{
		for (int loader = 0; loader < 3; ++loader)
			hideLoaderSection (loader);
		auto footerArea = getLocalBounds();
		footerArea.removeFromTop (40);
		footerArea.removeFromBottom (TR::LoaderLayoutSpec::kFooterRailSlotHeightPx);
		layoutFooterControls (footerArea);
	}
	else
	{
		hideFooterControls();
		for (int loader = 0; loader < 3; ++loader)
			hideLoaderSection (loader);

		// Restore column edges from layoutSpec (cleared by hideLoaderSection)
		for (int i = 0; i < 3; ++i)
		{
			columnLeft_[i]  = layoutSpec_.columnLeft[i];
			columnRight_[i] = layoutSpec_.columnRight[i];
		}

		// Layout each visible loader column
		for (int viewSlot = 0; viewSlot < visibleLoaderCount_; ++viewSlot)
		{
			const int loader = firstVisibleLoaderIndex_ + viewSlot;
			juce::Rectangle<int> area = layoutSpec_.columnAreas[loader];
			if (! area.isEmpty())
				layoutLoaderSection (area, loader);
		}
	}

	promptOverlay.setBounds (getLocalBounds());

	legendDirty = true;
	updateInfoIconCache();

	// Sync plugin-only cached areas back to layoutSpec so
	// subsequent syncLayoutSpecToMembers() calls don't overwrite them
	layoutSpec_.cachedFooterPanelArea = cachedFooterPanelArea_;
	layoutSpec_.cachedFooterTitleArea = cachedFooterTitleArea_;
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

	auto refs = getLoaderRefs (loaderIndex);
	TR::layoutSatLoaderChrome (area,
	                           loaderIndex,
	                           buttonH,
	                           gap,
	                           toggleBarH,
	                           toggleToFirstControlGapPx,
	                           refs,
	                           [this] (int i, juce::Rectangle<int> toggleBarArea)
	                           {
		                           if (i == 0)
			                           cachedToggleBarAreaA_ = toggleBarArea;
		                           else if (i == 1)
			                           cachedToggleBarAreaB_ = toggleBarArea;
		                           else
			                           cachedToggleBarAreaC_ = toggleBarArea;
	                           });

	const int sliderW = static_cast<int> (area.getWidth() * 0.50f);
	auto& namClear   = pick (namClearButtonA,  namClearButtonB,  namClearButtonC);
	const int selectedModel = getSelectedSatTypeModelIndex (refs.satType);
	const bool isCleanModel = selectedModel == static_cast<int> (SatEngine::Model::Clean);
	const bool isNamModel = selectedModel == static_cast<int> (SatEngine::Model::NAM);
	const bool isClipperModel = selectedModel == static_cast<int> (SatEngine::Model::Clipper);
	const bool showRaw = ! isCleanModel && ! isNamModel && ! isClipperModel;

	const bool expanded = (loaderIndex == 0) ? ioExpandedA_
	                     : (loaderIndex == 1) ? ioExpandedB_
	                     :                      ioExpandedC_;

	constexpr int loaderHeaderBlockH = 72;
	const int modeComboLabelOffset = 21;
	const int checkRowH = 42;
	const int collapsedCheckH = 42;
	const int checkH = expanded ? (checkRowH * 2 + gap) : collapsedCheckH;

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
	const int sharedContentH = area.getHeight() - collapsedCheckH - compactBottomSpacer;
	const int sliderH = juce::jmax (18, (sharedContentH - loaderHeaderBlockH - (parameterGaps * gap)) / parameterRows);
	const int visualSliderH = juce::jlimit (24, 32, sliderH);
	const int visualComboH = 38;
	const int visualAlgorithmH = 42;

	if (expanded)
	{
		TR::layoutSatExpandedIoPanel (contentArea,
		                               checkArea,
		                               sliderW,
		                               buttonH,
		                               gap,
		                               sliderH,
		                               visualSliderH,
		                               visualComboH,
		                               visualAlgorithmH,
		                               loaderHeaderBlockH,
		                               refs,
		                               namClear,
		                               showRaw,
		                               isNamModel);
	}
	else
	{
		TR::layoutSatCollapsedMainPanel (contentArea,
		                                  checkArea,
		                                  sliderW,
		                                  buttonH,
		                                  gap,
		                                  sliderH,
		                                  checkH,
		                                  visualSliderH,
		                                  visualAlgorithmH,
		                                  loaderHeaderBlockH,
		                                  refs,
		                                  namClear,
		                                  showRaw,
		                                  isNamModel);
	}
}

// ----------------------------------------------------------------
//  Loader enabled/disabled visual state
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::updateLoaderEnabledState (int loaderIndex)
{
	auto r = getLoaderRefs (loaderIndex);

	const bool enabled = r.enableBtn.getToggleState();
	const int selectedModel = getSelectedSatTypeModelIndex (r.satType);
	const bool isNam = (selectedModel == static_cast<int> (SatEngine::Model::NAM));
	const bool namReady = ! isNam || audioProcessor.isNamModelLoadedForLoader (loaderIndex);
	const float alpha = (enabled && namReady) ? 1.0f : 0.35f;
	const bool interactive = enabled && namReady;

	juce::Component* components[] = {
		&r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		&r.series, &r.pan, &r.fred, &r.pos,
		&r.inv, &r.chaos, &r.chaosFilter, &r.sidechain, &r.chaosDisp, &r.chaosFilterDisp, &r.sidechainDisp,
		&r.exp, &r.expDisp,
		&r.modeIn, &r.modeOut, &r.sumBus, &r.filterPos,
		&r.filterBar, &r.mix,
		&r.raw,
		&r.satDrive, &r.namSlim, &r.satChar, &r.satTypeCtrl, &r.satBias, &r.satSag, &r.detail, &r.instability, &r.offset
	};

	for (auto* c : components)
	{
		c->setAlpha (alpha);
		c->setEnabled (interactive);
	}

	r.satType.setAlpha (enabled ? 1.0f : 0.35f);
	r.satType.setEnabled (enabled);

	// Also sync sat control enablement (CLEAN disables sat knobs)
	updateSatControlsEnabledState (loaderIndex);

	repaint();
}

void SATTRAudioProcessorEditor::updateSatControlsEnabledState (int loaderIndex)
{
	auto r = getLoaderRefs (loaderIndex);
	TR::LoaderClearButton* clearButtons[] = { &namClearButtonA, &namClearButtonB, &namClearButtonC };
	auto& namClear = *clearButtons[juce::jlimit (0, 2, loaderIndex)];

	// Check if the loader itself is enabled first
	const bool loaderEnabled = r.enableBtn.getToggleState();

	const int selectedModel = getSelectedSatTypeModelIndex (r.satType);
	const bool isClean = (selectedModel == static_cast<int> (SatEngine::Model::Clean));
	const bool isNam = (selectedModel == static_cast<int> (SatEngine::Model::NAM));
	const bool isClipper = (selectedModel == static_cast<int> (SatEngine::Model::Clipper));
	const bool namReady = ! isNam || audioProcessor.isNamModelLoadedForLoader (loaderIndex);

	// Sat-specific controls should be interactive only when loader enabled AND not CLEAN.
	// Modal prompts are blocked by promptOverlay itself, so they must not alter visual alpha.
	const bool satInteractive = loaderEnabled && ! isClean && ! isNam;
	const float satAlpha = satInteractive ? 1.0f : 0.35f;

	juce::Component* satControls[] = {
		&r.satDrive, &r.satChar, &r.satTypeCtrl, &r.satBias, &r.satSag, &r.series, &r.detail, &r.instability, &r.raw
	};

	for (auto* c : satControls)
	{
		c->setAlpha (satAlpha);
		c->setEnabled (satInteractive);
	}

	const int safeLoaderIndex = juce::jlimit (0, 2, loaderIndex);
	if (isClean || isNam || isClipper)
	{
		rawVisualOverrideActive_[safeLoaderIndex] = true;
		r.raw.setToggleState (false, juce::dontSendNotification);
	}
	else if (rawVisualOverrideActive_[safeLoaderIndex])
	{
		rawVisualOverrideActive_[safeLoaderIndex] = false;
		if (auto* rawParam = audioProcessor.getValueTreeState().getRawParameterValue (kLoaderParams[safeLoaderIndex].satRaw))
			r.raw.setToggleState (rawParam->load() > 0.5f, juce::dontSendNotification);
	}

	const bool namInteractive = loaderEnabled && isNam;
	const float namAlpha = namInteractive ? 1.0f : 0.35f;
	r.namBrowse.setAlpha (namAlpha);
	r.namBrowse.setEnabled (namInteractive);
	r.namDisplay.setAlpha (namAlpha);
	r.namDisplay.setEnabled (namInteractive);
	const bool namSlimInteractive = namInteractive && namReady;
	r.namSlim.setAlpha (namSlimInteractive ? 1.0f : 0.35f);
	r.namSlim.setEnabled (namSlimInteractive);
	namClear.setAlpha (namSlimInteractive ? 1.0f : 0.35f);
	namClear.setEnabled (namSlimInteractive);
	namClear.setTooltip (namSlimInteractive ? juce::String ("Clear NAM model") : juce::String());

	if (isClean || isNam)
	{
		sidechainVisualOverrideActive_[safeLoaderIndex] = true;
		r.sidechain.setToggleState (false, juce::dontSendNotification);
		r.sidechain.setAlpha (0.35f);
		r.sidechain.setEnabled (false);
		r.sidechainDisp.setAlpha (0.35f);
		r.sidechainDisp.setEnabled (false);
	}
	else
	{
		if (sidechainVisualOverrideActive_[safeLoaderIndex])
		{
			sidechainVisualOverrideActive_[safeLoaderIndex] = false;
			if (auto* sidechainParam = audioProcessor.getValueTreeState().getRawParameterValue (kLoaderParams[safeLoaderIndex].sidechain))
				r.sidechain.setToggleState (sidechainParam->load() > 0.5f, juce::dontSendNotification);
		}
		const float sidechainAlpha = loaderEnabled ? 1.0f : 0.35f;
		r.sidechain.setAlpha (sidechainAlpha);
		r.sidechain.setEnabled (loaderEnabled);
		r.sidechainDisp.setAlpha (sidechainAlpha);
		r.sidechainDisp.setEnabled (loaderEnabled);
	}

	const bool typeInteractive = satInteractive;
	r.satTypeCtrl.setAlpha (typeInteractive ? 1.0f : 0.35f);
	r.satTypeCtrl.setEnabled (typeInteractive);
}


void SATTRAudioProcessorEditor::updateIoFxMeterSliders()
{
	TR::updateIoFxMeterSlidersShared (
		ioFxEnabled, paletteState.useCustom, paletteState.defaultColours, paletteState.customColours,
		[this](int i) { return getLoaderRefs (i); },
		[this](int i) { return audioProcessor.getLoaderInputMeterPeak (i); },
		[this](int i) { return audioProcessor.getLoaderOutputMeterPeak (i); },
		[this]()     { return audioProcessor.getGlobalOutputMeterPeak(); },
		globalOutputSlider);
}

// ----------------------------------------------------------------
//  Callbacks
// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::timerCallback()
{
	updateIoFxMeterSliders();

	// Sync filter bars from processor
	filterBarA_.updateFromProcessor();
	filterBarB_.updateFromProcessor();
	filterBarC_.updateFromProcessor();

	bool namDisplayDirty = false;
	for (int i = 0; i < 3; ++i)
	{
		const auto path = audioProcessor.getNamModelPathForLoader (i);
		if (path != cachedNamDisplayPaths_[i])
		{
			namDisplayDirty = true;
			break;
		}
	}
	if (namDisplayDirty)
		updateNamFileDisplays();

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
	legendDirty = true;

	if (slider == nullptr)
	{
		repaint();
		return;
	}

	auto repaintArea = [this] (juce::Rectangle<int> area)
	{
		area = area.expanded (8, 6).getIntersection (getLocalBounds());
		if (area.isEmpty())
			repaint();
		else
			repaint (area);
	};

	auto repaintFooterSlider = [&] (juce::Slider& footerSlider, int valueWidthPx)
	{
		const auto valueArea = footerExpanded_
			? makeExpandedFooterValueArea (cachedFooterPanelArea_.getRight(), footerSlider.getBounds())
			: makeFooterValueArea (footerSlider.getBounds(), valueWidthPx);
		repaintArea (footerSlider.getBounds().getUnion (valueArea));
	};

	for (int loader = 0; loader < 3; ++loader)
	{
		auto refs = getLoaderRefs (loader);
		const int colR = columnRight_[loader];

		if (colR <= 0)
			continue;

		juce::Slider* loaderSliders[kNumCachedParams] = {
			&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt, &refs.series,
			&refs.pan, &refs.fred, &refs.pos, &refs.mix,
			&refs.satDrive, &refs.namSlim, &refs.satChar, &refs.satTypeCtrl, &refs.satBias, &refs.satSag, &refs.detail, &refs.instability, &refs.offset
		};

		for (auto* s : loaderSliders)
		{
			if (slider == s)
			{
				repaintArea (s->getBounds().getUnion (getValueAreaFor (s->getBounds(), colR)));
				return;
			}
		}
	}

	if (slider == &globalMixSlider)
	{
		repaintFooterSlider (globalMixSlider, kFooterMixValueWidthPx);
		return;
	}

	if (slider == &globalOutputSlider)
	{
		repaintFooterSlider (globalOutputSlider, kFooterDbValueWidthPx);
		return;
	}

	if (slider == &limThresholdSlider)
	{
		repaintFooterSlider (limThresholdSlider, kFooterDbValueWidthPx);
		return;
	}

	repaint();
}

void SATTRAudioProcessorEditor::buttonClicked (juce::Button* button)
{
	if (button == &namBrowseButtonA) { openNamFileChooser (0); return; }
	if (button == &namBrowseButtonB) { openNamFileChooser (1); return; }
	if (button == &namBrowseButtonC) { openNamFileChooser (2); return; }

	if (button == &namClearButtonA || button == &namClearButtonB || button == &namClearButtonC)
	{
		const int loaderIndex = (button == &namClearButtonB) ? 1 : (button == &namClearButtonC) ? 2 : 0;
		audioProcessor.clearNamModelForLoader (loaderIndex);
		updateNamFileDisplays();
		updateLoaderEnabledState (loaderIndex);
		legendDirty = true;
		refreshLegendTextCache();
		resized();
		repaint();
		return;
	}
}

void SATTRAudioProcessorEditor::openNamFileChooser (int loaderIndex)
{
	using namespace TR;

	const int idx = juce::jlimit (0, 2, loaderIndex);
	juce::File* folders[] = { &currentNamFolderA, &currentNamFolderB, &currentNamFolderC };
	auto& startFolder = *folders[idx];
	const auto loadedPath = audioProcessor.getNamModelPathForLoader (idx);
	if (loadedPath.isNotEmpty())
	{
		const auto loadedParent = juce::File (loadedPath).getParentDirectory();
		if (loadedParent.exists() && loadedParent.isDirectory())
			startFolder = loadedParent;
	}
	if (! startFolder.exists() || ! startFolder.isDirectory())
		startFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

	namFileChooser = std::make_unique<juce::FileChooser> ("Load NAM model",
	                                                       startFolder,
	                                                       "*.nam");

	juce::Component::SafePointer<SATTRAudioProcessorEditor> chooserSafeThis (this);
	namFileChooser->launchAsync (juce::FileBrowserComponent::openMode
	                             | juce::FileBrowserComponent::canSelectFiles,
	                             [chooserSafeThis, idx] (const juce::FileChooser& chooser)
	{
		if (chooserSafeThis == nullptr)
			return;

		const auto file = chooser.getResult();
		if (file.existsAsFile())
			chooserSafeThis->loadNamFileFromPath (idx, file.getFullPathName());
	});
}

void SATTRAudioProcessorEditor::loadNamFileFromPath (int loaderIndex, const juce::String& path)
{
	const int idx = juce::jlimit (0, 2, loaderIndex);
	const juce::File file (path);
	juce::String error;
	if (! audioProcessor.loadNamModelForLoader (loaderIndex, path, error))
	{
		juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
		                                        "NAM load failed",
		                                        error.isNotEmpty() ? error : "Could not load NAM model.");
		return;
	}

	if (file.existsAsFile())
	{
		juce::File* folders[] = { &currentNamFolderA, &currentNamFolderB, &currentNamFolderC };
		*folders[idx] = file.getParentDirectory();
	}

	updateNamFileDisplays();
	repaint();
}

void SATTRAudioProcessorEditor::updateNamFileDisplays()
{
	NamFileLabel* displays[] = { &namFileDisplayA, &namFileDisplayB, &namFileDisplayC };
	NamBrowseButton* browseButtons[] = { &namBrowseButtonA, &namBrowseButtonB, &namBrowseButtonC };
	juce::File* folders[] = { &currentNamFolderA, &currentNamFolderB, &currentNamFolderC };
	for (int i = 0; i < 3; ++i)
	{
		const auto name = audioProcessor.getNamDisplayNameForLoader (i);
		const auto path = audioProcessor.getNamModelPathForLoader (i);
		juce::String fileName = name;
		if (path.isNotEmpty())
		{
			juce::File file (path);
			if (fileName.isEmpty())
				fileName = file.getFileName();
			const auto parent = file.getParentDirectory();
			if (parent.exists() && parent.isDirectory())
				*folders[i] = parent;
		}

		displays[i]->setText (fileName.isNotEmpty() ? fileName : "No file loaded", juce::dontSendNotification);
		displays[i]->setTooltip (path.isNotEmpty()
			? path
			: "Drop or browse a .nam model");
		browseButtons[i]->setTooltip (path.isNotEmpty()
			? fileName
			: "No file loaded");
		cachedNamDisplayPaths_[i] = path;
		updateLoaderEnabledState (i);
	}
}

void SATTRAudioProcessorEditor::updateAlignModeUi()
{
	const bool dryAlign = audioProcessor.isDryAlignModeEnabled();
	alignButton.setButtonText (dryAlign ? "A+DI" : "ALIGN");
	alignButton.setTooltip ({});
	alignButton.repaint();
}

void SATTRAudioProcessorEditor::comboBoxChanged (juce::ComboBox* combo)
{
	if (combo == &satTypeComboA) { commitSatTypeComboSelection (0); updateLoaderEnabledState (0); resized(); }
	else if (combo == &satTypeComboB) { commitSatTypeComboSelection (1); updateLoaderEnabledState (1); resized(); }
	else if (combo == &satTypeComboC) { commitSatTypeComboSelection (2); updateLoaderEnabledState (2); resized(); }

	legendDirty = true;
	repaint();
}

void SATTRAudioProcessorEditor::applyCrtState (bool enabled)
{
	// Legacy Graphic FX/CRT is intentionally disabled. I/O FX owns current
	// visual metering, so old ui_fx_tail state must never attach an ImageEffect.
	juce::ignoreUnused (enabled);
	crtEnabled = false;
	crtEffect.setEnabled (false);
	setComponentEffect (nullptr);
	crtTime = 0.0f;
	stopTimer();
	startTimerHz (ioFxEnabled ? kMeterTimerHz : kIdleTimerHz);
	repaint();
}

void SATTRAudioProcessorEditor::parameterChanged (const juce::String& paramID, float newValue)
{
	if (paramID == SATTRAudioProcessor::kParamUiFxTail)
	{
		applyCrtState (newValue > 0.5f);
		return;
	}
	if (paramID == SATTRAudioProcessor::kParamUiIoFx)
	{
		ioFxEnabled = newValue > 0.5f;
		stopTimer();
		startTimerHz (crtEnabled ? kCrtTimerHz : (ioFxEnabled ? kMeterTimerHz : kIdleTimerHz));
		updateIoFxMeterSliders();
		repaint();
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
					safeThis->updateLoaderEnabledState (idx);
					safeThis->resized();
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

	if (getTitleHitArea().contains (p))
	{
		if (e.mods.isPopupMenu())
		{
			audioProcessor.toggleSafeClipMode();
			repaint (getTitleHitArea().expanded (4));
		}

		showSafeClipTooltip();
		juce::Timer::callAfterDelay (80,
			[safeThis = juce::Component::SafePointer<SATTRAudioProcessorEditor> (this)]()
			{
				if (safeThis == nullptr)
					return;

				const auto localMouse = safeThis->getLocalPoint (nullptr, juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().toInt());
				if (safeThis->getTitleHitArea().contains (localMouse))
					safeThis->showSafeClipTooltip();
			});
		return;
	}

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
				const bool newState = ! b.state;
				if (visibleLoaderCount_ == 1)
				{
					syncSingleLoaderIoExpandedState (newState);
				}
				else
				{
					b.state = newState;
					audioProcessor.setUiIoExpanded (b.idx, b.state);
				}
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

	// CHAOS checkboxes: left-click is handled by the button, right-click opens amount/speed prompt.
	{
		juce::ToggleButton* enableBtns[]      = { &enableButtonA,  &enableButtonB,  &enableButtonC };
		juce::ToggleButton* chaosBtns[]       = { &chaosButtonA,   &chaosButtonB,   &chaosButtonC };
		juce::ToggleButton* chaosFilterBtns[] = { &chaosFilterButtonA, &chaosFilterButtonB, &chaosFilterButtonC };
		juce::Label*        chaosDisps[]      = { &chaosDisplayA,  &chaosDisplayB,  &chaosDisplayC };
		juce::Label*        chaosFilterDisps[]= { &chaosFilterDisplayA, &chaosFilterDisplayB, &chaosFilterDisplayC };

		for (int i = 0; i < 3; ++i)
		{
			if (! enableBtns[i]->getToggleState()
			 || (! chaosBtns[i]->isEnabled() && ! chaosFilterBtns[i]->isEnabled()))
				continue;

			const bool hitFilter = chaosFilterDisps[i]->isVisible()
				&& chaosFilterDisps[i]->getBounds().contains (p);

			const bool hitOffset = !hitFilter
				&& chaosDisps[i]->isVisible()
				&& chaosDisps[i]->getBounds().contains (p);

			if (hitFilter)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, true);
				else
					chaosFilterBtns[i]->setToggleState (! chaosFilterBtns[i]->getToggleState(), juce::sendNotificationSync);
				return;
			}
			if (hitOffset)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, false);
				else
					chaosBtns[i]->setToggleState (! chaosBtns[i]->getToggleState(), juce::sendNotificationSync);
				return;
			}
		}
	}

	// Sidechain checkbox: left-click on legend toggles; right-click on legend opens prompt.
	{
		juce::ToggleButton* enableBtns[] = { &enableButtonA,     &enableButtonB,     &enableButtonC };
		juce::ToggleButton* scBtns[]     = { &sidechainButtonA,  &sidechainButtonB,  &sidechainButtonC };
		juce::Label*        scDisps[]    = { &sidechainDisplayA, &sidechainDisplayB, &sidechainDisplayC };

		for (int i = 0; i < 3; ++i)
		{
			if (! enableBtns[i]->getToggleState() || ! scBtns[i]->isEnabled())
				continue;

			const bool hitSidechain = scDisps[i]->isVisible()
				&& scDisps[i]->getBounds().contains (p);

			if (hitSidechain)
			{
				if (e.mods.isPopupMenu())
					openSidechainPrompt (i);
				else
					scBtns[i]->setToggleState (! scBtns[i]->getToggleState(), juce::sendNotificationSync);
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
			if (! enableBtns[i]->getToggleState() || ! expBtns[i]->isEnabled())
				continue;

			const bool hitExp = expDisps[i]->isVisible()
				&& expDisps[i]->getBounds().contains (p);

			if (hitExp)
			{
				if (e.mods.isPopupMenu())
					openExpPrompt (i);
				else
					expBtns[i]->setToggleState (! expBtns[i]->getToggleState(), juce::sendNotificationSync);
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

void SATTRAudioProcessorEditor::mouseMove (const juce::MouseEvent& e)
{
	const auto p = e.getEventRelativeTo (this).getPosition();
	if (getTitleHitArea().contains (p))
	{
		showSafeClipTooltip();
	}
	else
	{
		if (tooltipWindow != nullptr)
			tooltipWindow->hideTip();
	}
}

void SATTRAudioProcessorEditor::mouseExit (const juce::MouseEvent& e)
{
	juce::ignoreUnused (e);
	if (tooltipWindow != nullptr)
		tooltipWindow->hideTip();
}

juce::Rectangle<int> SATTRAudioProcessorEditor::getTitleHitArea() const
{
	constexpr int titleY = 12;
	constexpr int titleH = 32;
	constexpr int titleW = 100;
	return { cachedHeaderTitleX_, titleY, titleW, titleH + 4 };
}

juce::String SATTRAudioProcessorEditor::getSafeClipTooltip() const
{
	return juce::String ("SAFE CLIP: ") + (audioProcessor.isSafeClipModeEnabled() ? "ON" : "OFF");
}

void SATTRAudioProcessorEditor::showSafeClipTooltip()
{
	const auto tip = getSafeClipTooltip();

	if (tooltipWindow != nullptr)
	{
		const auto screenPos = localPointToGlobal (getTitleHitArea().getTopLeft());
		tooltipWindow->displayTip (screenPos, tip);
	}
}

// ----------------------------------------------------------------
//  TR-style label/value system helpers
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::setupBar (juce::Slider& s)
{
    TR::setupLoaderBar (s);
}

void SATTRAudioProcessorEditor::syncLayoutSpecToMembers()
{
    visibleLoaderCount_      = layoutSpec_.visibleLoaderCount;
    firstVisibleLoaderIndex_ = layoutSpec_.firstVisibleLoaderIndex;
    footerExpanded_          = layoutSpec_.footerExpanded;
    singleLoaderIoExpanded_  = layoutSpec_.singleLoaderIoExpanded;
    ioExpandedA_             = layoutSpec_.ioExpandedA;
    ioExpandedB_             = layoutSpec_.ioExpandedB;
    ioExpandedC_             = layoutSpec_.ioExpandedC;
    applyingCompactResize_   = layoutSpec_.applyingCompactResize;
    cachedHeaderTitleX_      = layoutSpec_.headerTitleX;
    for (int i = 0; i < 3; ++i)
    {
        cachedLoaderTabAreas_[i]       = layoutSpec_.cachedLoaderTabAreas[i];
        cachedLoaderTabStartIndices_[i] = layoutSpec_.cachedLoaderTabStartIndices[i];
        columnLeft_[i]                 = layoutSpec_.columnLeft[i];
        columnRight_[i]                = layoutSpec_.columnRight[i];
    }
    cachedLoaderTabCount_    = layoutSpec_.cachedLoaderTabCount;
    cachedFooterRailArea_    = layoutSpec_.cachedFooterRailArea;
    cachedFooterPanelArea_   = layoutSpec_.cachedFooterPanelArea;
    cachedFooterTitleArea_   = layoutSpec_.cachedFooterTitleArea;
}

int SATTRAudioProcessorEditor::getCompactTargetWidthForLoaderCount (int loaderCount) noexcept
{
    return TR::LoaderLayoutEngine::getCompactTargetWidthForLoaderCount (layoutSpec_, loaderCount);
}

int SATTRAudioProcessorEditor::getMaxVisibleLoaderCountForWidth (int width) noexcept
{
    return TR::LoaderLayoutEngine::getMaxVisibleLoaderCountForWidth (layoutSpec_, width);
}

void SATTRAudioProcessorEditor::clearCompactRailAreas() noexcept
{
    TR::LoaderLayoutEngine::clearCompactRailAreas (layoutSpec_);
    syncLayoutSpecToMembers();
}

void SATTRAudioProcessorEditor::syncSingleLoaderIoExpandedState (bool expanded)
{
    TR::LoaderLayoutEngine::syncSingleLoaderIoExpandedState (layoutSpec_, expanded,
        [this] (int idx, bool exp) { audioProcessor.setUiIoExpanded (idx, exp); });
    syncLayoutSpecToMembers();
}

void SATTRAudioProcessorEditor::setVisibleLoaderCount (int loaderCount, bool requestResize)
{
    TR::LoaderLayoutEngine::setVisibleLoaderCount (layoutSpec_, loaderCount, requestResize,
        [this] { return getWidth(); },
        [this] { return getHeight(); },
        [this] (int w, int h) { setSize (w, h); },
        [this] { resized(); },
        [this] { repaint(); },
        nullptr);
    syncLayoutSpecToMembers();
}

void SATTRAudioProcessorEditor::setFirstVisibleLoaderIndex (int loaderIndex)
{
    TR::LoaderLayoutEngine::setFirstVisibleLoaderIndex (layoutSpec_, loaderIndex,
        [this] { resized(); },
        [this] { repaint(); },
        [this] (int idx) { audioProcessor.setUiFirstVisibleLoaderIndex (idx); });
    syncLayoutSpecToMembers();
}

void SATTRAudioProcessorEditor::setFooterExpanded (bool shouldBeExpanded)
{
    TR::LoaderLayoutEngine::setFooterExpanded (layoutSpec_, shouldBeExpanded,
        [this] { resized(); },
        [this] { repaint(); });
    syncLayoutSpecToMembers();
}

void SATTRAudioProcessorEditor::hideLoaderSection (int loaderIndex)
{
	auto refs = getLoaderRefs (loaderIndex);
	TR::LoaderClearButton* clearButtons[] = { &namClearButtonA, &namClearButtonB, &namClearButtonC };
	auto& namClear = *clearButtons[juce::jlimit (0, 2, loaderIndex)];

	juce::Component* components[] = {
		&refs.enableBtn,
		&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt,
		&refs.series, &refs.pan, &refs.fred, &refs.pos,
		&refs.inv, &refs.chaos, &refs.chaosFilter, &refs.sidechain, &refs.chaosDisp, &refs.chaosFilterDisp, &refs.sidechainDisp,
		&refs.exp, &refs.expDisp,
		&refs.modeIn, &refs.modeOut, &refs.sumBus, &refs.filterPos,
		&refs.filterBar, &refs.mix,
		&refs.satType, &refs.raw, &refs.namBrowse, &refs.namDisplay, &namClear,
		&refs.satDrive, &refs.namSlim, &refs.satChar, &refs.satTypeCtrl, &refs.satBias, &refs.satSag,
		&refs.detail, &refs.instability, &refs.offset
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
	TR::hideFooterControlsShared ({
		&globalMixSlider, &dualMixBar_, &globalOutputSlider, &limThresholdSlider,
		&matchCombo, &routeCombo, &mixModeCombo, &limModeCombo, &invPolCombo, &invStrCombo, &trimCombo
	});
}

void SATTRAudioProcessorEditor::layoutFooterControls (juce::Rectangle<int> area)
{
	TR::layoutSatFooterControls (area,
	                              cachedHeaderTitleX_,
	                              kCompactFooterPanelWidthPx,
	                              cachedFooterPanelArea_,
	                              cachedFooterTitleArea_,
	                              trimCombo,
	                              matchCombo,
	                              routeCombo,
	                              mixModeCombo,
	                              limModeCombo,
	                              invPolCombo,
	                              invStrCombo,
	                              globalMixSlider,
	                              dualMixBar_,
	                              globalOutputSlider,
	                              limThresholdSlider);
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
	const char* driveShortLabels[3];
	const char* girthShortLabels[3];
	const char* modShortLabels[3];
	const char* biasShortLabels[3];
	const char* reactShortLabels[3];
	for (int l = 0; l < 3; ++l)
	{
		auto lr = getLoaderRefs (l);
		const int satModel = getSelectedSatTypeModelIndex (lr.satType);
		driveLabels[l] = "DRIVE";
		girthLabels[l] = "CHAR";
		modLabels[l]   = "TYPE";
		biasLabels[l]  = "BIAS";
		driveShortLabels[l] = "DRIVE";
		girthShortLabels[l] = "CHAR";
		modShortLabels[l]   = "TYPE";
		biasShortLabels[l]  = "BIAS";
		switch (satModel)
		{
            case 1:
                girthLabels[l] = "CHAR";
                modLabels[l]   = "TYPE";
                reactLabels[l] = "COMP";
                girthShortLabels[l] = "CHAR";
                modShortLabels[l]   = "TYPE";
                reactShortLabels[l] = "COMP";
                break; // Tape
            case 2:
                girthLabels[l] = "CHAR";
                modLabels[l]   = "TYPE";
                reactLabels[l] = "SAG";
                girthShortLabels[l] = "CHAR";
                modShortLabels[l]   = "TYPE";
                reactShortLabels[l] = "SAG";
                break; // Tube
			case 3:
				driveLabels[l] = "DRIVE";
				girthLabels[l] = "CHAR";
				modLabels[l]   = "TYPE";
				biasLabels[l]  = "BIAS";
				reactLabels[l] = "COMP";
				driveShortLabels[l] = "DRIVE";
				girthShortLabels[l] = "CHAR";
				modShortLabels[l]   = "TYPE";
				reactShortLabels[l] = "COMP";
				break;
			case 4:
				girthLabels[l] = "CHAR";
				modLabels[l]   = "TYPE";
				biasLabels[l]  = "SYM";
				reactLabels[l] = "REACT";
				girthShortLabels[l] = "CHAR";
				modShortLabels[l]   = "TYPE";
				biasShortLabels[l]  = "SYM";
				reactShortLabels[l] = "REACT";
				break;
			case 5:
			case 8:
				driveLabels[l] = "DRIVE";
				girthLabels[l] = "KNEE";
				modLabels[l]   = "TYPE";
				biasLabels[l]  = "SYM";
				reactLabels[l] = "PEAK";
				girthShortLabels[l] = "KNEE";
				modShortLabels[l]   = "TYPE";
				biasShortLabels[l]  = "SYM";
				reactShortLabels[l] = "PEAK";
				break;
			case 6:
				driveLabels[l] = "DRIVE";
				girthLabels[l] = "KNEE";
				modLabels[l]   = "TYPE";
				biasLabels[l]  = "SYM";
				reactLabels[l] = "PEAK";
				girthShortLabels[l] = "KNEE";
				modShortLabels[l]   = "TYPE";
				biasShortLabels[l]  = "SYM";
				reactShortLabels[l] = "PEAK";
				break;
			default:
				reactLabels[l] = "DYN";
				reactShortLabels[l] = "DYN";
				break; // Clean/unknown
		}
	}

	ParamFmt fmts[kNumCachedParams] = {
		{0,"HP"}, {0,"LP"}, {1,"IN"}, {1,"OUT"}, {5,"TILT"}, {7,"SERIES"},
		{4,"PAN"}, {3,"ANGLE"}, {3,"DIST"}, {3,"MIX"},
		{3,"DRIVE"}, {3,"SIZE"}, {3,"CHAR"}, {3,"TYPE"}, {6,"BIAS"}, {3,"DYN"}, {3,"DETAIL"}, {3,"INST"}, {8,"DELAY"}
	};

	for (int loader = 0; loader < 3; ++loader)
	{
		fmts[10].label = driveLabels[loader];
		fmts[12].label = girthLabels[loader];
		fmts[13].label = modLabels[loader];
		fmts[14].label = biasLabels[loader];
		fmts[15].label = reactLabels[loader];
		fmts[10].shortLabel = driveShortLabels[loader];
		fmts[12].shortLabel = girthShortLabels[loader];
		fmts[13].shortLabel = modShortLabels[loader];
		fmts[14].shortLabel = biasShortLabels[loader];
		fmts[15].shortLabel = reactShortLabels[loader];

		auto refs = getLoaderRefs (loader);
		const bool loaderEnabled = refs.enableBtn.getToggleState();
		const int selectedModel = getSelectedSatTypeModelIndex (refs.satType);
		const bool cleanModel = (selectedModel == static_cast<int> (SatEngine::Model::Clean));
		const bool namModel = (selectedModel == static_cast<int> (SatEngine::Model::NAM));
		const bool namReady = ! namModel || audioProcessor.isNamModelLoadedForLoader (loader);
		juce::Slider* loaderSliders[kNumCachedParams] = {
			&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt, &refs.series,
			&refs.pan, &refs.fred, &refs.pos, &refs.mix,
			&refs.satDrive, &refs.namSlim, &refs.satChar, &refs.satTypeCtrl, &refs.satBias, &refs.satSag, &refs.detail, &refs.instability, &refs.offset
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
					const auto shortLabel = fmt.shortLabel != nullptr ? fmt.shortLabel : fmt.label;
					ct.full    = juce::String (pct) + "% " + fmt.label;
					ct.short_  = juce::String (pct) + "% " + shortLabel;
					ct.intOnly = juce::String (pct) + "%";
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
					ct.short_  = juce::String (pct) + "% " + (fmt.shortLabel != nullptr ? fmt.shortLabel : fmt.label);
					ct.intOnly = juce::String (pct) + "%";
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
				case 8: // Offset ms (decimal)
				{
					const auto timeText = formatSatOffsetMsForUi (val);
					ct.full    = timeText + " " + fmt.label;
					ct.short_  = timeText + " " + (fmt.shortLabel != nullptr ? fmt.shortLabel : fmt.label);
					ct.intOnly = formatSatOffsetMsNumberForUi (val);
					break;
				}
			}

			const bool cleanSatParameterIrrelevant = cleanModel && (p == 5 || (p >= 10 && p <= 17));
			const bool namSatParameterIrrelevant = namModel && (p == 5 || (p >= 12 && p <= 17));
			const bool satParameterIrrelevant = cleanSatParameterIrrelevant || namSatParameterIrrelevant;
			if (! loaderEnabled || ! namReady || satParameterIrrelevant)
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
	return TR::getLoaderMixText (*this, dualMixBar_, globalMixSlider, mixModeCombo);
}

juce::String SATTRAudioProcessorEditor::getMixTextShort() const
{
	return TR::getLoaderMixTextShort (*this, dualMixBar_, globalMixSlider, mixModeCombo);
}

juce::Rectangle<int> SATTRAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds,
                                                                  int columnRight) const
{
	return TR::getLoaderValueAreaFor (barBounds, columnRight, 8, 24, 6);
}

juce::Slider* SATTRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
	for (int i = 0; i < 3; ++i)
	{
		auto r = getLoaderRefs (i);
		const int colR = columnRight_[i];

		BarSlider* sliders[] = { &r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		                         &r.pan, &r.fred, &r.pos,
		                         &r.satDrive, &r.namSlim, &r.satChar, &r.satTypeCtrl, &r.satBias, &r.satSag,
		                         &r.detail, &r.instability, &r.offset };

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
	return TR::getLoaderInfoIconArea (getWidth(), 32, 10);
}

void SATTRAudioProcessorEditor::updateInfoIconCache()
{
	TR::updateLoaderInfoIconCache (cachedInfoGearPath, cachedInfoGearHole, getWidth(), 32, 10);
}

// ----------------------------------------------------------------
void SATTRAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
	TR::setLoaderPromptOverlayActive (*this, promptOverlay, promptOverlayActive, shouldBeActive, lnf,
		[this]
	{
		updateLoaderEnabledState (0);
		updateLoaderEnabledState (1);
		updateLoaderEnabledState (2);
		resized();
	});
}

void SATTRAudioProcessorEditor::moved()
{
	TR::anchorLoaderPromptsOnMove (*this, promptOverlayActive, promptOverlay, lnf);
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

	// -- Suffix determination via slider type --
	juce::String suffix;
	juce::String suffixShort;
	auto* bar = dynamic_cast<BarSlider*> (&s);
	const auto stype = bar ? bar->getType() : BarSliderType::Unknown;

	const bool isHp    = (stype == BarSliderType::HpFreq);
	const bool isLp    = (stype == BarSliderType::LpFreq);
	const bool isHpLp  = (isHp || isLp);
	const bool isIn    = (stype == BarSliderType::Input);
	const bool isOut   = (stype == BarSliderType::Output || stype == BarSliderType::GlobalOutput);
	const bool isLimThresh = (stype == BarSliderType::LimThreshold);
	const bool isTilt  = (stype == BarSliderType::Tilt);
	const bool isSeries = (stype == BarSliderType::Series);
	const bool isDetail = (stype == BarSliderType::Detail);
	const bool isInstability   = (stype == BarSliderType::Instability);
	const bool isOffset = (stype == BarSliderType::Offset);
	const bool isPan   = (stype == BarSliderType::Pan);
	const bool isFred  = (stype == BarSliderType::Fred);
	const bool isPos   = (stype == BarSliderType::Pos);
	const bool isMix   = (stype == BarSliderType::Mix || stype == BarSliderType::GlobalMix);
	const bool isSatDrive = (stype == BarSliderType::SatDrive);
	const bool isNamSlim = (stype == BarSliderType::NamSlim);
	const bool isSatChar = (stype == BarSliderType::SatChar);
	const bool isSatTypeCtrl   = (stype == BarSliderType::SatTypeCtrl);
	const bool isSatBias  = (stype == BarSliderType::SatBias);
	const bool isSatSag   = (stype == BarSliderType::SatSag);
	const bool isSatPct   = (isSatDrive || isNamSlim || isSatChar || isSatTypeCtrl || isSatSag);
	const bool isSatBiPct = isSatBias;

	if (isSeries)
		return;

	auto getSatPromptLabel = [this, &s, stype]() -> juce::String
	{
		int loaderIndex = -1;
		for (int i = 0; i < 3 && loaderIndex < 0; ++i)
		{
			auto refs = getLoaderRefs (i);
			if ((stype == BarSliderType::SatDrive && &refs.satDrive == &s)
			 || (stype == BarSliderType::NamSlim && &refs.namSlim == &s)
			 || (stype == BarSliderType::SatChar && &refs.satChar == &s)
			 || (stype == BarSliderType::SatTypeCtrl   && &refs.satTypeCtrl   == &s)
			 || (stype == BarSliderType::SatBias  && &refs.satBias  == &s)
			 || (stype == BarSliderType::SatSag   && &refs.satSag   == &s))
				loaderIndex = i;
		}

		if (loaderIndex < 0)
		{
			switch (stype)
			{
				case BarSliderType::SatDrive: return "DRIVE";
				case BarSliderType::NamSlim:  return "SIZE";
				case BarSliderType::SatChar: return "CHAR";
				case BarSliderType::SatTypeCtrl:   return "TYPE";
				case BarSliderType::SatBias:  return "BIAS";
				case BarSliderType::SatSag:   return "DYN";
				default:                        return {};
			}
		}

		auto lr = getLoaderRefs (loaderIndex);
		const int satModel = getSelectedSatTypeModelIndex (lr.satType);
		juce::String driveLabel = "DRIVE";
		juce::String girthLabel = "CHAR";
		juce::String modLabel   = "TYPE";
		juce::String biasLabel  = "BIAS";
		juce::String reactLabel = "DYN";

		switch (satModel)
		{
			case 1: girthLabel = "CHAR"; modLabel = "TYPE"; reactLabel = "COMP"; break;
			case 2: girthLabel = "CHAR"; modLabel = "TYPE"; reactLabel = "SAG";  break;
			case 3: driveLabel = "DRIVE"; girthLabel = "CHAR"; modLabel = "TYPE"; reactLabel = "COMP"; break;
			case 4: girthLabel = "CHAR"; modLabel = "TYPE"; biasLabel = "SYM"; reactLabel = "REACT"; break;
			case 5: driveLabel = "DRIVE"; girthLabel = "KNEE"; modLabel = "TYPE"; biasLabel = "SYM"; reactLabel = "PEAK"; break;
			case 8: driveLabel = "DRIVE"; girthLabel = "KNEE"; modLabel = "TYPE"; biasLabel = "SYM"; reactLabel = "PEAK"; break;
			case 6: driveLabel = "DRIVE"; girthLabel = "KNEE"; modLabel = "TYPE"; biasLabel = "SYM"; reactLabel = "PEAK"; break;
			default: break;
		}

		switch (stype)
		{
			case BarSliderType::SatDrive: return driveLabel;
			case BarSliderType::NamSlim:  return "SIZE";
			case BarSliderType::SatChar: return girthLabel;
			case BarSliderType::SatTypeCtrl:   return modLabel;
			case BarSliderType::SatBias:  return biasLabel;
			case BarSliderType::SatSag:   return reactLabel;
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
	else if (isDetail)      { suffix = " % DTL";    suffixShort = " % DTL"; }
	else if (isInstability) { suffix = " % INST";      suffixShort = " % INST"; }
	else if (isOffset)       { suffix = "ms";          suffixShort = "ms"; }
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

	// -- Initial display value --
	juce::String currentDisplay;
	if (isHpLp)
		currentDisplay = juce::String (juce::jlimit (20.0, 20000.0, s.getValue()), 2);
	else if (isIn || isOut)
		currentDisplay = juce::String (juce::roundToInt (s.getValue()));
	else if (isLimThresh)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isTilt)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isDetail || isInstability)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else if (isOffset)
		currentDisplay = formatTimeMsForPromptValue (juce::jlimit (0.0, (double) SATTRAudioProcessor::kOffsetMax, s.getValue()));
	else if (isPan)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
	else if (isSatBias)
		currentDisplay = juce::String (juce::jlimit (-100.0, 100.0, s.getValue() * 100.0), 2);
	else if (isNamSlim)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
	else if (isSatPct)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else if (isFred || isPos || isMix)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 2);
	else
		currentDisplay = s.getTextFromValue (s.getValue());

	// -- Per-slider input constraints --
	double minVal = 0.0, maxVal = 1.0;
	int maxLen = 0, maxDecs = 4;

	if (isHpLp)
	{
		minVal = 20.0;   maxVal = 20000.0;
		maxDecs = 2;     maxLen = 8;     // "20000.00"
	}
	else if (isIn || isOut)
	{
		minVal = SATTRAudioProcessor::kGainFloorDb;
		maxVal = SATTRAudioProcessor::kGainMaxDb;
		maxDecs = 0;     maxLen = 4;     // "-144"
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
	else if (isOffset)
	{
		minVal = SATTRAudioProcessor::kOffsetMin;
		maxVal = SATTRAudioProcessor::kOffsetMax;
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

	TR::NumericEntryPromptSpec spec;
	spec.title = suffixTextShort.isNotEmpty() ? suffixTextShort : "VALUE";
	spec.label = spec.title;
	spec.unit = suffixTextShort;
	spec.currentDisplay = currentDisplay;
	spec.currentValue = s.getValue();
	spec.minValue = minVal;
	spec.maxValue = maxVal;
	spec.maxLength = maxLen;
	spec.maxDecimals = maxDecs;

	juce::Component::SafePointer<SATTRAudioProcessorEditor> safeThis (this);
	juce::Slider* sliderPtr = &s;
	spec.onAccept = [safeThis, sliderPtr] (const juce::String& txt) mutable
	{
		if (safeThis == nullptr || sliderPtr == nullptr)
			return;

		auto normalised = txt.trim().replaceCharacter (',', '.');
		juce::String t = normalised.trimStart();
		while (t.startsWithChar ('+'))
			t = t.substring (1).trimStart();
		const juce::String numericToken = t.initialSectionContainingOnly ("0123456789.,-");

		// Percent-based sliders: user typed 0..100/200, slider stores 0..1/2.
		auto* barPtr = dynamic_cast<BarSlider*> (sliderPtr);
		const auto st = barPtr ? barPtr->getType() : BarSliderType::Unknown;
		const bool isGainFader = (st == BarSliderType::Input ||
		                          st == BarSliderType::Output ||
		                          st == BarSliderType::GlobalOutput);
		double v = (isGainFader && t.containsIgnoreCase ("inf"))
			? (double) SATTRAudioProcessor::kGainFloorDb
			: numericToken.getDoubleValue();

		const bool needsPercentConvert = (st == BarSliderType::Pan ||
		                                  st == BarSliderType::Fred  || st == BarSliderType::Pos  ||
		                                  st == BarSliderType::Mix  ||
		                                  st == BarSliderType::SatDrive || st == BarSliderType::NamSlim ||
		                                  st == BarSliderType::SatChar ||
		                                  st == BarSliderType::SatTypeCtrl   || st == BarSliderType::SatBias  ||
		                                  st == BarSliderType::SatSag   ||
		                                  st == BarSliderType::Detail ||
		                                  st == BarSliderType::Instability ||
		                                  st == BarSliderType::GlobalMix);

		if (needsPercentConvert)
			v *= 0.01;

		const auto range = sliderPtr->getRange();
		sliderPtr->setValue (juce::jlimit (range.getStart(), range.getEnd(), v), juce::sendNotificationSync);
	};

	TR::openNumericEntryPopupShared (this, lnf, activeScheme, std::move (spec));
}

// ----------------------------------------------------------------
//  MIX SEND prompt (DRY + WET levels)
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::openMixSendPrompt()
{
	auto& vts = audioProcessor.getValueTreeState();
	auto linearToDb = [] (float gain) -> float
	{
		return gain <= 0.0001f ? -100.0f : 20.0f * std::log10 (juce::jlimit (0.0001f, 1.0f, gain));
	};
	auto dbToLinear = [] (float db) -> float
	{
		return db <= -100.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, std::pow (10.0f, db / 20.0f));
	};

	std::vector<TR::SimpleRowsPromptContent::Row> rows;
	rows.push_back ({ "DRY", "dB", -100.0f, 0.0f,
	                  linearToDb (TR::getParameterPlain (vts, SATTRAudioProcessor::kParamDryLevel, SATTRAudioProcessor::kDryLevelDefault)),
	                  linearToDb (SATTRAudioProcessor::kDryLevelDefault), 1,
	                  [&vts, dbToLinear] (float value) { TR::setParameterPlain (vts, SATTRAudioProcessor::kParamDryLevel, dbToLinear (value)); } });
	rows.push_back ({ "WET", "dB", -100.0f, 0.0f,
	                  linearToDb (TR::getParameterPlain (vts, SATTRAudioProcessor::kParamWetLevel, SATTRAudioProcessor::kWetLevelDefault)),
	                  linearToDb (SATTRAudioProcessor::kWetLevelDefault), 1,
	                  [&vts, dbToLinear] (float value) { TR::setParameterPlain (vts, SATTRAudioProcessor::kParamWetLevel, dbToLinear (value)); } });

	TR::openRowsPromptShared (this, lnf, activeScheme, "MIX/SEND", std::move (rows));
	dualMixBar_.updateFromProcessor();
	refreshLegendTextCache();
	repaint();
}


// ----------------------------------------------------------------
//  FILTER prompt (HP + LP frequencies, on/off, slope)
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::openFilterPrompt (int loaderIndex)
{
	loaderIndex = juce::jlimit (0, 2, loaderIndex);
	auto pickId = [&] (const char* a, const char* b, const char* c) -> const char*
	{
		return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
	};

	TR::FilterPromptSpec spec;
	spec.hpParam = pickId (SATTRAudioProcessor::kParamHpFreqA,  SATTRAudioProcessor::kParamHpFreqB,  SATTRAudioProcessor::kParamHpFreqC);
	spec.lpParam = pickId (SATTRAudioProcessor::kParamLpFreqA,  SATTRAudioProcessor::kParamLpFreqB,  SATTRAudioProcessor::kParamLpFreqC);
	spec.hpOnParam = pickId (SATTRAudioProcessor::kParamHpOnA, SATTRAudioProcessor::kParamHpOnB, SATTRAudioProcessor::kParamHpOnC);
	spec.lpOnParam = pickId (SATTRAudioProcessor::kParamLpOnA, SATTRAudioProcessor::kParamLpOnB, SATTRAudioProcessor::kParamLpOnC);
	spec.hpSlopeParam = pickId (SATTRAudioProcessor::kParamHpSlopeA, SATTRAudioProcessor::kParamHpSlopeB, SATTRAudioProcessor::kParamHpSlopeC);
	spec.lpSlopeParam = pickId (SATTRAudioProcessor::kParamLpSlopeA, SATTRAudioProcessor::kParamLpSlopeB, SATTRAudioProcessor::kParamLpSlopeC);
	spec.freqMin = SATTRAudioProcessor::kFilterFreqMin;
	spec.freqMax = SATTRAudioProcessor::kFilterFreqMax;
	spec.hpDefault = SATTRAudioProcessor::kFilterHpFreqDefault;
	spec.lpDefault = SATTRAudioProcessor::kFilterLpFreqDefault;
	spec.slopeMin = SATTRAudioProcessor::kFilterSlopeMin;
	spec.slopeMax = SATTRAudioProcessor::kFilterSlopeMax;
	spec.refreshFilterDisplay = [this, loaderIndex]
	{
		getLoaderRefs (loaderIndex).filterBar.updateFromProcessor();
		refreshLegendTextCache();
		repaint();
	};

	TR::openFilterPromptShared (this, lnf, activeScheme, audioProcessor.getValueTreeState(), spec);
}



void SATTRAudioProcessorEditor::openSidechainPrompt (int loaderIndex)
{
	loaderIndex = juce::jlimit (0, 2, loaderIndex);
	const auto& ids = kLoaderParams[loaderIndex];
	auto& vts = audioProcessor.getValueTreeState();

	std::vector<TR::SimpleRowsPromptContent::Row> rows;
	rows.push_back ({ "GAIN", "dB", SATTRAudioProcessor::kSidechainGainMin, SATTRAudioProcessor::kSidechainGainMax,
	                  TR::getParameterPlain (vts, ids.sidechainGain, SATTRAudioProcessor::kSidechainGainDefault),
	                  SATTRAudioProcessor::kSidechainGainDefault, 0,
	                  [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainGain, value); } });
	rows.push_back ({ "SMTH", "%", SATTRAudioProcessor::kSidechainSmoothMin * 100.0f, SATTRAudioProcessor::kSidechainSmoothMax * 100.0f,
	                  TR::getParameterPlain (vts, ids.sidechainSmooth, SATTRAudioProcessor::kSidechainSmoothDefault) * 100.0f,
	                  SATTRAudioProcessor::kSidechainSmoothDefault * 100.0f, 0,
	                  [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainSmooth, value / 100.0f); } });

	{
		TR::SimpleRowsPromptContent::Row row;
		row.label = "";
		row.min = 0.0f;
		row.max = 1.0f;
		row.value = TR::getParameterPlain (vts, ids.sidechainHpOn, SATTRAudioProcessor::kSidechainHpOnDefault ? 1.0f : 0.0f);
		row.def = SATTRAudioProcessor::kSidechainHpOnDefault ? 1.0f : 0.0f;
		row.kind = TR::SimpleRowKind::checkboxWithToggle;
		row.choices = { "HP", "6dB/oct", "12dB/oct", "24dB/oct" };
		row.toggleValue = TR::getParameterPlain (vts, ids.sidechainHpSlope, (float) SATTRAudioProcessor::kSidechainHpSlopeDefault);
		row.apply = [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainHpOn, value >= 0.5f ? 1.0f : 0.0f); };
		row.applyToggle = [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainHpSlope, (float) juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin, SATTRAudioProcessor::kFilterSlopeMax, juce::roundToInt (value))); };
		rows.push_back (std::move (row));
	}
	rows.push_back ({ "HP FREQ", "Hz", SATTRAudioProcessor::kSidechainFilterFreqMin, SATTRAudioProcessor::kSidechainFilterFreqMax,
	                  TR::getParameterPlain (vts, ids.sidechainHp, SATTRAudioProcessor::kSidechainHpDefault),
	                  SATTRAudioProcessor::kSidechainHpDefault, 0,
	                  [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainHp, value); } });

	{
		TR::SimpleRowsPromptContent::Row row;
		row.label = "";
		row.min = 0.0f;
		row.max = 1.0f;
		row.value = TR::getParameterPlain (vts, ids.sidechainLpOn, SATTRAudioProcessor::kSidechainLpOnDefault ? 1.0f : 0.0f);
		row.def = SATTRAudioProcessor::kSidechainLpOnDefault ? 1.0f : 0.0f;
		row.kind = TR::SimpleRowKind::checkboxWithToggle;
		row.choices = { "LP", "6dB/oct", "12dB/oct", "24dB/oct" };
		row.toggleValue = TR::getParameterPlain (vts, ids.sidechainLpSlope, (float) SATTRAudioProcessor::kSidechainLpSlopeDefault);
		row.apply = [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainLpOn, value >= 0.5f ? 1.0f : 0.0f); };
		row.applyToggle = [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainLpSlope, (float) juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin, SATTRAudioProcessor::kFilterSlopeMax, juce::roundToInt (value))); };
		rows.push_back (std::move (row));
	}
	rows.push_back ({ "LP FREQ", "Hz", SATTRAudioProcessor::kSidechainFilterFreqMin, SATTRAudioProcessor::kSidechainFilterFreqMax,
	                  TR::getParameterPlain (vts, ids.sidechainLp, SATTRAudioProcessor::kSidechainLpDefault),
	                  SATTRAudioProcessor::kSidechainLpDefault, 0,
	                  [&vts, ids] (float value) { TR::setParameterPlain (vts, ids.sidechainLp, value); } });

	TR::openRowsPromptShared (this, lnf, activeScheme, "SIDECHAIN", std::move (rows));
	refreshLegendTextCache();
	repaint();
}


// ----------------------------------------------------------------
//  CHAOS prompt (AMOUNT + SPEED)
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::openChaosPrompt (int loaderIndex, bool isFilter)
{
	loaderIndex = juce::jlimit (0, 2, loaderIndex);
	const auto& ids = kLoaderParams[loaderIndex];
	auto& vts = audioProcessor.getValueTreeState();

	TR::SimpleChaosPromptBinding binding;
	binding.amountParamId = isFilter ? ids.chaosAmtFilter : ids.chaosAmt;
	binding.speedParamId = isFilter ? ids.chaosSpdFilter : ids.chaosSpd;

	TR::openSimpleChaosPromptAction (this, lnf, activeScheme, vts, binding,
		[this]
		{
			refreshLegendTextCache();
			repaint();
		});
}


// ----------------------------------------------------------------
//  EXP Prompt - right-click on EXP button opens ORDER/RATIO/THRESHOLD
// ----------------------------------------------------------------

void SATTRAudioProcessorEditor::openExpPrompt (int loaderIndex)
{
	loaderIndex = juce::jlimit (0, 2, loaderIndex);
	auto& vts = audioProcessor.getValueTreeState();
	auto pickId = [&] (const char* a, const char* b, const char* c) -> const char*
	{
		return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
	};

	const char* orderParamId = pickId (SATTRAudioProcessor::kParamExpOrderA, SATTRAudioProcessor::kParamExpOrderB, SATTRAudioProcessor::kParamExpOrderC);
	const char* ratioParamId = pickId (SATTRAudioProcessor::kParamExpRatioA, SATTRAudioProcessor::kParamExpRatioB, SATTRAudioProcessor::kParamExpRatioC);
	const char* threshParamId = pickId (SATTRAudioProcessor::kParamExpThreshA, SATTRAudioProcessor::kParamExpThreshB, SATTRAudioProcessor::kParamExpThreshC);
	const char* kneeParamId = pickId (SATTRAudioProcessor::kParamExpKneeA, SATTRAudioProcessor::kParamExpKneeB, SATTRAudioProcessor::kParamExpKneeC);
	const char* atkParamId = pickId (SATTRAudioProcessor::kParamExpAtkA, SATTRAudioProcessor::kParamExpAtkB, SATTRAudioProcessor::kParamExpAtkC);
	const char* relParamId = pickId (SATTRAudioProcessor::kParamExpRelA, SATTRAudioProcessor::kParamExpRelB, SATTRAudioProcessor::kParamExpRelC);
	const char* scHpParamId = pickId (SATTRAudioProcessor::kParamExpScHpA, SATTRAudioProcessor::kParamExpScHpB, SATTRAudioProcessor::kParamExpScHpC);
	const char* scLpParamId = pickId (SATTRAudioProcessor::kParamExpScLpA, SATTRAudioProcessor::kParamExpScLpB, SATTRAudioProcessor::kParamExpScLpC);
	const char* scHpOnParamId = pickId (SATTRAudioProcessor::kParamExpScHpOnA, SATTRAudioProcessor::kParamExpScHpOnB, SATTRAudioProcessor::kParamExpScHpOnC);
	const char* scLpOnParamId = pickId (SATTRAudioProcessor::kParamExpScLpOnA, SATTRAudioProcessor::kParamExpScLpOnB, SATTRAudioProcessor::kParamExpScLpOnC);
	const char* scHpSlopeParamId = pickId (SATTRAudioProcessor::kParamExpScHpSlopeA, SATTRAudioProcessor::kParamExpScHpSlopeB, SATTRAudioProcessor::kParamExpScHpSlopeC);
	const char* scLpSlopeParamId = pickId (SATTRAudioProcessor::kParamExpScLpSlopeA, SATTRAudioProcessor::kParamExpScLpSlopeB, SATTRAudioProcessor::kParamExpScLpSlopeC);
	const char* scGainParamId = pickId (SATTRAudioProcessor::kParamExpScGainA, SATTRAudioProcessor::kParamExpScGainB, SATTRAudioProcessor::kParamExpScGainC);

	std::vector<TR::SimpleRowsPromptContent::Row> rows;
	{
		TR::SimpleRowsPromptContent::Row row;
		row.label = "ORDER";
		row.min = 0.0f;
		row.max = 1.0f;
		row.value = TR::getParameterPlain (vts, orderParamId, 0.0f);
		row.def = 0.0f;
		row.kind = TR::SimpleRowKind::toggle2;
		row.choices = { "PRE", "POST" };
		row.apply = [&vts, orderParamId] (float value) { TR::setParameterPlain (vts, orderParamId, (float) juce::roundToInt (value)); };
		rows.push_back (std::move (row));
	}

	rows.push_back ({ "RATIO", "", SATTRAudioProcessor::kExpRatioMin, SATTRAudioProcessor::kExpRatioMax,
	                  TR::getParameterPlain (vts, ratioParamId, SATTRAudioProcessor::kExpRatioDefault),
	                  SATTRAudioProcessor::kExpRatioDefault, 1,
	                  [&vts, ratioParamId] (float value) { TR::setParameterPlain (vts, ratioParamId, value); } });
	rows.push_back ({ "THR", "dB", SATTRAudioProcessor::kExpThreshMin, SATTRAudioProcessor::kExpThreshMax,
	                  TR::getParameterPlain (vts, threshParamId, SATTRAudioProcessor::kExpThreshDefault),
	                  SATTRAudioProcessor::kExpThreshDefault, 1,
	                  [&vts, threshParamId] (float value) { TR::setParameterPlain (vts, threshParamId, value); } });
	rows.push_back ({ "KNEE", "dB", SATTRAudioProcessor::kExpKneeMin, SATTRAudioProcessor::kExpKneeMax,
	                  TR::getParameterPlain (vts, kneeParamId, SATTRAudioProcessor::kExpKneeDefault),
	                  SATTRAudioProcessor::kExpKneeDefault, 1,
	                  [&vts, kneeParamId] (float value) { TR::setParameterPlain (vts, kneeParamId, value); } });
	rows.push_back ({ "ATK", "ms", SATTRAudioProcessor::kExpAtkMin, SATTRAudioProcessor::kExpAtkMax,
	                  TR::getParameterPlain (vts, atkParamId, SATTRAudioProcessor::kExpAtkDefault),
	                  SATTRAudioProcessor::kExpAtkDefault, 1,
	                  [&vts, atkParamId] (float value) { TR::setParameterPlain (vts, atkParamId, value); } });
	rows.push_back ({ "RLS", "ms", SATTRAudioProcessor::kExpRelMin, SATTRAudioProcessor::kExpRelMax,
	                  TR::getParameterPlain (vts, relParamId, SATTRAudioProcessor::kExpRelDefault),
	                  SATTRAudioProcessor::kExpRelDefault, 0,
	                  [&vts, relParamId] (float value) { TR::setParameterPlain (vts, relParamId, value); } });
	rows.push_back ({ "SC GAIN", "dB", SATTRAudioProcessor::kExpScGainMin, SATTRAudioProcessor::kExpScGainMax,
	                  TR::getParameterPlain (vts, scGainParamId, SATTRAudioProcessor::kExpScGainDefault),
	                  SATTRAudioProcessor::kExpScGainDefault, 1,
	                  [&vts, scGainParamId] (float value) { TR::setParameterPlain (vts, scGainParamId, value); } });

	{
		TR::SimpleRowsPromptContent::Row row;
		row.label = "";
		row.min = 0.0f;
		row.max = 1.0f;
		row.value = TR::getParameterPlain (vts, scHpOnParamId, SATTRAudioProcessor::kExpScHpOnDefault ? 1.0f : 0.0f);
		row.def = SATTRAudioProcessor::kExpScHpOnDefault ? 1.0f : 0.0f;
		row.kind = TR::SimpleRowKind::checkboxWithToggle;
		row.choices = { "HP", "6dB/oct", "12dB/oct", "24dB/oct" };
		row.toggleValue = TR::getParameterPlain (vts, scHpSlopeParamId, (float) SATTRAudioProcessor::kExpScHpSlopeDefault);
		row.apply = [&vts, scHpOnParamId] (float value) { TR::setParameterPlain (vts, scHpOnParamId, value >= 0.5f ? 1.0f : 0.0f); };
		row.applyToggle = [&vts, scHpSlopeParamId] (float value) { TR::setParameterPlain (vts, scHpSlopeParamId, (float) juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin, SATTRAudioProcessor::kFilterSlopeMax, juce::roundToInt (value))); };
		rows.push_back (std::move (row));
	}
	rows.push_back ({ "SC HP", "Hz", SATTRAudioProcessor::kExpScFreqMin, SATTRAudioProcessor::kExpScFreqMax,
	                  TR::getParameterPlain (vts, scHpParamId, SATTRAudioProcessor::kExpScHpDefault),
	                  SATTRAudioProcessor::kExpScHpDefault, 0,
	                  [&vts, scHpParamId] (float value) { TR::setParameterPlain (vts, scHpParamId, value); } });

	{
		TR::SimpleRowsPromptContent::Row row;
		row.label = "";
		row.min = 0.0f;
		row.max = 1.0f;
		row.value = TR::getParameterPlain (vts, scLpOnParamId, SATTRAudioProcessor::kExpScLpOnDefault ? 1.0f : 0.0f);
		row.def = SATTRAudioProcessor::kExpScLpOnDefault ? 1.0f : 0.0f;
		row.kind = TR::SimpleRowKind::checkboxWithToggle;
		row.choices = { "LP", "6dB/oct", "12dB/oct", "24dB/oct" };
		row.toggleValue = TR::getParameterPlain (vts, scLpSlopeParamId, (float) SATTRAudioProcessor::kExpScLpSlopeDefault);
		row.apply = [&vts, scLpOnParamId] (float value) { TR::setParameterPlain (vts, scLpOnParamId, value >= 0.5f ? 1.0f : 0.0f); };
		row.applyToggle = [&vts, scLpSlopeParamId] (float value) { TR::setParameterPlain (vts, scLpSlopeParamId, (float) juce::jlimit (SATTRAudioProcessor::kFilterSlopeMin, SATTRAudioProcessor::kFilterSlopeMax, juce::roundToInt (value))); };
		rows.push_back (std::move (row));
	}
	rows.push_back ({ "SC LP", "Hz", SATTRAudioProcessor::kExpScFreqMin, SATTRAudioProcessor::kExpScFreqMax,
	                  TR::getParameterPlain (vts, scLpParamId, SATTRAudioProcessor::kExpScLpDefault),
	                  SATTRAudioProcessor::kExpScLpDefault, 0,
	                  [&vts, scLpParamId] (float value) { TR::setParameterPlain (vts, scLpParamId, value); } });

	TR::openRowsPromptShared (this, lnf, activeScheme, "EXP", std::move (rows));
	refreshLegendTextCache();
	repaint();
}


//==============================================================================
//  Info Prompt
//==============================================================================
void SATTRAudioProcessorEditor::openInfoPopup()
{
	TR::openInfoPopupFromXmlShared (this, lnf, activeScheme, InfoContent::xml,
		[this] { openGraphicsPopup(); });
}

void SATTRAudioProcessorEditor::openGraphicsPopup()
{
	paletteState.useCustom = audioProcessor.getUiUseCustomPalette();
	ioFxEnabled = audioProcessor.getUiIoFxEnabled();
	for (int i = 0; i < TR::LoaderPaletteState::colourCount; ++i)
		paletteState.customColours[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);
	applyCrtState (audioProcessor.getUiFxTailEnabled());
	refreshActivePalette();

	TR::openGraphicsPopupShared (this, lnf, activeScheme,
		paletteState.defaultColours, paletteState.customColours, paletteState.useCustom, ioFxEnabled,
		[this] (bool enabled)
		{
			paletteState.useCustom = enabled;
			audioProcessor.setUiUseCustomPalette (enabled);
			refreshActivePalette();
			repaint();
		},
		[this] (int index, juce::Colour colour)
		{
			if (index < 0 || index >= TR::LoaderPaletteState::colourCount)
				return;
			paletteState.customColours[(size_t) index] = colour;
			audioProcessor.setUiCustomPaletteColour (index, colour);
			refreshActivePalette();
			repaint();
		},
		[this] (bool enabled)
		{
			ioFxEnabled = enabled;
			audioProcessor.setUiIoFxEnabled (enabled);
			stopTimer();
			startTimerHz (crtEnabled ? kCrtTimerHz : (ioFxEnabled ? kMeterTimerHz : kIdleTimerHz));
			updateIoFxMeterSliders();
			repaint();
		},
		[this]
		{
			refreshActivePalette();
			repaint();
		});
}

void SATTRAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
	label.setColour (juce::Label::textColourId, colour);
}

#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "../../TR-Shared/LoaderUI/TRLoaderUI.h"
#include "CrtEffect.h"

// Local aliases for shared types
using PromptToggleButton = TR::PromptToggleButton;

class SATTRAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Slider::Listener,
                                   private juce::Button::Listener,
                                   private juce::ComboBox::Listener,
                                   private juce::AudioProcessorValueTreeState::Listener,
                                   private juce::Timer
{
public:
	// Shared template access
	template <typename, typename> friend class TR::LoaderFilterBarComponent;
	template <typename, typename> friend class TR::LoaderDualMixBarComponent;

	explicit SATTRAudioProcessorEditor (SATTRAudioProcessor&);
	~SATTRAudioProcessorEditor() override;

	void paint (juce::Graphics&) override;
	void paintOverChildren (juce::Graphics&) override;
	void resized() override;
	void moved() override;
	void parentHierarchyChanged() override;

private:
	void mouseDown (const juce::MouseEvent& e) override;
	void mouseDoubleClick (const juce::MouseEvent& e) override;
	void mouseDrag (const juce::MouseEvent& e) override;
	void mouseMove (const juce::MouseEvent& e) override;
	void mouseExit (const juce::MouseEvent& e) override;

	void timerCallback() override;
	void sliderValueChanged (juce::Slider* slider) override;
	void buttonClicked (juce::Button* button) override;
	void comboBoxChanged (juce::ComboBox* combo) override;
	void parameterChanged (const juce::String& paramID, float newValue) override;

	void openNumericEntryPopupForSlider (juce::Slider& s);
	void openInfoPopup();
	void openGraphicsPopup();
	void openChaosPrompt (int loaderIndex, bool isFilter);
	void openExpPrompt (int loaderIndex);
	void openFilterPrompt (int loaderIndex);
	void openSidechainPrompt (int loaderIndex);
	void openNamFileChooser (int loaderIndex);
	void loadNamFileFromPath (int loaderIndex, const juce::String& path);
	void updateNamFileDisplays();
	void openMixSendPrompt();
	void applyLabelTextColour (juce::Label& label, juce::Colour colour);
	void layoutLoaderSection (juce::Rectangle<int> area, int loaderIndex);
	int getCompactTargetWidthForLoaderCount (int loaderCount) noexcept;
	int getMaxVisibleLoaderCountForWidth (int width) noexcept;
	void clearCompactRailAreas() noexcept;
	void setVisibleLoaderCount (int loaderCount, bool requestResize);
	void setFirstVisibleLoaderIndex (int loaderIndex);
	void syncSingleLoaderIoExpandedState (bool expanded);
	void setFooterExpanded (bool shouldBeExpanded);
	void hideLoaderSection (int loaderIndex);
	void hideFooterControls();
	void layoutFooterControls (juce::Rectangle<int> area);
	juce::Rectangle<int> getTitleHitArea() const;
	juce::String getSafeClipTooltip() const;
	void showSafeClipTooltip();
	void updateLoaderEnabledState (int loaderIndex);
	void updateSatControlsEnabledState (int loaderIndex);
	void syncSatTypeComboSelection (int loaderIndex);
	void commitSatTypeComboSelection (int loaderIndex);
	int getSelectedSatTypeModelIndex (const juce::ComboBox& combo) const noexcept;
	juce::String getMixText() const;
	juce::String getMixTextShort() const;
	void updateAlignModeUi();

	// TR-style label/value display system
	bool legendDirty = true;
	bool refreshLegendTextCache();
	juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds, int columnRight) const;
	juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
	juce::Rectangle<int> getInfoIconArea() const;
	void updateInfoIconCache();
	void setupBar (juce::Slider& s);
	void updateIoFxMeterSliders();

	SATTRAudioProcessor& audioProcessor;

	// ══════════════════════════════════════════════════════════════
	//  Shared components (aliases to TR::LoaderBarSlider etc.)
	// ══════════════════════════════════════════════════════════════
	using BarSlider        = TR::LoaderBarSlider<SATTRAudioProcessorEditor>;
	using NamBrowseButton  = TR::LoaderBrowseButton<SATTRAudioProcessorEditor>;
	using NamFileLabel     = TR::LoaderFileLabel<SATTRAudioProcessorEditor>;

	// Allow shared template to access activeScheme for paint()
	template<typename> friend class TR::LoaderBarSlider;

	// ============================================================================
	//  Filter bar (dual HP/LP marker component, replaces separate sliders)
	// ============================================================================
	using FilterBarComponent = TR::LoaderFilterBarComponent<SATTRAudioProcessorEditor, SATTRAudioProcessor>;

	// ============================================================================
	//  Dual dry/wet level bar (SEND mix mode)
	// ============================================================================
	using DualMixBarComponent = TR::LoaderDualMixBarComponent<SATTRAudioProcessorEditor, SATTRAudioProcessor>;

	// ══════════════════════════════════════════════════════════════
	//  Look and Feel
	// ══════════════════════════════════════════════════════════════
	using MinimalLNF = TR::LoaderLookAndFeel;

	MinimalLNF lnf;

	// ══════════════════════════════════════════════════════════════
	//  DRY helpers for tripled loader A/B/C setup
	// ══════════════════════════════════════════════════════════════
	struct LoaderRefs
	{
		juce::ToggleButton &enableBtn;
		BarSlider &hp, &lp, &in, &out, &tilt, &series, &pan, &fred, &pos;
		juce::ToggleButton &inv, &chaos, &chaosFilter, &sidechain;  juce::Label &chaosDisp, &chaosFilterDisp, &sidechainDisp;
		juce::ToggleButton &exp;  juce::Label &expDisp;
		juce::ComboBox &modeIn, &modeOut, &sumBus, &filterPos;
		FilterBarComponent &filterBar;  BarSlider &mix;
		juce::ComboBox &satType;
		juce::ToggleButton &raw;
		NamBrowseButton &namBrowse;
		NamFileLabel &namDisplay;
		BarSlider &satDrive, &namSlim, &satChar, &satTypeCtrl, &satBias, &satSag;
		BarSlider &detail;
		BarSlider &instability;
		BarSlider &offset;
	};
	struct AttachRefs
	{
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &enableAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &hpAtt, &lpAtt, &inAtt, &outAtt, &tiltAtt, &seriesAtt, &panAtt, &fredAtt, &posAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &invAtt, &chaosAtt, &chaosFilterAtt, &sidechainAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &expAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> &modeInAtt, &modeOutAtt, &sumBusAtt, &filterPosAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &mixAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> &satTypeAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &rawAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &satDriveAtt, &namSlimAtt, &satCharAtt, &satTypeCtrlAtt, &satBiasAtt, &satSagAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &detailAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &instabilityAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &offsetAtt;
	};
	LoaderRefs  getLoaderRefs (int index);
	AttachRefs  getAttachRefs (int index);
	TR::LoaderPanelSpec describeLoaderPanelSpec (int index);
	void setupLoaderUI (int loaderIndex, LoaderRefs refs, const char* chaosAmtId, const char* chaosSpdId);
	void createLoaderAttachments (juce::AudioProcessorValueTreeState& params, int loaderIndex,
	                              LoaderRefs ui, AttachRefs att);

	struct LoaderParamIds
	{
		const char* enable;
		const char* hpFreq;  const char* lpFreq;  const char* in;  const char* out;  const char* tilt;
		const char* series;  const char* pan;     const char* fred;   const char* pos;   const char* reso;
		const char* inv;     const char* chaos;  const char* chaosFilter; const char* sidechain;
		const char* sidechainGain; const char* sidechainSmooth;
		const char* sidechainHp; const char* sidechainLp; const char* sidechainHpOn; const char* sidechainLpOn;
		const char* sidechainHpSlope; const char* sidechainLpSlope;
		const char* chaosAmt; const char* chaosSpd;
		const char* chaosAmtFilter; const char* chaosSpdFilter;
		const char* modeIn;  const char* modeOut; const char* sumBus; const char* filterPos; const char* mix;
		const char* satType; const char* satRaw; const char* satDrive; const char* namSlim; const char* satChar;
		const char* satTypeCtrl;  const char* satBias;  const char* satSag;
		const char* detail;
		const char* instability;
		const char* offset;
		const char* exp;
	};
	static const LoaderParamIds kLoaderParams[3];

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader A
	// ══════════════════════════════════════════════════════════════
	PromptToggleButton enableButtonA;

	BarSlider hpFreqSliderA;
	BarSlider lpFreqSliderA;
	BarSlider inSliderA;
	BarSlider outSliderA;
	BarSlider tiltSliderA;
	BarSlider seriesSliderA;
	BarSlider detailSliderA;
	BarSlider instabilitySliderA;
	BarSlider panSliderA;
	BarSlider fredSliderA;
	BarSlider posSliderA;

	PromptToggleButton invButtonA;
	PromptToggleButton chaosButtonA;
	PromptToggleButton chaosFilterButtonA;
	PromptToggleButton sidechainButtonA;
	juce::Label chaosDisplayA;
	juce::Label chaosFilterDisplayA;
	juce::Label sidechainDisplayA;
	PromptToggleButton expButtonA;
	juce::Label expDisplayA;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tiltAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> seriesAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detailAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> instabilityAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosFilterAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sidechainAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> expAttachA;
	juce::ComboBox modeInComboA;
	juce::ComboBox modeOutComboA;
	juce::ComboBox sumBusComboA;
	juce::ComboBox filterPosComboA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sumBusAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterPosAttachA;

	juce::ComboBox satTypeComboA;
	NamBrowseButton namBrowseButtonA;
	NamFileLabel namFileDisplayA;
	TR::LoaderClearButton namClearButtonA;
	BarSlider satDriveSliderA;
	BarSlider namSlimSliderA;
	BarSlider satCharSliderA;
	BarSlider satTypeCtrlSliderA;
	BarSlider satBiasSliderA;
	BarSlider satSagSliderA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachA;
	PromptToggleButton rawButtonA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> namSlimAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satCharAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satTypeCtrlAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachA;
	BarSlider offsetSliderA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> offsetAttachA;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader B
	// ══════════════════════════════════════════════════════════════
	PromptToggleButton enableButtonB;

	BarSlider hpFreqSliderB;
	BarSlider lpFreqSliderB;
	BarSlider inSliderB;
	BarSlider outSliderB;
	BarSlider tiltSliderB;
	BarSlider seriesSliderB;
	BarSlider detailSliderB;
	BarSlider instabilitySliderB;
	BarSlider panSliderB;
	BarSlider fredSliderB;
	BarSlider posSliderB;

	PromptToggleButton invButtonB;
	PromptToggleButton chaosButtonB;
	PromptToggleButton chaosFilterButtonB;
	PromptToggleButton sidechainButtonB;
	juce::Label chaosDisplayB;
	juce::Label chaosFilterDisplayB;
	juce::Label sidechainDisplayB;
	PromptToggleButton expButtonB;
	juce::Label expDisplayB;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tiltAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> seriesAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detailAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> instabilityAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosFilterAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sidechainAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> expAttachB;
	juce::ComboBox modeInComboB;
	juce::ComboBox modeOutComboB;
	juce::ComboBox sumBusComboB;
	juce::ComboBox filterPosComboB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sumBusAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterPosAttachB;

	juce::ComboBox satTypeComboB;
	NamBrowseButton namBrowseButtonB;
	NamFileLabel namFileDisplayB;
	TR::LoaderClearButton namClearButtonB;
	BarSlider satDriveSliderB;
	BarSlider namSlimSliderB;
	BarSlider satCharSliderB;
	BarSlider satTypeCtrlSliderB;
	BarSlider satBiasSliderB;
	BarSlider satSagSliderB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachB;
	PromptToggleButton rawButtonB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> namSlimAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satCharAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satTypeCtrlAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachB;
	BarSlider offsetSliderB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> offsetAttachB;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader C
	// ══════════════════════════════════════════════════════════════
	PromptToggleButton enableButtonC;

	BarSlider hpFreqSliderC;
	BarSlider lpFreqSliderC;
	BarSlider inSliderC;
	BarSlider outSliderC;
	BarSlider tiltSliderC;
	BarSlider seriesSliderC;
	BarSlider detailSliderC;
	BarSlider instabilitySliderC;
	BarSlider panSliderC;
	BarSlider fredSliderC;
	BarSlider posSliderC;

	PromptToggleButton invButtonC;
	PromptToggleButton chaosButtonC;
	PromptToggleButton chaosFilterButtonC;
	PromptToggleButton sidechainButtonC;
	juce::Label chaosDisplayC;
	juce::Label chaosFilterDisplayC;
	juce::Label sidechainDisplayC;
	PromptToggleButton expButtonC;
	juce::Label expDisplayC;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tiltAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> seriesAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> detailAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> instabilityAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosFilterAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sidechainAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> expAttachC;
	juce::ComboBox modeInComboC;
	juce::ComboBox modeOutComboC;
	juce::ComboBox sumBusComboC;
	juce::ComboBox filterPosComboC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sumBusAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterPosAttachC;

	juce::ComboBox satTypeComboC;
	NamBrowseButton namBrowseButtonC;
	NamFileLabel namFileDisplayC;
	TR::LoaderClearButton namClearButtonC;
	BarSlider satDriveSliderC;
	BarSlider namSlimSliderC;
	BarSlider satCharSliderC;
	BarSlider satTypeCtrlSliderC;
	BarSlider satBiasSliderC;
	BarSlider satSagSliderC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachC;
	PromptToggleButton rawButtonC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> namSlimAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satCharAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satTypeCtrlAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachC;
	BarSlider offsetSliderC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> offsetAttachC;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Filter Bars & per-loader MIX
	// ══════════════════════════════════════════════════════════════
	FilterBarComponent filterBarA_;
	FilterBarComponent filterBarB_;
	FilterBarComponent filterBarC_;

	BarSlider mixSliderA;   // per-loader MIX A (kParamMixA)
	BarSlider mixSliderB;   // per-loader MIX B (kParamMixB)
	BarSlider mixSliderC;   // per-loader MIX C (kParamMixC)
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachC;

	juce::File currentNamFolderA { juce::File::getSpecialLocation (juce::File::userHomeDirectory) };
	juce::File currentNamFolderB { juce::File::getSpecialLocation (juce::File::userHomeDirectory) };
	juce::File currentNamFolderC { juce::File::getSpecialLocation (juce::File::userHomeDirectory) };
	juce::String cachedNamDisplayPaths_[3];
	std::unique_ptr<juce::FileChooser> namFileChooser;
	bool rawVisualOverrideActive_[3] = { false, false, false };
	bool sidechainVisualOverrideActive_[3] = { false, false, false };

	// ══════════════════════════════════════════════════════════════
	//  Collapse/Expand state  (per-loader independent)
	// ══════════════════════════════════════════════════════════════
	bool ioExpandedA_ = false;
	bool ioExpandedB_ = false;
	bool ioExpandedC_ = false;
	juce::Rectangle<int> cachedToggleBarAreaA_;
	juce::Rectangle<int> cachedToggleBarAreaB_;
	juce::Rectangle<int> cachedToggleBarAreaC_;

	// Loader/footer compaction state: switches between 1/2/3 visible loaders
	// and the GLOBAL footer focus without changing processing.
	int visibleLoaderCount_ = 3;
	int firstVisibleLoaderIndex_ = 0;
	bool footerExpanded_ = false;
	bool singleLoaderIoExpanded_ = false;
	bool applyingCompactResize_ = false;

	TR::LoaderLayoutSpec layoutSpec_;
	void syncLayoutSpecToMembers();
	int cachedHeaderTitleX_ = 16;
	juce::Rectangle<int> cachedLoaderTabAreas_[3];
	int cachedLoaderTabStartIndices_[3] = {};
	int cachedLoaderTabCount_ = 0;
	juce::Rectangle<int> cachedFooterRailArea_;
	juce::Rectangle<int> cachedFooterPanelArea_;
	juce::Rectangle<int> cachedFooterTitleArea_;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Global
	// ══════════════════════════════════════════════════════════════
	juce::ComboBox routeCombo;
	juce::ComboBox matchCombo;
	juce::ComboBox trimCombo;
	juce::ComboBox mixModeCombo;
	juce::ComboBox limModeCombo;
	juce::ComboBox invPolCombo;
	juce::ComboBox invStrCombo;

	BarSlider globalMixSlider;   // Global dry/wet MIX (kParamMix)
	BarSlider globalOutputSlider; // Global output gain (kParamOutput)
	BarSlider limThresholdSlider; // Limiter threshold (kParamLimThreshold)
	DualMixBarComponent dualMixBar_;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> routeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> matchAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> trimAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mixModeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> limModeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> invPolAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> invStrAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalMixAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalOutputAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> limThresholdAttach;

	// Auto-align
	PromptToggleButton alignButton;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> alignAttach;

	// ══════════════════════════════════════════════════════════════
	//  CRT Effect
	// ══════════════════════════════════════════════════════════════
	CrtEffect crtEffect;
	float crtTime = 0.0f;
	bool crtEnabled = false;
	bool ioFxEnabled = true;

	void applyCrtState (bool enabled);

	// ══════════════════════════════════════════════════════════════
	//  Palette & Colour Scheme
	// ══════════════════════════════════════════════════════════════
	TR::LoaderPaletteState paletteState;
	TR::TRScheme activeScheme;

	void refreshActivePalette()
	{
		TR::applyLoaderPalette (*this, paletteState, lnf, activeScheme,
			{
				&modeInComboA, &modeOutComboA, &sumBusComboA, &filterPosComboA, &satTypeComboA,
				&modeInComboB, &modeOutComboB, &sumBusComboB, &filterPosComboB, &satTypeComboB,
				&modeInComboC, &modeOutComboC, &sumBusComboC, &filterPosComboC, &satTypeComboC,
				&routeCombo, &matchCombo, &trimCombo, &mixModeCombo, &limModeCombo, &invPolCombo, &invStrCombo
			},
			{ &filterBarA_, &filterBarB_, &filterBarC_ },
			{ &namClearButtonA, &namClearButtonB, &namClearButtonC },
			[this] (const TR::TRScheme& scheme)
			{
				dualMixBar_.setScheme (scheme);

				if (tooltipWindow != nullptr)
				{
					tooltipWindow->setColour (juce::TooltipWindow::backgroundColourId, scheme.bg);
					tooltipWindow->setColour (juce::TooltipWindow::textColourId,       scheme.text);
					tooltipWindow->setColour (juce::TooltipWindow::outlineColourId,    scheme.outline);
					tooltipWindow->setLookAndFeel (&lnf);
				}

				updateIoFxMeterSliders();
			});
	}

	// ══════════════════════════════════════════════════════════════
	//  Misc UI State
	// ══════════════════════════════════════════════════════════════
	bool isDraggingWindow = false;
	juce::Point<int> dragStartPos;

	// ══════════════════════════════════════════════════════════════
	//  TR-style legend text cache (for value display)
	// ══════════════════════════════════════════════════════════════
	struct CachedParamText { juce::String full, short_, intOnly; };
	// Param indices: HP=0, LP=1, IN=2, OUT=3, TILT=4, SERIES=5, PAN=6, FRED=7, POS=8, MIX=9, DRIVE=10, SIZE=11, CHAR=12, TYPE=13, BIAS=14, DYN=15, DETAIL=16, INST=17, DELAY=18
	static constexpr int kNumCachedParams = 19;
	CachedParamText cachedTexts[3][kNumCachedParams];  // [loader][param]

	// Global mix legend cache (for SEND mode dB display)
	juce::String cachedMixTextFull;
	juce::String cachedMixTextShort;
	juce::String cachedMixIntOnly;

	// Column right edges (set in resized(), used by getValueAreaFor())
	int columnLeft_[3]  = {};
	int columnRight_[3] = {};

	// Offset bar areas (set in layoutLoaderSection, painted in paintOverChildren)

	// Value display areas (calculated in paint(), used for click detection)
	std::array<juce::Rectangle<int>, 3 * kNumCachedParams> cachedValueAreas_;

	// Gear icon for info button
	juce::Path cachedInfoGearPath;
	juce::Rectangle<float> cachedInfoGearHole;

public:
	// Public for template friend access from TRSharedUI.h
	using PromptOverlay = TR::PromptOverlay;
	PromptOverlay promptOverlay;
	std::unique_ptr<juce::TooltipWindow> tooltipWindow;
	void setPromptOverlayActive (bool shouldBeActive);
	bool promptOverlayActive = false;

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SATTRAudioProcessorEditor)
};

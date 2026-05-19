#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CrtEffect.h"
#include "TRSharedUI.h"

class SATTRAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Slider::Listener,
                                   private juce::Button::Listener,
                                   private juce::ComboBox::Listener,
                                   private juce::AudioProcessorValueTreeState::Listener,
                                   private juce::Timer
{
public:
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
	void openMixSendPrompt();
	void applyLabelTextColour (juce::Label& label, juce::Colour colour);
	void layoutLoaderSection (juce::Rectangle<int> area, int loaderIndex);
	void updateLoaderEnabledState (int loaderIndex);
	void updateSatControlsEnabledState (int loaderIndex);
	void syncSatTypeComboSelection (int loaderIndex);
	void commitSatTypeComboSelection (int loaderIndex);
	int getSelectedSatTypeModelIndex (const juce::ComboBox& combo) const noexcept;
	juce::String getMixText() const;
	juce::String getMixTextShort() const;

	// TR-style label/value display system
	bool legendDirty = true;
	bool refreshLegendTextCache();
	juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds, int columnRight) const;
	juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
	juce::Rectangle<int> getInfoIconArea() const;
	void updateInfoIconCache();
	void setupBar (juce::Slider& s);

	SATTRAudioProcessor& audioProcessor;

	// ══════════════════════════════════════════════════════════════
	//  Custom Slider with right-click popup
	// ══════════════════════════════════════════════════════════════
	class BarSlider : public juce::Slider
	{
	public:
		enum class Type { Unknown, HpFreq, LpFreq, Input, Output, Tilt,
	                  Series, Pan, Fred, Pos,
	                  Mix, GlobalMix, GlobalOutput, LimThreshold,
	                  SatDrive, SatGirth, SatMod, SatBias, SatSag, Detail, Instability, Delay };
		void setOwner (SATTRAudioProcessorEditor* o) { owner = o; }
		void setType (Type t) { type_ = t; }
		Type getType() const { return type_; }
		void setAllowNumericPopup (bool allow) { allowNumericPopup = allow; }

		void mouseDown (const juce::MouseEvent& e) override
		{
			if (e.mods.isPopupMenu() && allowNumericPopup)
			{
				if (owner != nullptr)
					owner->openNumericEntryPopupForSlider (*this);
				return;
			}
			juce::Slider::mouseDown (e);
		}

		juce::String getTextFromValue (double v) override;

	private:
		SATTRAudioProcessorEditor* owner = nullptr;
		Type type_ = Type::Unknown;
		bool allowNumericPopup = true;
	};

	// ══════════════════════════════════════════════════════════════
	//  Filter bar (dual HP/LP marker component, replaces separate sliders)
	// ══════════════════════════════════════════════════════════════
	class FilterBarComponent : public juce::Component,
	                           public juce::SettableTooltipClient
	{
	public:
		void setOwner (SATTRAudioProcessorEditor* o, int loaderIdx) { owner = o; loaderIndex_ = loaderIdx; }
		void setScheme (const TR::TRScheme& s) { scheme = s; repaint(); }

		void paint (juce::Graphics& g) override;
		void mouseDown (const juce::MouseEvent& e) override;
		void mouseDrag (const juce::MouseEvent& e) override;
		void mouseUp (const juce::MouseEvent& e) override;
		void mouseMove (const juce::MouseEvent& e) override;
		void mouseDoubleClick (const juce::MouseEvent& e) override;

		void updateFromProcessor();

	private:
		SATTRAudioProcessorEditor* owner = nullptr;
		int loaderIndex_ = 0;
		TR::TRScheme scheme {};

		float hpFreq_ = 80.0f;
		float lpFreq_ = 12000.0f;
		bool  hpOn_   = true;
		bool  lpOn_   = true;

		enum DragTarget { None, HP, LP };
		DragTarget currentDrag_ = None;

		static constexpr float kMinFreq = 20.0f;
		static constexpr float kMaxFreq = 20000.0f;
		static constexpr float kPad     = 7.0f;
		static constexpr int   kMarkerHitPx = 10;

		juce::Rectangle<float> getInnerArea() const;
		float freqToNormX (float freq) const;
		float normXToFreq (float normX) const;
		float getMarkerScreenX (float freq) const;
		DragTarget hitTestMarker (juce::Point<float> p) const;
		void  setFreqFromMouseX (float mouseX, DragTarget target);
		void  updateTooltipForTarget (DragTarget target);
	};

	// ══════════════════════════════════════════════════════════════
	//  Dual dry/wet level bar (SEND mix mode)
	// ══════════════════════════════════════════════════════════════
	class DualMixBarComponent : public juce::Component,
	                            public juce::SettableTooltipClient
	{
	public:
		DualMixBarComponent() = default;
		void setOwner (SATTRAudioProcessorEditor* o) { owner = o; }
		void setScheme (const TR::TRScheme& s) { scheme = s; repaint(); }

		void paint (juce::Graphics& g) override;
		void mouseDown (const juce::MouseEvent& e) override;
		void mouseDrag (const juce::MouseEvent& e) override;
		void mouseUp (const juce::MouseEvent& e) override;
		void mouseMove (const juce::MouseEvent& e) override;

		void updateFromProcessor();

		float getDryLevel() const { return dryLevel_; }
		float getWetLevel() const { return wetLevel_; }

		enum DragTarget { None, DRY, WET };
		DragTarget getLastTouched() const { return lastTouched_; }

	private:
		SATTRAudioProcessorEditor* owner = nullptr;
		TR::TRScheme scheme {};

		float dryLevel_ = 0.0f;
		float wetLevel_ = 1.0f;

		DragTarget currentDrag_ = None;
		DragTarget lastTouched_ = WET;

		static constexpr float kPad = 7.0f;
		static constexpr int   kMarkerHitPx = 14;

		juce::Rectangle<float> getInnerArea() const;
		DragTarget hitTestMarker (juce::Point<float> p) const;
		void  setLevelFromMouseX (float mouseX, DragTarget target);
		void  updateTooltipForTarget (DragTarget target);
	};

	// ══════════════════════════════════════════════════════════════
	//  Look and Feel
	// ══════════════════════════════════════════════════════════════
	class MinimalLNF : public juce::LookAndFeel_V4
	{
	public:
		MinimalLNF()
		{
			scheme = { juce::Colours::black, juce::Colours::white,
			           juce::Colours::white, juce::Colours::white };
			TR::applySchemeToLookAndFeel (*this, scheme);
		}

		void setScheme (const TR::TRScheme& s)
		{
			scheme = s;
			TR::applySchemeToLookAndFeel (*this, scheme);
		}

		void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
		                       float sliderPos, float minSliderPos, float maxSliderPos,
		                       const juce::Slider::SliderStyle, juce::Slider&) override;

		void drawTickBox (juce::Graphics&, juce::Component&,
		                  float x, float y, float w, float h,
		                  bool ticked, bool isEnabled,
		                  bool highlighted, bool down) override;

		void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
		                      bool shouldDrawButtonAsHighlighted,
		                      bool shouldDrawButtonAsDown) override;

		void drawButtonBackground (juce::Graphics&, juce::Button&,
		                           const juce::Colour& backgroundColour,
		                           bool shouldDrawButtonAsHighlighted,
		                           bool shouldDrawButtonAsDown) override;

		void drawComboBox (juce::Graphics&, int width, int height,
		                   bool isButtonDown, int buttonX, int buttonY,
		                   int buttonW, int buttonH, juce::ComboBox&) override;

		juce::Font getComboBoxFont (juce::ComboBox& box) override;

		void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
		{
			label.setFont (getComboBoxFont (box));
			label.setBounds (1, 1, box.getWidth() - 2, box.getHeight() - 2);
			label.setJustificationType (juce::Justification::centred);
		}

		void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

		void drawAlertBox (juce::Graphics&, juce::AlertWindow&,
		                   const juce::Rectangle<int>& textArea,
		                   juce::TextLayout& textLayout) override;

		void drawBubble (juce::Graphics&, juce::BubbleComponent&,
		                 const juce::Point<float>& tip,
		                 const juce::Rectangle<float>& body) override;

		juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
		juce::Font getAlertWindowMessageFont() override;
		juce::Font getLabelFont (juce::Label& label) override;
		juce::Font getSliderPopupFont (juce::Slider&) override;
		juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
		                                       juce::Point<int> screenPos,
		                                       juce::Rectangle<int> parentArea) override;
		void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

		void drawScrollbar (juce::Graphics&, juce::ScrollBar&,
		                    int x, int y, int width, int height,
		                    bool isScrollbarVertical,
		                    int thumbStartPosition, int thumbSize,
		                    bool isMouseOver, bool isMouseDown) override;

		int getMinimumScrollbarThumbSize (juce::ScrollBar&) override { return 16; }
		int getScrollbarButtonSize (juce::ScrollBar&) override      { return 0; }

		TR::TRScheme scheme;
	};

	MinimalLNF lnf;

	// ══════════════════════════════════════════════════════════════
	//  DRY helpers for tripled loader A/B/C setup
	// ══════════════════════════════════════════════════════════════
	struct LoaderRefs
	{
		juce::ToggleButton &enableBtn;
		BarSlider &hp, &lp, &in, &out, &tilt, &series, &pan, &fred, &pos;
		juce::ToggleButton &inv, &chaos, &chaosFilter;  juce::Label &chaosDisp;
		juce::ToggleButton &exp;  juce::Label &expDisp;
		juce::ComboBox &modeIn, &modeOut, &sumBus, &filterPos;
		FilterBarComponent &filterBar;  BarSlider &mix;
		juce::ComboBox &satType;
		juce::ToggleButton &raw;
		BarSlider &satDrive, &satGirth, &satMod, &satBias, &satSag;
		BarSlider &detail;
		BarSlider &instability;
		BarSlider &delay;
	};
	struct AttachRefs
	{
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &enableAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &hpAtt, &lpAtt, &inAtt, &outAtt, &tiltAtt, &seriesAtt, &panAtt, &fredAtt, &posAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &invAtt, &chaosAtt, &chaosFilterAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &expAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> &modeInAtt, &modeOutAtt, &sumBusAtt, &filterPosAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &mixAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> &satTypeAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &rawAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &satDriveAtt, &satGirthAtt, &satModAtt, &satBiasAtt, &satSagAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &detailAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &instabilityAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &delayAtt;
	};
	LoaderRefs  getLoaderRefs (int index);
	AttachRefs  getAttachRefs (int index);
	void setupLoaderUI (int loaderIndex, LoaderRefs refs, const char* chaosAmtId, const char* chaosSpdId);
	void createLoaderAttachments (juce::AudioProcessorValueTreeState& params, int loaderIndex,
	                              LoaderRefs ui, AttachRefs att);

	struct LoaderParamIds
	{
		const char* enable;
		const char* hpFreq;  const char* lpFreq;  const char* in;  const char* out;  const char* tilt;
		const char* series;  const char* pan;     const char* fred;   const char* pos;   const char* reso;
		const char* inv;     const char* chaos;  const char* chaosFilter;
		const char* chaosAmt; const char* chaosSpd;
		const char* chaosAmtFilter; const char* chaosSpdFilter;
		const char* modeIn;  const char* modeOut; const char* sumBus; const char* filterPos; const char* mix;
		const char* satType; const char* satRaw; const char* satDrive; const char* satGirth;
		const char* satMod;  const char* satBias;  const char* satSag;
		const char* detail;
		const char* instability;
		const char* delay;
		const char* exp;
	};
	static const LoaderParamIds kLoaderParams[3];

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader A
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonA;

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

	juce::ToggleButton invButtonA;
	juce::ToggleButton chaosButtonA;
	juce::ToggleButton chaosFilterButtonA;
	juce::Label chaosDisplayA;
	juce::ToggleButton expButtonA;
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
	BarSlider satDriveSliderA;
	BarSlider satGirthSliderA;
	BarSlider satModSliderA;
	BarSlider satBiasSliderA;
	BarSlider satSagSliderA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachA;
	juce::ToggleButton rawButtonA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satGirthAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satModAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachA;
	BarSlider delaySliderA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachA;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader B
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonB;

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

	juce::ToggleButton invButtonB;
	juce::ToggleButton chaosButtonB;
	juce::ToggleButton chaosFilterButtonB;
	juce::Label chaosDisplayB;
	juce::ToggleButton expButtonB;
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
	BarSlider satDriveSliderB;
	BarSlider satGirthSliderB;
	BarSlider satModSliderB;
	BarSlider satBiasSliderB;
	BarSlider satSagSliderB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachB;
	juce::ToggleButton rawButtonB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satGirthAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satModAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachB;
	BarSlider delaySliderB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachB;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Loader C
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonC;

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

	juce::ToggleButton invButtonC;
	juce::ToggleButton chaosButtonC;
	juce::ToggleButton chaosFilterButtonC;
	juce::Label chaosDisplayC;
	juce::ToggleButton expButtonC;
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
	BarSlider satDriveSliderC;
	BarSlider satGirthSliderC;
	BarSlider satModSliderC;
	BarSlider satBiasSliderC;
	BarSlider satSagSliderC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> satTypeAttachC;
	juce::ToggleButton rawButtonC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rawAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satDriveAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satGirthAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satModAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satBiasAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> satSagAttachC;
	BarSlider delaySliderC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachC;

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

	// ══════════════════════════════════════════════════════════════
	//  Collapse/Expand state  (per-loader independent)
	// ══════════════════════════════════════════════════════════════
	bool ioExpandedA_ = false;
	bool ioExpandedB_ = false;
	bool ioExpandedC_ = false;
	juce::Rectangle<int> cachedToggleBarAreaA_;
	juce::Rectangle<int> cachedToggleBarAreaB_;
	juce::Rectangle<int> cachedToggleBarAreaC_;

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
	juce::ToggleButton alignButton;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> alignAttach;

	// ══════════════════════════════════════════════════════════════
	//  CRT Effect
	// ══════════════════════════════════════════════════════════════
	CrtEffect crtEffect;
	float crtTime = 0.0f;
	bool crtEnabled = false;

	void applyCrtState (bool enabled)
	{
		crtEnabled = enabled;
		crtEffect.setEnabled (enabled);
		crtTime = 0.0f;
	}

	// ══════════════════════════════════════════════════════════════
	//  Palette & Colour Scheme
	// ══════════════════════════════════════════════════════════════
	bool useCustomPalette = false;
	std::array<juce::Colour, 2> defaultPalette { juce::Colours::white, juce::Colours::black };
	std::array<juce::Colour, 2> customPalette  { juce::Colours::white, juce::Colours::black };
	TR::TRScheme activeScheme;

	void applyActivePalette()
	{
		const auto& palette = useCustomPalette ? customPalette : defaultPalette;

		TR::TRScheme scheme;
		scheme.bg      = palette[1];
		scheme.fg      = palette[0];
		scheme.outline = palette[0];
		scheme.text    = palette[0];

		activeScheme = scheme;
		lnf.setScheme (activeScheme);

		auto applyComboScheme = [this] (juce::ComboBox& c) {
			c.setColour (juce::ComboBox::textColourId,       activeScheme.text);
			c.setColour (juce::ComboBox::backgroundColourId, activeScheme.bg);
			c.setColour (juce::ComboBox::outlineColourId,    activeScheme.outline);
		};

		for (int i = 0; i < 3; ++i)
		{
			auto r = getLoaderRefs (i);
			applyComboScheme (r.modeIn);
			applyComboScheme (r.modeOut);
			applyComboScheme (r.sumBus);
			applyComboScheme (r.filterPos);
			applyComboScheme (r.satType);
			r.filterBar.setScheme (activeScheme);
		}

		applyComboScheme (routeCombo);
		applyComboScheme (matchCombo);
		applyComboScheme (trimCombo);
		applyComboScheme (mixModeCombo);
		applyComboScheme (limModeCombo);
		applyComboScheme (invPolCombo);
		applyComboScheme (invStrCombo);
		dualMixBar_.setScheme (activeScheme);
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
	// Param indices: HP=0, LP=1, IN=2, OUT=3, TILT=4, SERIES=5, PAN=6, FRED=7, POS=8, MIX=9, DRIVE=10, GIRTH=11, MOD=12, BIAS=13, SAG=14, DETAIL=15, INST=16, DELAY=17
	static constexpr int kNumCachedParams = 18;
	CachedParamText cachedTexts[3][kNumCachedParams];  // [loader][param]

	// Global mix legend cache (for SEND mode dB display)
	juce::String cachedMixTextFull;
	juce::String cachedMixTextShort;
	juce::String cachedMixIntOnly;

	// Column right edges (set in resized(), used by getValueAreaFor())
	int columnLeft_[3]  = {};
	int columnRight_[3] = {};

	// Delay bar areas (set in layoutLoaderSection, painted in paintOverChildren)

	// Value display areas (calculated in paint(), used for click detection)
	std::array<juce::Rectangle<int>, 54> cachedValueAreas_;  // 18 per loader x 3

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

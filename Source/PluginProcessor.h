#pragma once

#include <JuceHeader.h>
#include "SaturationEngine.h"
#include <array>
#include <atomic>
#include <vector>

class CABTRAudioProcessor : public juce::AudioProcessor,
                             private juce::Timer
{
public:
	CABTRAudioProcessor();
	~CABTRAudioProcessor() override;

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader A
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableA      = "enable_a";
	static constexpr const char* kParamHpFreqA      = "hp_freq_a";
	static constexpr const char* kParamLpFreqA      = "lp_freq_a";
	static constexpr const char* kParamHpOnA        = "hp_on_a";
	static constexpr const char* kParamLpOnA        = "lp_on_a";
	static constexpr const char* kParamHpSlopeA     = "hp_slope_a";
	static constexpr const char* kParamLpSlopeA     = "lp_slope_a";
	static constexpr const char* kParamInA          = "in_a";
	static constexpr const char* kParamOutA         = "out_a";
	static constexpr const char* kParamTiltA        = "tilt_a";
	static constexpr const char* kParamStartA       = "start_a";
	static constexpr const char* kParamEndA         = "end_a";
	static constexpr const char* kParamSizeA        = "size_a";
	static constexpr const char* kParamSeriesA      = "series_a";
	static constexpr const char* kParamVarA         = "var_a";
	static constexpr const char* kParamPanA         = "pan_a";
	static constexpr const char* kParamFredA        = "fred_a";
	static constexpr const char* kParamPosA         = "pos_a";
	static constexpr const char* kParamResoA        = "reso_a";
	static constexpr const char* kParamInvA         = "inv_a";
	static constexpr const char* kParamDelayA       = "delay_a";
	static constexpr const char* kParamChaosA       = "chaos_a";
	static constexpr const char* kParamChaosFilterA    = "chaos_filter_a";
	static constexpr const char* kParamChaosAmtA       = "chaos_amt_a";
	static constexpr const char* kParamChaosSpdA       = "chaos_spd_a";
	static constexpr const char* kParamChaosAmtFilterA = "chaos_amt_filter_a";
	static constexpr const char* kParamChaosSpdFilterA = "chaos_spd_filter_a";
	static constexpr const char* kParamModeInA      = "mode_in_a";  // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamModeOutA     = "mode_out_a"; // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamSumBusA      = "sum_bus_a"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamFilterPosA   = "filter_pos_a"; // 0=F▼T▼, 1=F▲T▲, 2=F▲T▼, 3=F▼T▲
	static constexpr const char* kParamMixA         = "mix_a";     // Per-loader dry/wet
	static constexpr const char* kParamSatTypeA     = "sat_type_a";
	static constexpr const char* kParamSatDriveA    = "sat_drive_a";
	static constexpr const char* kParamSatGirthA    = "sat_girth_a";
	static constexpr const char* kParamSatModA      = "sat_mod_a";
	static constexpr const char* kParamSatBiasA     = "sat_bias_a";
	static constexpr const char* kParamSatSagA      = "sat_sag_a";
	static constexpr const char* kParamSatRawA      = "sat_raw_a";
	static constexpr const char* kParamExpA         = "exp_a";
	static constexpr const char* kParamExpOrderA    = "exp_order_a";   // 0=PRE, 1=POST
	static constexpr const char* kParamExpRatioA    = "exp_ratio_a";
	static constexpr const char* kParamExpThreshA   = "exp_thresh_a";
	static constexpr const char* kParamExpAtkA      = "exp_atk_a";
	static constexpr const char* kParamExpRelA      = "exp_rel_a";

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader B
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableB      = "enable_b";
	static constexpr const char* kParamHpFreqB      = "hp_freq_b";
	static constexpr const char* kParamLpFreqB      = "lp_freq_b";
	static constexpr const char* kParamHpOnB        = "hp_on_b";
	static constexpr const char* kParamLpOnB        = "lp_on_b";
	static constexpr const char* kParamHpSlopeB     = "hp_slope_b";
	static constexpr const char* kParamLpSlopeB     = "lp_slope_b";
	static constexpr const char* kParamInB          = "in_b";
	static constexpr const char* kParamOutB         = "out_b";
	static constexpr const char* kParamTiltB        = "tilt_b";
	static constexpr const char* kParamStartB       = "start_b";
	static constexpr const char* kParamEndB         = "end_b";
	static constexpr const char* kParamSizeB        = "size_b";
	static constexpr const char* kParamSeriesB      = "series_b";
	static constexpr const char* kParamVarB         = "var_b";
	static constexpr const char* kParamPanB         = "pan_b";
	static constexpr const char* kParamFredB        = "fred_b";
	static constexpr const char* kParamPosB         = "pos_b";
	static constexpr const char* kParamResoB        = "reso_b";
	static constexpr const char* kParamInvB         = "inv_b";
	static constexpr const char* kParamDelayB       = "delay_b";
	static constexpr const char* kParamChaosB       = "chaos_b";
	static constexpr const char* kParamChaosFilterB    = "chaos_filter_b";
	static constexpr const char* kParamChaosAmtB       = "chaos_amt_b";
	static constexpr const char* kParamChaosSpdB       = "chaos_spd_b";
	static constexpr const char* kParamChaosAmtFilterB = "chaos_amt_filter_b";
	static constexpr const char* kParamChaosSpdFilterB = "chaos_spd_filter_b";
	static constexpr const char* kParamModeInB      = "mode_in_b";  // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamModeOutB     = "mode_out_b"; // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamSumBusB      = "sum_bus_b"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamFilterPosB   = "filter_pos_b";
	static constexpr const char* kParamMixB         = "mix_b";     // Per-loader dry/wet
	static constexpr const char* kParamSatTypeB     = "sat_type_b";
	static constexpr const char* kParamSatDriveB    = "sat_drive_b";
	static constexpr const char* kParamSatGirthB    = "sat_girth_b";
	static constexpr const char* kParamSatModB      = "sat_mod_b";
	static constexpr const char* kParamSatBiasB     = "sat_bias_b";
	static constexpr const char* kParamSatSagB      = "sat_sag_b";
	static constexpr const char* kParamSatRawB      = "sat_raw_b";
	static constexpr const char* kParamExpB         = "exp_b";
	static constexpr const char* kParamExpOrderB    = "exp_order_b";
	static constexpr const char* kParamExpRatioB    = "exp_ratio_b";
	static constexpr const char* kParamExpThreshB   = "exp_thresh_b";
	static constexpr const char* kParamExpAtkB      = "exp_atk_b";
	static constexpr const char* kParamExpRelB      = "exp_rel_b";

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader C
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableC      = "enable_c";
	static constexpr const char* kParamHpFreqC      = "hp_freq_c";
	static constexpr const char* kParamLpFreqC      = "lp_freq_c";
	static constexpr const char* kParamHpOnC        = "hp_on_c";
	static constexpr const char* kParamLpOnC        = "lp_on_c";
	static constexpr const char* kParamHpSlopeC     = "hp_slope_c";
	static constexpr const char* kParamLpSlopeC     = "lp_slope_c";
	static constexpr const char* kParamInC          = "in_c";
	static constexpr const char* kParamOutC         = "out_c";
	static constexpr const char* kParamTiltC        = "tilt_c";
	static constexpr const char* kParamStartC       = "start_c";
	static constexpr const char* kParamEndC         = "end_c";
	static constexpr const char* kParamSizeC        = "size_c";
	static constexpr const char* kParamSeriesC      = "series_c";
	static constexpr const char* kParamVarC         = "var_c";
	static constexpr const char* kParamPanC         = "pan_c";
	static constexpr const char* kParamFredC        = "fred_c";
	static constexpr const char* kParamPosC         = "pos_c";
	static constexpr const char* kParamResoC        = "reso_c";
	static constexpr const char* kParamInvC         = "inv_c";
	static constexpr const char* kParamDelayC       = "delay_c";
	static constexpr const char* kParamChaosC       = "chaos_c";
	static constexpr const char* kParamChaosFilterC    = "chaos_filter_c";
	static constexpr const char* kParamChaosAmtC       = "chaos_amt_c";
	static constexpr const char* kParamChaosSpdC       = "chaos_spd_c";
	static constexpr const char* kParamChaosAmtFilterC = "chaos_amt_filter_c";
	static constexpr const char* kParamChaosSpdFilterC = "chaos_spd_filter_c";
	static constexpr const char* kParamModeInC      = "mode_in_c";
	static constexpr const char* kParamModeOutC     = "mode_out_c";
	static constexpr const char* kParamSumBusC      = "sum_bus_c"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamFilterPosC   = "filter_pos_c";
	static constexpr const char* kParamMixC         = "mix_c";
	static constexpr const char* kParamSatTypeC     = "sat_type_c";
	static constexpr const char* kParamSatDriveC    = "sat_drive_c";
	static constexpr const char* kParamSatGirthC    = "sat_girth_c";
	static constexpr const char* kParamSatModC      = "sat_mod_c";
	static constexpr const char* kParamSatBiasC     = "sat_bias_c";
	static constexpr const char* kParamSatSagC      = "sat_sag_c";
	static constexpr const char* kParamSatRawC      = "sat_raw_c";
	static constexpr const char* kParamExpC         = "exp_c";
	static constexpr const char* kParamExpOrderC    = "exp_order_c";
	static constexpr const char* kParamExpRatioC    = "exp_ratio_c";
	static constexpr const char* kParamExpThreshC   = "exp_thresh_c";
	static constexpr const char* kParamExpAtkC      = "exp_atk_c";
	static constexpr const char* kParamExpRelC      = "exp_rel_c";

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamInput        = "input";
	static constexpr const char* kParamOutput       = "output";
	static constexpr const char* kParamRoute        = "route";     // 0=A->B->C, 1=A|B|C, 2=A->B|C, 3=A|B->C
	static constexpr const char* kParamMix          = "mix";       // Global dry/wet mix
	static constexpr const char* kParamMixMode      = "mix_mode";  // 0=INSERT, 1=SEND
	static constexpr const char* kParamDryLevel     = "dry_level"; // SEND mode dry level
	static constexpr const char* kParamWetLevel     = "wet_level"; // SEND mode wet level
	static constexpr const char* kParamMatch        = "match";
	static constexpr const char* kParamTrim         = "trim";     // Tilt EQ profile (0=None..5=Bright+)

	// Limiter
	static constexpr const char* kParamLimThreshold = "lim_threshold";
	static constexpr const char* kParamLimMode      = "lim_mode";

	// Invert Polarity / Invert Stereo
	static constexpr const char* kParamInvPol       = "inv_pol";
	static constexpr const char* kParamInvStr       = "inv_str";

	// Oversampling (global)
	static constexpr const char* kParamOversample   = "oversample";

	// Auto-align (momentary trigger)
	static constexpr const char* kParamAlign        = "align";

	// ══════════════════════════════════════════════════════════════
	//  UI State Parameters (hidden from DAW automation)
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamUiWidth      = "ui_width";
	static constexpr const char* kParamUiHeight     = "ui_height";
	static constexpr const char* kParamUiPalette    = "ui_palette";
	static constexpr const char* kParamUiFxTail     = "ui_fx_tail";
	static constexpr const char* kParamUiColor0     = "ui_color0";
	static constexpr const char* kParamUiColor1     = "ui_color1";

	// ══════════════════════════════════════════════════════════════
	//  UI State Keys (non-automatable, stored in APVTS tree)
	// ══════════════════════════════════════════════════════════════
	struct UiStateKeys
	{
		static constexpr const char* ioExpandedA = "uiIoExpandedA";
		static constexpr const char* ioExpandedB = "uiIoExpandedB";
		static constexpr const char* ioExpandedC = "uiIoExpandedC";
	};

	void  setUiIoExpanded (int loaderIndex, bool expanded);
	bool  getUiIoExpanded (int loaderIndex) const noexcept;

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — Filters
	// ══════════════════════════════════════════════════════════════
	static constexpr float kFilterFreqMin           = 20.0f;
	static constexpr float kFilterFreqMax           = 20000.0f;
	static constexpr float kFilterHpFreqDefault     = 80.0f;
	static constexpr float kFilterLpFreqDefault     = 12000.0f;
	static constexpr int   kFilterSlopeMin          = 0;       // 6 dB/oct
	static constexpr int   kFilterSlopeMax          = 2;       // 24 dB/oct
	static constexpr int   kFilterSlopeDefault      = 1;       // 12 dB/oct

	// Butterworth Q constants for 24 dB/oct (4th-order cascaded pair)
	static constexpr float kBW4_Q1 = 0.54119610f;   // 1/(2cos(3π/8))
	static constexpr float kBW4_Q2 = 1.30656296f;   // 1/(2cos(π/8))

	// Shared DSP constants
	static constexpr float kSqrt2Over2   = 0.707106781f;  // √2/2  — Butterworth Q, M/S scaling, -3 dB
	static constexpr float kSqrt2        = 1.41421356f;   // √2    — MID impulse compensation
	static constexpr float kTwoPi        = 6.2831853f;     // 2π
	static constexpr float kDistDecay    = 2.0794f;        // DIST exponential rolloff rate

	// Limiter ranges
	static constexpr float kLimThresholdMin     = -36.0f;
	static constexpr float kLimThresholdMax     =   0.0f;
	static constexpr float kLimThresholdDefault =   0.0f;
	static constexpr int   kLimModeDefault      = 0;

	// Invert Polarity / Invert Stereo defaults
	static constexpr int   kInvPolDefault       = 0;   // 0=NONE  1=WET  2=GLOBAL
	static constexpr int   kInvStrDefault       = 0;   // 0=NONE  1=WET  2=GLOBAL

	// Delay (per-loader, for auto-align)
	static constexpr float kDelayMin            = 0.0f;
	static constexpr float kDelayMax            = 5.0f;     // ms (small — saturation phase shifts are tiny)
	static constexpr float kDelayDefault        = 0.0f;

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — Saturation
	// ══════════════════════════════════════════════════════════════
	static constexpr int   kSatTypeMin          = 0;
	static constexpr int   kSatTypeMax          = static_cast<int> (SatEngine::Model::NumModels) - 1;
	static constexpr int   kSatTypeDefault      = 0;   // CLEAN
	static constexpr float kSatDriveMin         = 0.0f;
	static constexpr float kSatDriveMax         = 1.0f;
	static constexpr float kSatDriveDefault     = 0.0f;
	static constexpr float kSatGirthMin         = 0.0f;
	static constexpr float kSatGirthMax         = 1.0f;
	static constexpr float kSatGirthDefault     = 0.0f;
	static constexpr float kSatModMin           = 0.0f;
	static constexpr float kSatModMax           = 1.0f;
	static constexpr float kSatModDefault       = 0.0f;
	static constexpr float kSatBiasMin          = -1.0f;
	static constexpr float kSatBiasMax          = 1.0f;
	static constexpr float kSatBiasDefault      = 0.0f;
	static constexpr float kSatSagMin           = 0.0f;
	static constexpr float kSatSagMax           = 1.0f;
	static constexpr float kSatSagDefault       = 0.0f;

	static constexpr int   kOversampleMin       = 0;   // x1 (off)
	static constexpr int   kOversampleMax       = 4;   // x16
	static constexpr int   kOversampleDefault   = 0;   // x1 (off)

	// MATCH tilt target slopes (dB/oct): None, White, Pink, Brown, Bright, Bright+
	static constexpr float kTargetSlopes[] = { 0.0f, 0.0f, -3.0f, -6.0f, 3.0f, 6.0f };
	static constexpr int   kNumTargetSlopes = 6;

	// NORM peak-target levels: Off, 0 dB, -3 dB, -6 dB, -12 dB, -18 dB
	static constexpr float kNormTargets[] = { 1.0f, 1.0f, 0.70795f, 0.50119f, 0.25119f, 0.12589f };
	static constexpr int   kNumNormTargets = 6;
	static constexpr float kMaxNormBoost   = 7.94328f; // +18 dB

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — IR Controls
	// ══════════════════════════════════════════════════════════════
	static constexpr float kInMin                   = -100.0f;
	static constexpr float kInMax                   = 0.0f;
	static constexpr float kInDefault               = 0.0f;

	static constexpr float kOutMin                  = -100.0f;
	static constexpr float kOutMax                  = 24.0f;
	static constexpr float kOutDefault              = 0.0f;

	static constexpr float kTiltMin                 = -6.0f;
	static constexpr float kTiltMax                 = 6.0f;
	static constexpr float kTiltDefault             = 0.0f;

	static constexpr float kStartMin                = 0.0f;
	static constexpr float kStartMax                = 10000.0f; // 10 seconds max IR
	static constexpr float kStartDefault            = 0.0f;

	static constexpr float kEndMin                  = 0.0f;
	static constexpr float kEndMax                  = 10000.0f;
	static constexpr float kEndDefault              = 10000.0f;

	static constexpr float kSizeMin                 = 0.25f;      // 25%
	static constexpr float kSizeMax                 = 4.0f;       // 400%
	static constexpr float kSizeDefault             = 1.0f;       // 100%

	static constexpr float kSeriesMin               = 1.0f;
	static constexpr float kSeriesMax               = 4.0f;
	static constexpr float kSeriesDefault           = 1.0f;

	static constexpr float kVarMin                  = 0.0f;
	static constexpr float kVarMax                  = 1.0f;
	static constexpr float kVarDefault              = 0.0f;

	static constexpr float kPanMin                  = 0.0f;       // 0% = full left
	static constexpr float kPanMax                  = 1.0f;       // 100% = full right
	static constexpr float kPanDefault              = 0.5f;       // 50% = center

	static constexpr float kChaosAmtMin              = 0.0f;
	static constexpr float kChaosAmtMax              = 100.0f;
	static constexpr float kChaosAmtDefault          = 50.0f;
	static constexpr float kChaosSpdMin              = 0.01f;      // Hz
	static constexpr float kChaosSpdMax              = 100.0f;     // Hz
	static constexpr float kChaosSpdDefault          = 5.0f;       // Hz
	static constexpr float kChaosDriftAmp            = 0.3f;

	// Expander / Noise Gate
	static constexpr float kExpRatioMin              = 1.0f;   // 1:1 (no expansion)
	static constexpr float kExpRatioMax              = 10.0f;  // 1:10
	static constexpr float kExpRatioDefault          = 1.0f;
	static constexpr float kExpThreshMin             = -60.0f; // dB
	static constexpr float kExpThreshMax             = 0.0f;   // dB
	static constexpr float kExpThreshDefault          = 0.0f;  // 0 dB = off
	static constexpr float kExpAtkMin                = 0.01f;  // ms
	static constexpr float kExpAtkMax                = 100.0f; // ms
	static constexpr float kExpAtkDefault            = 0.1f;   // ms
	static constexpr float kExpRelMin                = 5.0f;   // ms
	static constexpr float kExpRelMax                = 2000.0f;// ms
	static constexpr float kExpRelDefault            = 50.0f;  // ms

	static constexpr float kFredMin                 = 0.0f;
	static constexpr float kFredMax                 = 1.0f;
	static constexpr float kFredDefault             = 0.0f;       // 0% = no Fredman effect

	static constexpr float kGlobalMixMin            = 0.0f;
	static constexpr float kGlobalMixMax            = 1.0f;
	static constexpr float kGlobalMixDefault        = 1.0f;       // 100% wet by default

	static constexpr int   kMixModeDefault  = 0;   // 0=INSERT, 1=SEND
	static constexpr float kDryLevelDefault = 0.0f;
	static constexpr float kWetLevelDefault = 1.0f;

	static constexpr float kPosMin                  = 0.0f;       // 0% = no effect
	static constexpr float kPosMax                  = 1.0f;       // 100% = full Friedman simulation
	static constexpr float kPosDefault              = 0.0f;

	static constexpr float kResoMin                  = 0.0f;       // 0%
	static constexpr float kResoMax                  = 2.0f;       // 200%
	static constexpr float kResoDefault              = 1.0f;       // 100% = original IR

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — Global
	// ══════════════════════════════════════════════════════════════
	static constexpr float kInputMin                = -100.0f;
	static constexpr float kInputMax                = 0.0f;
	static constexpr float kInputDefault            = 0.0f;

	static constexpr float kOutputMin               = -100.0f;
	static constexpr float kOutputMax               = 24.0f;
	static constexpr float kOutputDefault           = 0.0f;

	static constexpr int   kModeMin                 = 0;
	static constexpr int   kModeMax                 = 2;          // L+R, MID, SIDE
	static constexpr int   kModeDefault             = 0;          // L+R

	static constexpr int   kSumBusMax               = 2;          // 0=ST, 1=→M, 2=→S
	static constexpr int   kSumBusDefault           = 0;          // ST (unchanged)
	static constexpr int   kFilterPosDefault        = 0;          // 0=F▼T▼ (both POST)

	static constexpr int   kRouteMin                = 0;
	static constexpr int   kRouteMax                = 3;          // 0=A->B->C, 1=A|B|C, 2=A->B|C, 3=A|B->C
	static constexpr int   kRouteDefault            = 1;          // Parallel by default
	static constexpr int   kMatchDefault            = 0;
	static constexpr int   kTrimDefault             = 0;          // None (no tilt)

	// ══════════════════════════════════════════════════════════════
	//  AudioProcessor overrides
	// ══════════════════════════════════════════════════════════════
	void prepareToPlay (double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;

#if ! JucePlugin_PreferredChannelConfigurations
	bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
#endif

	void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override;

	const juce::String getName() const override;

	bool acceptsMidi() const override;
	bool producesMidi() const override;
	bool isMidiEffect() const override;
	double getTailLengthSeconds() const override;

	int getNumPrograms() override;
	int getCurrentProgram() override;
	void setCurrentProgram (int index) override;
	const juce::String getProgramName (int index) override;
	void changeProgramName (int index, const juce::String& newName) override;

	void getStateInformation (juce::MemoryBlock& destData) override;
	void setStateInformation (const void* data, int sizeInBytes) override;

	// ══════════════════════════════════════════════════════════════
	//  Public API — Parameter access
	// ══════════════════════════════════════════════════════════════
	juce::AudioProcessorValueTreeState& getValueTreeState() { return parameters; }

	// ══════════════════════════════════════════════════════════════
	//  UI State Accessors
	// ══════════════════════════════════════════════════════════════
	int getUiEditorWidth() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiWidth))
			return static_cast<int> (*p);
		return 800;
	}

	int getUiEditorHeight() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiHeight))
			return static_cast<int> (*p);
		return 600;
	}

	void setUiEditorSize (int w, int h)
	{
		if (auto* pw = parameters.getParameter (kParamUiWidth))
			pw->setValueNotifyingHost (pw->convertTo0to1 (static_cast<float> (w)));
		if (auto* ph = parameters.getParameter (kParamUiHeight))
			ph->setValueNotifyingHost (ph->convertTo0to1 (static_cast<float> (h)));
	}

	bool getUiUseCustomPalette() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiPalette))
			return static_cast<int> (*p) != 0;
		return false;
	}

	void setUiUseCustomPalette (bool useCustom)
	{
		if (auto* p = parameters.getParameter (kParamUiPalette))
			p->setValueNotifyingHost (useCustom ? 1.0f : 0.0f);
	}

	bool getUiFxTailEnabled() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiFxTail))
			return static_cast<bool> (*p);
		return false;
	}

	void setUiFxTailEnabled (bool enabled)
	{
		if (auto* p = parameters.getParameter (kParamUiFxTail))
			p->setValueNotifyingHost (enabled ? 1.0f : 0.0f);
	}

	juce::Colour getUiCustomPaletteColour (int index) const
	{
		if (index < 0 || index > 1)
			return juce::Colours::green;

		const char* paramId = (index == 0) ? kParamUiColor0 : kParamUiColor1;
		if (auto* p = parameters.getRawParameterValue (paramId))
		{
			const auto rgb = static_cast<juce::uint32> (*p);
			return juce::Colour (0xFF000000 | rgb);
		}
		return juce::Colours::green;
	}

	void setUiCustomPaletteColour (int index, juce::Colour colour)
	{
		if (index < 0 || index > 1)
			return;

		const char* paramId = (index == 0) ? kParamUiColor0 : kParamUiColor1;
		if (auto* p = parameters.getParameter (paramId))
		{
			const auto rgb = colour.getARGB() & 0x00FFFFFF;
			const float normalized = static_cast<float> (rgb) / static_cast<float> (0xFFFFFF);
			p->setValueNotifyingHost (normalized);
		}
	}

	// ══════════════════════════════════════════════════════════════
	//  DSP State — IR Convolution (public for editor access)
	// ══════════════════════════════════════════════════════════════
	struct IRLoaderState
		{
		// Filter states (6/12/24 dB/oct slope-selectable)
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> hpFilter;
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> hpFilter2;  // 2nd stage for 24dB/oct
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> lpFilter;
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> lpFilter2;  // 2nd stage for 24dB/oct
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> posFilter;
		
		
		
		// EMA-smoothed filter frequencies (per-sample smoothing, coeff update every 32 samples)
		float smoothedHpFreq = 80.0f;
		float smoothedLpFreq = 12000.0f;
		float smoothedPosFreq = -1.0f;
		float lastHpFreq = -1.0f;
		float lastLpFreq = -1.0f;
		int lastHpSlope = -1;
		int lastLpSlope = -1;
		float lastPosFreq = -1.0f;
		int filterCoeffCountdown = 0;
		static constexpr int kFilterCoeffUpdateInterval = 8;
		float lastPan = -1.0f;
		float lastPanLeft = 1.0f;
		float lastPanRight = 1.0f;
		
		// FRED (Fredman miking) state: fractional delay buffer for off-axis simulation
		// ~159µs delay ≈ 5cm path difference (realistic Fredman setup)
		// Buffer sized for max delay at any sample rate up to 192kHz
		static constexpr int kFredDelayBufSize = 64;          // holds enough for ~159µs @ 192kHz (31 samples) + guard
		static constexpr float kFredDelayMicros = 159.0f;     // delay in microseconds (sample-rate independent)
		float fredDelayBuffer[2][kFredDelayBufSize] = {};
		int fredDelayIndex = 0;

		// CHAOS micro-delay line (reused by Hermite+Drift engine)
		static constexpr int kChaosDelayMaxSamples = 1024;
		float chaosDelayBuffer[2][kChaosDelayMaxSamples] = {};
		int chaosDelayWritePos = 0;

		// CHS D Hermite+Drift: delay (per-channel for stereo)
		float chaosDPrev[2]         = {};
		float chaosDCurr[2]         = {};
		float chaosDNext[2]         = {};
		float chaosDPhase[2]        = {};
		float chaosDDriftPhase[2]   = {};
		float chaosDDriftFreqHz[2]  = {};
		float chaosDOut[2]          = {};
		juce::Random chaosDRng[2];

		// CHS D Hermite+Drift: gain (per-channel, decorrelated)
		float chaosGPrev[2]         = {};
		float chaosGCurr[2]         = {};
		float chaosGNext[2]         = {};
		float chaosGPhase[2]        = {};
		float chaosGDriftPhase[2]   = {};
		float chaosGDriftFreqHz[2]  = {};
		float chaosGOut[2]          = {};
		juce::Random chaosGRng[2];

		// CHS F Hermite+Drift: filter (mono S&H + quadrature drift)
		float chaosFPrev            = 0.0f;
		float chaosFCurr            = 0.0f;
		float chaosFNext            = 0.0f;
		float chaosFPhase           = 0.0f;
		float chaosFDriftPhase      = 0.0f;
		float chaosFDriftFreqHz     = 0.0f;
		float chaosFOut[2]          = {};
		juce::Random chaosFRng;

		// Per-loader tilt EQ state (1st-order shelf, pivot 1kHz)
		float tiltB0 = 1.0f, tiltB1 = 0.0f, tiltA1 = 0.0f;
		float tiltTargetB0 = 1.0f, tiltTargetB1 = 0.0f, tiltTargetA1 = 0.0f;
		float tiltState[2] = { 0.0f, 0.0f };
		float lastTiltDb = 0.0f;

		// Saturation engine state
		SatEngine::State satState;

		// Expander envelope follower state (per-channel)
		float expEnv[2] = { 0.0f, 0.0f };

		// Delay line for phase alignment (max ~0.5s)
		juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 96000 };
		juce::SmoothedValue<float> smoothedDelay { 0.0f };
		
		// Delete copy operations (contains atomic)
		IRLoaderState() = default;
		IRLoaderState (const IRLoaderState&) = delete;
		IRLoaderState& operator= (const IRLoaderState&) = delete;
		IRLoaderState (IRLoaderState&&) = default;
		IRLoaderState& operator= (IRLoaderState&&) = default;
	};

	IRLoaderState stateA;
	IRLoaderState stateB;
	IRLoaderState stateC;

	// ══════════════════════════════════════════════════════════════
	//  Helper methods (public for editor to trigger IR loading)
	// ══════════════════════════════════════════════════════════════


private:
	// Timer callback to monitor parameter changes
	void timerCallback() override;
	// ══════════════════════════════════════════════════════════════
	//  Parameter State
	// ══════════════════════════════════════════════════════════════
	juce::AudioProcessorValueTreeState parameters;
	juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

	double currentSampleRate = 44100.0;
	int currentBlockSize = 512;

	// Cached raw parameter pointers (resolved once in prepareToPlay, avoids hash lookup per block)
	std::atomic<float>* pEnableA = nullptr;
	std::atomic<float>* pEnableB = nullptr;
	std::atomic<float>* pEnableC = nullptr;
	std::atomic<float>* pRoute = nullptr;
	std::atomic<float>* pMix = nullptr;
	std::atomic<float>* pInput = nullptr;
	std::atomic<float>* pOutput = nullptr;
	std::atomic<float>* pHpFreqA = nullptr;
	std::atomic<float>* pLpFreqA = nullptr;
	std::atomic<float>* pHpOnA = nullptr;
	std::atomic<float>* pLpOnA = nullptr;
	std::atomic<float>* pHpSlopeA = nullptr;
	std::atomic<float>* pLpSlopeA = nullptr;
	std::atomic<float>* pSeriesA = nullptr;
	std::atomic<float>* pVarA = nullptr;
	std::atomic<float>* pPanA = nullptr;
	std::atomic<float>* pFredA = nullptr;
	std::atomic<float>* pPosA = nullptr;
	std::atomic<float>* pInA = nullptr;
	std::atomic<float>* pOutA = nullptr;
	std::atomic<float>* pTiltA = nullptr;
	std::atomic<float>* pHpFreqB = nullptr;
	std::atomic<float>* pLpFreqB = nullptr;
	std::atomic<float>* pHpOnB = nullptr;
	std::atomic<float>* pLpOnB = nullptr;
	std::atomic<float>* pHpSlopeB = nullptr;
	std::atomic<float>* pLpSlopeB = nullptr;
	std::atomic<float>* pSeriesB = nullptr;
	std::atomic<float>* pVarB = nullptr;
	std::atomic<float>* pPanB = nullptr;
	std::atomic<float>* pFredB = nullptr;
	std::atomic<float>* pPosB = nullptr;
	std::atomic<float>* pInB = nullptr;
	std::atomic<float>* pOutB = nullptr;
	std::atomic<float>* pTiltB = nullptr;
	std::atomic<float>* pChaosA = nullptr;
	std::atomic<float>* pChaosFilterA = nullptr;
	std::atomic<float>* pChaosAmtA = nullptr;
	std::atomic<float>* pChaosSpdA = nullptr;
	std::atomic<float>* pChaosAmtFilterA = nullptr;
	std::atomic<float>* pChaosSpdFilterA = nullptr;
	std::atomic<float>* pModeInA = nullptr;
	std::atomic<float>* pModeOutA = nullptr;
	std::atomic<float>* pSumBusA = nullptr;
	std::atomic<float>* pFilterPosA = nullptr;
	std::atomic<float>* pChaosB = nullptr;
	std::atomic<float>* pChaosFilterB = nullptr;
	std::atomic<float>* pChaosAmtB = nullptr;
	std::atomic<float>* pChaosSpdB = nullptr;
	std::atomic<float>* pChaosAmtFilterB = nullptr;
	std::atomic<float>* pChaosSpdFilterB = nullptr;
	std::atomic<float>* pModeInB = nullptr;
	std::atomic<float>* pModeOutB = nullptr;
	std::atomic<float>* pSumBusB = nullptr;
	std::atomic<float>* pFilterPosB = nullptr;
	std::atomic<float>* pMixA = nullptr;
	std::atomic<float>* pMixB = nullptr;
	std::atomic<float>* pHpFreqC = nullptr;
	std::atomic<float>* pLpFreqC = nullptr;
	std::atomic<float>* pHpOnC = nullptr;
	std::atomic<float>* pLpOnC = nullptr;
	std::atomic<float>* pHpSlopeC = nullptr;
	std::atomic<float>* pLpSlopeC = nullptr;
	std::atomic<float>* pSeriesC = nullptr;
	std::atomic<float>* pVarC = nullptr;
	std::atomic<float>* pPanC = nullptr;
	std::atomic<float>* pFredC = nullptr;
	std::atomic<float>* pPosC = nullptr;
	std::atomic<float>* pInC = nullptr;
	std::atomic<float>* pOutC = nullptr;
	std::atomic<float>* pTiltC = nullptr;
	std::atomic<float>* pChaosC = nullptr;
	std::atomic<float>* pChaosFilterC = nullptr;
	std::atomic<float>* pChaosAmtC = nullptr;
	std::atomic<float>* pChaosSpdC = nullptr;
	std::atomic<float>* pChaosAmtFilterC = nullptr;
	std::atomic<float>* pChaosSpdFilterC = nullptr;
	std::atomic<float>* pModeInC = nullptr;
	std::atomic<float>* pModeOutC = nullptr;
	std::atomic<float>* pSumBusC = nullptr;
	std::atomic<float>* pFilterPosC = nullptr;
	std::atomic<float>* pMixC = nullptr;
	std::atomic<float>* pMatch = nullptr;
	std::atomic<float>* pTrim  = nullptr;
	std::atomic<float>* pLimThreshold = nullptr;
	std::atomic<float>* pLimMode     = nullptr;
	std::atomic<float>* pInvPol      = nullptr;
	std::atomic<float>* pInvStr      = nullptr;
	std::atomic<float>* pMixMode     = nullptr;
	std::atomic<float>* pDryLevel    = nullptr;
	std::atomic<float>* pWetLevel    = nullptr;

	// Saturation cached pointers
	std::atomic<float>* pSatTypeA  = nullptr;
	std::atomic<float>* pSatDriveA = nullptr;
	std::atomic<float>* pSatGirthA = nullptr;
	std::atomic<float>* pSatModA   = nullptr;
	std::atomic<float>* pSatBiasA  = nullptr;
	std::atomic<float>* pSatSagA   = nullptr;
	std::atomic<float>* pSatRawA   = nullptr;
	std::atomic<float>* pSatTypeB  = nullptr;
	std::atomic<float>* pSatDriveB = nullptr;
	std::atomic<float>* pSatGirthB = nullptr;
	std::atomic<float>* pSatModB   = nullptr;
	std::atomic<float>* pSatBiasB  = nullptr;
	std::atomic<float>* pSatSagB   = nullptr;
	std::atomic<float>* pSatRawB   = nullptr;
	std::atomic<float>* pSatTypeC  = nullptr;
	std::atomic<float>* pSatDriveC = nullptr;
	std::atomic<float>* pSatGirthC = nullptr;
	std::atomic<float>* pSatModC   = nullptr;
	std::atomic<float>* pSatBiasC  = nullptr;
	std::atomic<float>* pSatSagC   = nullptr;
	std::atomic<float>* pSatRawC   = nullptr;
	std::atomic<float>* pOversample = nullptr;
	std::atomic<float>* pDelayA  = nullptr;
	std::atomic<float>* pDelayB  = nullptr;
	std::atomic<float>* pDelayC  = nullptr;
	std::atomic<float>* pExpA       = nullptr;
	std::atomic<float>* pExpOrderA  = nullptr;
	std::atomic<float>* pExpRatioA  = nullptr;
	std::atomic<float>* pExpThreshA = nullptr;
	std::atomic<float>* pExpAtkA    = nullptr;
	std::atomic<float>* pExpRelA    = nullptr;
	std::atomic<float>* pExpB       = nullptr;
	std::atomic<float>* pExpOrderB  = nullptr;
	std::atomic<float>* pExpRatioB  = nullptr;
	std::atomic<float>* pExpThreshB = nullptr;
	std::atomic<float>* pExpAtkB    = nullptr;
	std::atomic<float>* pExpRelB    = nullptr;
	std::atomic<float>* pExpC       = nullptr;
	std::atomic<float>* pExpOrderC  = nullptr;
	std::atomic<float>* pExpRatioC  = nullptr;
	std::atomic<float>* pExpThreshC = nullptr;
	std::atomic<float>* pExpAtkC    = nullptr;
	std::atomic<float>* pExpRelC    = nullptr;

	// Oversampling (pre-allocated for all factors, per-loader)
	// [loaderIdx][factorIdx] where factorIdx: 0=x2, 1=x4, 2=x8, 3=x16
	std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers_[3][4];
	int currentOsOrder_ = 0;  // current oversampling order (0=off)

	// Tilt EQ filter state (1st-order shelf, per-channel)
	float tiltState_[2] = { 0.0f, 0.0f };
	int   tiltLastProfile_ = -1;
	float tiltLastSlope_   = 0.0f;  // last applied compensating slope
	float tiltB0_ = 1.0f, tiltB1_ = 0.0f, tiltA1_ = 0.0f;        // current (smoothed)
	float tiltTargetB0_ = 1.0f, tiltTargetB1_ = 0.0f, tiltTargetA1_ = 0.0f; // target

	// Wet NORM AGC state (peak follower + gain smoothing) — kept for TRIM
	float normPeakFollower_  = 0.0f;
	float normSmoothedGain_  = 1.0f;
	int   normWarmupSamples_ = 0;    // samples since peak follower activated

	// ── Dual-stage transparent peak limiter ──
	static constexpr float kLimFloor = 1.0e-12f;
	float limEnv1_[2] = { kLimFloor, kLimFloor };
	float limEnv2_[2] = { kLimFloor, kLimFloor };
	float limAtt1_  = 0.0f;
	float limRel1_  = 0.0f;
	float limRel2_  = 0.0f;

	inline void applyLimiter (float* leftData, float* rightData, int numSamples,
	                         float thresholdGain) noexcept
	{
		for (int i = 0; i < numSamples; ++i)
		{
			const float peakL = std::abs (leftData[i]);
			const float peakR = std::abs (rightData[i]);

			// Stage 1 — leveler (2 ms attack, 10 ms release)
			for (int ch = 0; ch < 2; ++ch)
			{
				const float p = (ch == 0) ? peakL : peakR;
				if (p > limEnv1_[ch])
					limEnv1_[ch] = limAtt1_ * limEnv1_[ch] + (1.0f - limAtt1_) * p;
				else
					limEnv1_[ch] = limRel1_ * limEnv1_[ch] + (1.0f - limRel1_) * p;
				if (limEnv1_[ch] < kLimFloor) limEnv1_[ch] = kLimFloor;
			}

			// Stage 2 — brickwall (instant attack, 100 ms release)
			for (int ch = 0; ch < 2; ++ch)
			{
				const float p = (ch == 0) ? peakL : peakR;
				if (p > limEnv2_[ch])
					limEnv2_[ch] = p;
				else
					limEnv2_[ch] = limRel2_ * limEnv2_[ch] + (1.0f - limRel2_) * p;
				if (limEnv2_[ch] < kLimFloor) limEnv2_[ch] = kLimFloor;
			}

			// Stereo-linked gain reduction
			float gr = 1.0f;
			const float maxEnv1 = juce::jmax (limEnv1_[0], limEnv1_[1]);
			const float maxEnv2 = juce::jmax (limEnv2_[0], limEnv2_[1]);
			if (maxEnv1 > thresholdGain)
				gr = juce::jmin (gr, thresholdGain / maxEnv1);
			if (maxEnv2 > thresholdGain)
				gr = juce::jmin (gr, thresholdGain / maxEnv2);

			leftData[i]  *= gr;
			rightData[i] *= gr;
		}
	}

	// Post-prepare fade-in to suppress convolver/filter warmup transients
	int fadeInSamplesRemaining_ = 0;
	int fadeInTotalSamples_     = 0;

	// ══════════════════════════════════════════════════════════════
	//  DSP Helpers — Reusable buffers to avoid allocations
	// ══════════════════════════════════════════════════════════════
	juce::AudioBuffer<float> tempBufferA;
	juce::AudioBuffer<float> tempBufferB;
	juce::AudioBuffer<float> tempBufferC;
	juce::AudioBuffer<float> globalDryBuffer;  // For global dry/wet MIX
	juce::AudioBuffer<float> loaderDryBuffer;  // For per-loader dry/wet MIX
	
	// Pre-computed coefficients (set once in prepareToPlay, avoids per-block std::exp)
	float cachedTiltSmoothCoeff_  = 0.0f;  // 1 - exp(-1/(sr*0.03))
	float cachedDcBlockR_         = 0.0f;  // 1 - 2*pi*5/sr
	float cachedNormFastCoeff_    = 0.0f;  // for NORM AGC 10ms tau
	float cachedNormSlowCoeff_    = 0.0f;  // for NORM AGC 20ms tau

	// DC blocking filter state (per-channel)
	float dcBlockX_[2] = { 0.0f, 0.0f };
	float dcBlockY_[2] = { 0.0f, 0.0f };

	// Auto-align (momentary trigger state)
	juce::int64 lastAlignTime_ = 0;

	// ══════════════════════════════════════════════════════════════
	//  Private Helper methods
	// ══════════════════════════════════════════════════════════════
	// loaderIndex: 0=A, 1=B, 2=C
	void processLoader (IRLoaderState& state, 
	                    juce::AudioBuffer<float>& buffer,
	                    int loaderIndex,
	                    bool skipAutoGain = false);

	// Delay for auto-align
	void applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, int loaderIndex);
	void calculateAutoAlignment();


	// Shared M/S encoding helper (used by processBlock + offline export)
	static void applyMidSideMode (juce::AudioBuffer<float>& buf, int modeVal, int numSamples);

	// Generic Hermite + Drift chaos engine (per-sample advance)
	inline void advanceChaosEngine (
		float& prev, float& curr, float& next, float& phase,
		float& driftPhase, float& driftFreqHz, float& output,
		juce::Random& rng, float period, float amtNorm, float sr) noexcept
	{
		phase += 1.0f;
		if (phase >= period)
		{
			phase -= period;
			prev = curr;
			curr = next;
			next = rng.nextFloat() * 2.0f - 1.0f;
			const float driftBase = sr / juce::jmax (1.0f, period) * 0.37f;
			driftFreqHz = driftBase * (0.88f + rng.nextFloat() * 0.24f);
		}
		const float t  = phase / period;
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
		const float h10 =         t3 - 2.0f * t2 + t;
		const float h01 = -2.0f * t3 + 3.0f * t2;
		const float h11 =         t3 -        t2;
		const float tangCurr = (next - prev) * 0.5f;
		const float tangNext = -curr * 0.5f;
		const float shValue  = h00 * curr + h10 * tangCurr + h01 * next + h11 * tangNext;

		driftPhase += driftFreqHz / sr;
		if (driftPhase > 1e6f) driftPhase -= 1e6f;
		const float driftValue = std::sin (driftPhase * kTwoPi) * kChaosDriftAmp;

		const float shWeight = juce::jlimit (0.0f, 1.0f, amtNorm * 1.5f - 0.15f);
		output = driftValue + shValue * shWeight;
	}

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CABTRAudioProcessor)
};

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"
#include "SatDspDiag.h"

// ---------------------------------------------------------------------------
// DSP utility functions (consistent with ECHO-TR)
// ---------------------------------------------------------------------------
namespace
{
	// Fast dB→linear conversion using exp2 instead of pow(10, dB/20).
	// Mathematically equivalent: 10^(dB/20) = 2^(dB * log2(10)/20) = 2^(dB * 0.16609640474)
	inline float fastDecibelsToGain (float dB) noexcept
	{
		return (dB <= -100.0f) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	// Relaxed atomic load helpers — safe for audio thread (single-writer GUI, single-reader audio).
	// Avoids unnecessary memory fences from default seq_cst ordering.
	inline float loadRelaxed (std::atomic<float>* p, float def = 0.0f) noexcept
	{
		return p != nullptr ? p->load (std::memory_order_relaxed) : def;
	}
	inline bool loadRelaxedBool (std::atomic<float>* p, bool def = false) noexcept
	{
		return loadRelaxed (p, def ? 1.0f : 0.0f) > 0.5f;
	}
	inline int loadRelaxedInt (std::atomic<float>* p, int def = 0) noexcept
	{
		return static_cast<int> (std::lround (loadRelaxed (p, static_cast<float> (def))));
	}

	// Compute 1st-order symmetric tilt shelf coefficients (bilinear, pivot 1kHz).
	// Shared by per-loader tilt EQ and global MATCH tilt EQ.
	inline void computeTiltShelfCoeffs (double sampleRate, float slopeDb,
	                                    float& outB0, float& outB1, float& outA1) noexcept
	{
		if (std::abs (slopeDb) < 0.1f)
		{
			outB0 = 1.0f; outB1 = 0.0f; outA1 = 0.0f;
			return;
		}
		const double pivot = 1000.0;
		const double octavesToNyquist = std::log2 ((sampleRate * 0.5) / pivot);
		const double gainAtNyquistDb  = static_cast<double> (slopeDb) * octavesToNyquist;
		const double gNy = std::pow (10.0, gainAtNyquistDb / 20.0);
		const double wc = 2.0 * sampleRate * std::tan (juce::MathConstants<double>::pi * pivot / sampleRate);
		const double K  = wc / (2.0 * sampleRate);
		const double g  = std::sqrt (gNy);
		const double norm = 1.0 / (1.0 + K * g);
		outB0 = static_cast<float> ((g + K) * norm);
		outB1 = static_cast<float> ((K - g) * norm);
		outA1 = static_cast<float> ((K * g - 1.0) * norm);
	}
}

//==============================================================================
CABTRAudioProcessor::CABTRAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#else
     :
#endif
      parameters (*this, nullptr, juce::Identifier ("CABTRState"), createParameterLayout())
{
	startTimer (200);
}

CABTRAudioProcessor::~CABTRAudioProcessor()
{
}

//==============================================================================
const juce::String CABTRAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool CABTRAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool CABTRAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool CABTRAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double CABTRAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int CABTRAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int CABTRAudioProcessor::getCurrentProgram()
{
    return 0;
}

void CABTRAudioProcessor::setCurrentProgram (int index)
{
	juce::ignoreUnused (index);
}

const juce::String CABTRAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void CABTRAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
	juce::ignoreUnused (index, newName);
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CABTRAudioProcessor::createParameterLayout()
{
	juce::AudioProcessorValueTreeState::ParameterLayout layout;

	// ══════════════════════════════════════════════════════════════
	//  Loader A Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableA, "Enable A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqA, "HP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqA, "LP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnA, "HP On A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnA, "LP On A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeA, "HP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeA, "LP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInA, "In A",
		juce::NormalisableRange<float> (kInMin, kInMax, 0.0f, 2.5f),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutA, "Out A", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltA, "Tilt A",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	// Legacy IR compatibility parameters kept only for old sessions/presets.
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartA, "Start A",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndA, "End A",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeA, "Size A", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesA, "Series A",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVarA, "Variation A",
		juce::NormalisableRange<float> (kVarMin, kVarMax, 0.001f),
		kVarDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanA, "Pan A", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredA, "Angle A", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosA, "Distance A", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoA, "Reso A",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvA, "Invert A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayA, "Delay A",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.5f), kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosA, "Chaos D A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterA, "Chaos F A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtA, "Chaos Amount A",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdA, "Chaos Speed A",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterA, "Chaos Filter Amount A",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterA, "Chaos Filter Speed A",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInA, "Mode In A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutA, "Mode Out A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusA, "Sum Bus A", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosA, "Filter Pos A",
		juce::StringArray { juce::String::fromUTF8 (u8"F\u25bc T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25bc T\u25b2") },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixA, "Mix A", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSatTypeA, "Sat Type A",
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TUBE", "TRANSISTOR", "DIODE", "TUNDRA", "FUZZ", "DOOM", "DESTROY", "CLIPPER" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveA, "Sat Drive A",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatGirthA, "Sat Girth A",
		juce::NormalisableRange<float> (kSatGirthMin, kSatGirthMax, 0.001f), kSatGirthDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatModA, "Sat Mod A",
		juce::NormalisableRange<float> (kSatModMin, kSatModMax, 0.001f), kSatModDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasA, "Sat Bias A",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagA, "Sat Sag A",
		juce::NormalisableRange<float> (kSatSagMin, kSatSagMax, 0.001f), kSatSagDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSatRawA, "Sat Raw A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpA, "Expander A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderA, "Exp Order A", false));  // false=PRE, true=POST
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioA, "Exp Ratio A",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshA, "Exp Thresh A",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkA, "Exp Atk A",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelA, "Exp Rel A",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.1f, 0.3f), kExpRelDefault));

	// ══════════════════════════════════════════════════════════════
	//  Loader B Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableB, "Enable B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqB, "HP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqB, "LP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnB, "HP On B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnB, "LP On B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeB, "HP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeB, "LP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInB, "In B",
		juce::NormalisableRange<float> (kInMin, kInMax, 0.0f, 2.5f),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutB, "Out B", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltB, "Tilt B",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	// Legacy IR compatibility parameters kept only for old sessions/presets.
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartB, "Start B",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndB, "End B",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeB, "Size B", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesB, "Series B",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVarB, "Variation B",
		juce::NormalisableRange<float> (kVarMin, kVarMax, 0.001f),
		kVarDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanB, "Pan B", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredB, "Angle B", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosB, "Distance B", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoB, "Reso B",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvB, "Invert B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayB, "Delay B",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.5f), kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosB, "Chaos D B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterB, "Chaos F B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtB, "Chaos Amount B",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdB, "Chaos Speed B",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterB, "Chaos Filter Amount B",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterB, "Chaos Filter Speed B",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInB, "Mode In B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutB, "Mode Out B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusB, "Sum Bus B", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosB, "Filter Pos B",
		juce::StringArray { juce::String::fromUTF8 (u8"F\u25bc T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25bc T\u25b2") },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixB, "Mix B", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSatTypeB, "Sat Type B",
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TUBE", "TRANSISTOR", "DIODE", "TUNDRA", "FUZZ", "DOOM", "DESTROY", "CLIPPER" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveB, "Sat Drive B",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatGirthB, "Sat Girth B",
		juce::NormalisableRange<float> (kSatGirthMin, kSatGirthMax, 0.001f), kSatGirthDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatModB, "Sat Mod B",
		juce::NormalisableRange<float> (kSatModMin, kSatModMax, 0.001f), kSatModDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasB, "Sat Bias B",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagB, "Sat Sag B",
		juce::NormalisableRange<float> (kSatSagMin, kSatSagMax, 0.001f), kSatSagDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSatRawB, "Sat Raw B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpB, "Expander B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderB, "Exp Order B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioB, "Exp Ratio B",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshB, "Exp Thresh B",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkB, "Exp Atk B",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelB, "Exp Rel B",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.1f, 0.3f), kExpRelDefault));

	// ══════════════════════════════════════════════════════════════
	//  Loader C Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableC, "Enable C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqC, "HP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqC, "LP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnC, "HP On C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnC, "LP On C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeC, "HP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeC, "LP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInC, "In C",
		juce::NormalisableRange<float> (kInMin, kInMax, 0.0f, 2.5f),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutC, "Out C", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltC, "Tilt C",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	// Legacy IR compatibility parameters kept only for old sessions/presets.
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartC, "Start C",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f),
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndC, "End C",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f),
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeC, "Size C", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesC, "Series C",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVarC, "Variation C",
		juce::NormalisableRange<float> (kVarMin, kVarMax, 0.001f),
		kVarDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanC, "Pan C", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredC, "Angle C", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosC, "Distance C", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoC, "Reso C",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvC, "Invert C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayC, "Delay C",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.5f), kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosC, "Chaos D C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterC, "Chaos F C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtC, "Chaos Amount C",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdC, "Chaos Speed C",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterC, "Chaos Filter Amount C",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterC, "Chaos Filter Speed C",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInC, "Mode In C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutC, "Mode Out C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusC, "Sum Bus C", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosC, "Filter Pos C",
		juce::StringArray { juce::String::fromUTF8 (u8"F\u25bc T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25bc T\u25b2") },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixC, "Mix C", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSatTypeC, "Sat Type C",
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TUBE", "TRANSISTOR", "DIODE", "TUNDRA", "FUZZ", "DOOM", "DESTROY", "CLIPPER" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveC, "Sat Drive C",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatGirthC, "Sat Girth C",
		juce::NormalisableRange<float> (kSatGirthMin, kSatGirthMax, 0.001f), kSatGirthDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatModC, "Sat Mod C",
		juce::NormalisableRange<float> (kSatModMin, kSatModMax, 0.001f), kSatModDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasC, "Sat Bias C",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagC, "Sat Sag C",
		juce::NormalisableRange<float> (kSatSagMin, kSatSagMax, 0.001f), kSatSagDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSatRawC, "Sat Raw C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpC, "Expander C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderC, "Exp Order C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioC, "Exp Ratio C",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshC, "Exp Thresh C",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkC, "Exp Atk C",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelC, "Exp Rel C",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.1f, 0.3f), kExpRelDefault));

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input", kInputMin, kInputMax, kInputDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output", kOutputMin, kOutputMax, kOutputDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamRoute, "Route", juce::StringArray { "A>B>C", "A|B|C", "A>B|C", "A|B>C" }, kRouteDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// Mix Mode (INSERT / SEND) + Dry/Wet levels for SEND mode
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMixMode, "Mix Mode",
		juce::StringArray { "INSERT", "SEND" }, kMixModeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDryLevel, "Dry Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kDryLevelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamWetLevel, "Wet Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kWetLevelDefault));

	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMatch, "Match", juce::StringArray { "None", "White", "Pink (-3dB)", "Brown (-6dB)", "Bright (+3dB)", "Bright+ (+6dB)" }, kMatchDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamTrim, "Norm", juce::StringArray { "Off", "0 dB", "-3 dB", "-6 dB", "-12 dB", "-18 dB" }, kTrimDefault));

	// Limiter
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLimThreshold, "Lim Threshold",
		juce::NormalisableRange<float> (kLimThresholdMin, kLimThresholdMax, 0.1f), kLimThresholdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamLimMode, "Lim Mode", juce::StringArray { "NONE", "WET", "GLOBAL" }, kLimModeDefault));

	// Invert Polarity / Invert Stereo
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamInvPol, "Invert Polarity",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvPolDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamInvStr, "Invert Stereo",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvStrDefault));

	// Oversampling
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamOversample, "Oversampling",
		juce::StringArray { "x1", "x2", "x4", "x8", "x16" }, kOversampleDefault));

	// Auto-align (momentary trigger)
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamAlign, "Align", false));

	// ══════════════════════════════════════════════════════════════
	//  UI State Parameters (hidden from automation)
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiWidth, 1 }, "UI Width", 400, 2000, 800,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiHeight, 1 }, "UI Height", 300, 1500, 600,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiPalette, 1 }, "UI Palette", 0, 1, 0,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterBool> (
		juce::ParameterID { kParamUiFxTail, 1 }, "UI FX Tail", false,
		juce::AudioParameterBoolAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor0, 1 }, "UI Color 0", 0, 0xFFFFFF, 0x00FF00,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor1, 1 }, "UI Color 1", 0, 0xFFFFFF, 0x000000,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	return layout;
}

//==============================================================================
void CABTRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	currentSampleRate = sampleRate;
	currentBlockSize = samplesPerBlock;
	
	LOG_IR_EVENT ("prepareToPlay: sr=" + juce::String (sampleRate) + 
	              " blockSize=" + juce::String (samplesPerBlock));

	juce::dsp::ProcessSpec spec;
	spec.sampleRate = sampleRate;
	spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
	spec.numChannels = 2;

	stateA.hpFilter.prepare (spec);
	stateA.hpFilter2.prepare (spec);
	stateA.lpFilter.prepare (spec);
	stateA.lpFilter2.prepare (spec);
	stateB.hpFilter.prepare (spec);
	stateB.hpFilter2.prepare (spec);
	stateB.lpFilter.prepare (spec);
	stateB.lpFilter2.prepare (spec);
	stateC.hpFilter.prepare (spec);
	stateC.hpFilter2.prepare (spec);
	stateC.lpFilter.prepare (spec);
	stateC.lpFilter2.prepare (spec);
	stateA.posFilter.prepare (spec);
	stateB.posFilter.prepare (spec);
	stateC.posFilter.prepare (spec);

	// Prepare delay lines for auto-align
	stateA.delayLine.prepare (spec);
	stateB.delayLine.prepare (spec);
	stateC.delayLine.prepare (spec);
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	stateA.smoothedDelay.reset (sampleRate, 0.05);
	stateB.smoothedDelay.reset (sampleRate, 0.05);
	stateC.smoothedDelay.reset (sampleRate, 0.05);
	stateA.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateB.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateC.smoothedDelay.setCurrentAndTargetValue (0.0f);
	
	// Prepare temp buffers (pre-allocated for audio thread)
	// Use generous size to handle hosts that send variable/larger blocks
	// (e.g. FL Studio). Runtime guard in processBlock handles anything larger.
	const int bufAlloc = juce::jmax (samplesPerBlock, 8192);
	tempBufferA.setSize (2, bufAlloc);
	tempBufferB.setSize (2, bufAlloc);
	tempBufferC.setSize (2, bufAlloc);
	globalDryBuffer.setSize (2, bufAlloc);
	loaderDryBuffer.setSize (2, bufAlloc);

	// Cache raw parameter pointers (avoids hash-table lookup every processBlock)
	pEnableA = parameters.getRawParameterValue (kParamEnableA);
	pEnableB = parameters.getRawParameterValue (kParamEnableB);
	pRoute   = parameters.getRawParameterValue (kParamRoute);
	pMix     = parameters.getRawParameterValue (kParamMix);
	pInput   = parameters.getRawParameterValue (kParamInput);
	pOutput  = parameters.getRawParameterValue (kParamOutput);
	pHpFreqA = parameters.getRawParameterValue (kParamHpFreqA);
	pLpFreqA = parameters.getRawParameterValue (kParamLpFreqA);
	pHpOnA   = parameters.getRawParameterValue (kParamHpOnA);
	pLpOnA   = parameters.getRawParameterValue (kParamLpOnA);
	pHpSlopeA = parameters.getRawParameterValue (kParamHpSlopeA);
	pLpSlopeA = parameters.getRawParameterValue (kParamLpSlopeA);
	pSeriesA = parameters.getRawParameterValue (kParamSeriesA);
	pVarA    = parameters.getRawParameterValue (kParamVarA);
	pPanA    = parameters.getRawParameterValue (kParamPanA);
	pFredA   = parameters.getRawParameterValue (kParamFredA);
	pPosA    = parameters.getRawParameterValue (kParamPosA);
	pOutA    = parameters.getRawParameterValue (kParamOutA);
	pInA     = parameters.getRawParameterValue (kParamInA);
	pTiltA   = parameters.getRawParameterValue (kParamTiltA);
	pHpFreqB = parameters.getRawParameterValue (kParamHpFreqB);
	pLpFreqB = parameters.getRawParameterValue (kParamLpFreqB);
	pHpOnB   = parameters.getRawParameterValue (kParamHpOnB);
	pLpOnB   = parameters.getRawParameterValue (kParamLpOnB);
	pHpSlopeB = parameters.getRawParameterValue (kParamHpSlopeB);
	pLpSlopeB = parameters.getRawParameterValue (kParamLpSlopeB);
	pSeriesB = parameters.getRawParameterValue (kParamSeriesB);
	pVarB    = parameters.getRawParameterValue (kParamVarB);
	pPanB    = parameters.getRawParameterValue (kParamPanB);
	pFredB   = parameters.getRawParameterValue (kParamFredB);
	pPosB    = parameters.getRawParameterValue (kParamPosB);
	pOutB    = parameters.getRawParameterValue (kParamOutB);
	pInB     = parameters.getRawParameterValue (kParamInB);
	pTiltB   = parameters.getRawParameterValue (kParamTiltB);
	pChaosA    = parameters.getRawParameterValue (kParamChaosA);
	pChaosFilterA = parameters.getRawParameterValue (kParamChaosFilterA);
	pChaosAmtA = parameters.getRawParameterValue (kParamChaosAmtA);
	pChaosSpdA = parameters.getRawParameterValue (kParamChaosSpdA);
	pChaosAmtFilterA = parameters.getRawParameterValue (kParamChaosAmtFilterA);
	pChaosSpdFilterA = parameters.getRawParameterValue (kParamChaosSpdFilterA);
	pModeInA   = parameters.getRawParameterValue (kParamModeInA);
	pModeOutA  = parameters.getRawParameterValue (kParamModeOutA);
	pSumBusA   = parameters.getRawParameterValue (kParamSumBusA);
	pFilterPosA = parameters.getRawParameterValue (kParamFilterPosA);
	pChaosB    = parameters.getRawParameterValue (kParamChaosB);
	pChaosFilterB = parameters.getRawParameterValue (kParamChaosFilterB);
	pChaosAmtB = parameters.getRawParameterValue (kParamChaosAmtB);
	pChaosSpdB = parameters.getRawParameterValue (kParamChaosSpdB);
	pChaosAmtFilterB = parameters.getRawParameterValue (kParamChaosAmtFilterB);
	pChaosSpdFilterB = parameters.getRawParameterValue (kParamChaosSpdFilterB);
	pModeInB   = parameters.getRawParameterValue (kParamModeInB);
	pModeOutB  = parameters.getRawParameterValue (kParamModeOutB);
	pSumBusB   = parameters.getRawParameterValue (kParamSumBusB);
	pFilterPosB = parameters.getRawParameterValue (kParamFilterPosB);
	pMixA      = parameters.getRawParameterValue (kParamMixA);
	pMixB      = parameters.getRawParameterValue (kParamMixB);
	pEnableC   = parameters.getRawParameterValue (kParamEnableC);
	pHpFreqC   = parameters.getRawParameterValue (kParamHpFreqC);
	pLpFreqC   = parameters.getRawParameterValue (kParamLpFreqC);
	pHpOnC     = parameters.getRawParameterValue (kParamHpOnC);
	pLpOnC     = parameters.getRawParameterValue (kParamLpOnC);
	pHpSlopeC  = parameters.getRawParameterValue (kParamHpSlopeC);
	pLpSlopeC  = parameters.getRawParameterValue (kParamLpSlopeC);
	pSeriesC   = parameters.getRawParameterValue (kParamSeriesC);
	pVarC      = parameters.getRawParameterValue (kParamVarC);
	pPanC      = parameters.getRawParameterValue (kParamPanC);
	pFredC     = parameters.getRawParameterValue (kParamFredC);
	pPosC      = parameters.getRawParameterValue (kParamPosC);
	pOutC      = parameters.getRawParameterValue (kParamOutC);
	pInC       = parameters.getRawParameterValue (kParamInC);
	pTiltC     = parameters.getRawParameterValue (kParamTiltC);
	pChaosC    = parameters.getRawParameterValue (kParamChaosC);
	pChaosFilterC = parameters.getRawParameterValue (kParamChaosFilterC);
	pChaosAmtC = parameters.getRawParameterValue (kParamChaosAmtC);
	pChaosSpdC = parameters.getRawParameterValue (kParamChaosSpdC);
	pChaosAmtFilterC = parameters.getRawParameterValue (kParamChaosAmtFilterC);
	pChaosSpdFilterC = parameters.getRawParameterValue (kParamChaosSpdFilterC);
	pModeInC   = parameters.getRawParameterValue (kParamModeInC);
	pModeOutC  = parameters.getRawParameterValue (kParamModeOutC);
	pSumBusC   = parameters.getRawParameterValue (kParamSumBusC);
	pFilterPosC = parameters.getRawParameterValue (kParamFilterPosC);
	pMixC      = parameters.getRawParameterValue (kParamMixC);
	pMatch     = parameters.getRawParameterValue (kParamMatch);
	pTrim      = parameters.getRawParameterValue (kParamTrim);
	pLimThreshold = parameters.getRawParameterValue (kParamLimThreshold);
	pLimMode     = parameters.getRawParameterValue (kParamLimMode);
	pInvPol      = parameters.getRawParameterValue (kParamInvPol);
	pInvStr      = parameters.getRawParameterValue (kParamInvStr);
	pMixMode     = parameters.getRawParameterValue (kParamMixMode);
	pDryLevel    = parameters.getRawParameterValue (kParamDryLevel);
	pWetLevel    = parameters.getRawParameterValue (kParamWetLevel);

	// Saturation parameter pointers
	pSatTypeA  = parameters.getRawParameterValue (kParamSatTypeA);
	pSatDriveA = parameters.getRawParameterValue (kParamSatDriveA);
	pSatGirthA = parameters.getRawParameterValue (kParamSatGirthA);
	pSatModA   = parameters.getRawParameterValue (kParamSatModA);
	pSatBiasA  = parameters.getRawParameterValue (kParamSatBiasA);
	pSatSagA   = parameters.getRawParameterValue (kParamSatSagA);
	pSatRawA   = parameters.getRawParameterValue (kParamSatRawA);
	pSatTypeB  = parameters.getRawParameterValue (kParamSatTypeB);
	pSatDriveB = parameters.getRawParameterValue (kParamSatDriveB);
	pSatGirthB = parameters.getRawParameterValue (kParamSatGirthB);
	pSatModB   = parameters.getRawParameterValue (kParamSatModB);
	pSatBiasB  = parameters.getRawParameterValue (kParamSatBiasB);
	pSatSagB   = parameters.getRawParameterValue (kParamSatSagB);
	pSatRawB   = parameters.getRawParameterValue (kParamSatRawB);
	pSatTypeC  = parameters.getRawParameterValue (kParamSatTypeC);
	pSatDriveC = parameters.getRawParameterValue (kParamSatDriveC);
	pSatGirthC = parameters.getRawParameterValue (kParamSatGirthC);
	pSatModC   = parameters.getRawParameterValue (kParamSatModC);
	pSatBiasC  = parameters.getRawParameterValue (kParamSatBiasC);
	pSatSagC   = parameters.getRawParameterValue (kParamSatSagC);
	pSatRawC   = parameters.getRawParameterValue (kParamSatRawC);
	pOversample = parameters.getRawParameterValue (kParamOversample);
	pDelayA  = parameters.getRawParameterValue (kParamDelayA);
	pDelayB  = parameters.getRawParameterValue (kParamDelayB);
	pDelayC  = parameters.getRawParameterValue (kParamDelayC);
	pExpA       = parameters.getRawParameterValue (kParamExpA);
	pExpOrderA  = parameters.getRawParameterValue (kParamExpOrderA);
	pExpRatioA  = parameters.getRawParameterValue (kParamExpRatioA);
	pExpThreshA = parameters.getRawParameterValue (kParamExpThreshA);
	pExpAtkA    = parameters.getRawParameterValue (kParamExpAtkA);
	pExpRelA    = parameters.getRawParameterValue (kParamExpRelA);
	pExpB       = parameters.getRawParameterValue (kParamExpB);
	pExpOrderB  = parameters.getRawParameterValue (kParamExpOrderB);
	pExpRatioB  = parameters.getRawParameterValue (kParamExpRatioB);
	pExpThreshB = parameters.getRawParameterValue (kParamExpThreshB);
	pExpAtkB    = parameters.getRawParameterValue (kParamExpAtkB);
	pExpRelB    = parameters.getRawParameterValue (kParamExpRelB);
	pExpC       = parameters.getRawParameterValue (kParamExpC);
	pExpOrderC  = parameters.getRawParameterValue (kParamExpOrderC);
	pExpRatioC  = parameters.getRawParameterValue (kParamExpRatioC);
	pExpThreshC = parameters.getRawParameterValue (kParamExpThreshC);
	pExpAtkC    = parameters.getRawParameterValue (kParamExpAtkC);
	pExpRelC    = parameters.getRawParameterValue (kParamExpRelC);

	// Reset tilt EQ state
	tiltState_[0] = tiltState_[1] = 0.0f;
	tiltLastProfile_ = -1;

	// Reset wet NORM AGC state
	normPeakFollower_  = 0.0f;
	normSmoothedGain_  = 1.0f;
	normWarmupSamples_ = 0;

	// Limiter state reset
	limEnv1_[0] = limEnv1_[1] = kLimFloor;
	limEnv2_[0] = limEnv2_[1] = kLimFloor;
	limAtt1_ = std::exp (-1.0f / ((float) sampleRate * 0.002f));
	limRel1_ = std::exp (-1.0f / ((float) sampleRate * 0.010f));
	limRel2_ = std::exp (-1.0f / ((float) sampleRate * 0.100f));

	// Pre-compute coefficients that depend only on sample rate (avoids per-block std::exp)
	{
		const float sr = static_cast<float> (sampleRate);
		cachedTiltSmoothCoeff_ = 1.0f - std::exp (-1.0f / (sr * 0.03f));
		cachedDcBlockR_        = 1.0f - (juce::MathConstants<float>::twoPi * 5.0f / sr);
		// NORM AGC: per-block coefficients depend on numSamples, but the base tau is fixed.
		// We store the per-sample versions; processBlock scales by numSamples.
		cachedNormFastCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.01f)); // 10ms ramp-down
		cachedNormSlowCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.02f)); // 20ms ramp-up
	}

	// Fade-in to suppress startup filter/modulation transients (~5ms)
	fadeInTotalSamples_     = juce::jmax (64, (int) (sampleRate * 0.005));
	fadeInSamplesRemaining_ = fadeInTotalSamples_;

	// Reset FRED and CHAOS state for all loaders
	for (auto* state : { &stateA, &stateB, &stateC })
	{
		std::memset (state->fredDelayBuffer, 0, sizeof (state->fredDelayBuffer));
		state->fredDelayIndex = 0;
		std::memset (state->chaosDelayBuffer, 0, sizeof (state->chaosDelayBuffer));
		state->chaosDelayWritePos = 0;
		for (int c = 0; c < 2; ++c)
		{
			state->chaosDPrev[c] = state->chaosDCurr[c] = state->chaosDNext[c] = 0.0f;
			state->chaosDPhase[c] = state->chaosDDriftPhase[c] = state->chaosDDriftFreqHz[c] = 0.0f;
			state->chaosDOut[c] = 0.0f;
			state->chaosGPrev[c] = state->chaosGCurr[c] = state->chaosGNext[c] = 0.0f;
			state->chaosGPhase[c] = state->chaosGDriftPhase[c] = state->chaosGDriftFreqHz[c] = 0.0f;
			state->chaosGOut[c] = 0.0f;
		}
		state->chaosFPrev = state->chaosFCurr = state->chaosFNext = 0.0f;
		state->chaosFPhase = state->chaosFDriftPhase = state->chaosFDriftFreqHz = 0.0f;
		state->chaosFOut[0] = state->chaosFOut[1] = 0.0f;
		state->smoothedPosFreq = 12000.0f;
		state->filterCoeffCountdown = 0;
	}

	// Initialize EMA-smoothed filter frequencies to current parameter values
	std::atomic<float>* hpPtrs[] = { pHpFreqA, pHpFreqB, pHpFreqC };
	std::atomic<float>* lpPtrs[] = { pLpFreqA, pLpFreqB, pLpFreqC };
	IRLoaderState* states[] = { &stateA, &stateB, &stateC };
	for (int i = 0; i < 3; ++i)
	{
		states[i]->smoothedHpFreq = hpPtrs[i]->load();
		states[i]->smoothedLpFreq = lpPtrs[i]->load();
		states[i]->satState.reset();
	}

	// Initialize oversampling objects (all factors × all loaders)
	for (int ldr = 0; ldr < 3; ++ldr)
	{
		for (int order = 1; order <= 4; ++order)
		{
			oversamplers_[ldr][order - 1] = std::make_unique<juce::dsp::Oversampling<float>> (
				2, (size_t) order,
				juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
			oversamplers_[ldr][order - 1]->initProcessing ((size_t) samplesPerBlock);
		}
	}
	currentOsOrder_ = 0;
}

void CABTRAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool CABTRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//==============================================================================
// DSP NOTES:
//
// 1. MODE PROCESSING: Mid/Side conversion
//    - MID = (L+R) / sqrt(2)  — preserves RMS energy
//    - SIDE = (L-R) / sqrt(2)
//    - L+R = standard stereo pass-through
//
// 2. ROUTING:
//    - PARALLEL (A|B|C): Independent processing, summed output
//    - SERIES (A→B→C): Output of one loader feeds the next
//    - HYBRID: A→B|C and A|B→C
//
// 3. SIMD OPTIMIZATION: FloatVectorOperations for buffer operations
//    - applyGain, multiply, add use SIMD when available
//    - Significant speedup on modern CPUs
//==============================================================================

static inline void injectMSBus (float l, float r, int bus,
                                float& stL, float& stR,
                                float& midBus, float& sideBus)
{
	if (bus == 0)      { stL += l; stR += r; }
	else if (bus == 1) { midBus += (l + r) * 0.5f; }
	else               { sideBus += (l - r) * 0.5f; }
}

void CABTRAudioProcessor::applyMidSideMode (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
{
	if ((modeVal == 1 || modeVal == 2) && buf.getNumChannels() >= 2)
	{
		auto* L = buf.getWritePointer (0);
		auto* R = buf.getWritePointer (1);
		for (int i = 0; i < nSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeVal == 1) // MID = (L+R) / sqrt(2)
			{
				const float mid = (l + r) * kSqrt2Over2;
				L[i] = R[i] = mid;
			}
			else // SIDE = (L-R) / sqrt(2)
			{
				const float side = (l - r) * kSqrt2Over2;
				L[i] = R[i] = side;
			}
		}
	}
}

void CABTRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;
	juce::ignoreUnused (midiMessages);

#if SAT_DSP_DIAG
	const auto _diagStart = std::chrono::steady_clock::now();
	_diagCollector.reset();
#endif

	auto totalNumInputChannels  = getTotalNumInputChannels();
	auto totalNumOutputChannels = getTotalNumOutputChannels();

	// Clear unused output channels
	for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
		buffer.clear (i, 0, buffer.getNumSamples());

	const int numSamples = buffer.getNumSamples();
	if (numSamples == 0)
		return;

	// Resize work buffers to match actual block size each call.
	// avoidReallocating=true keeps existing allocation if big enough (zero-alloc path).
	// This is CRITICAL: the temp buffers must report the correct numSamples so that
	// processLoader (which reads buffer.getNumSamples()) only processes valid data.
	// Without this, stale data beyond numSamples creates a feedback loop.
	{
		const int nc = buffer.getNumChannels();
		tempBufferA.setSize  (nc, numSamples, false, false, true);
		tempBufferB.setSize  (nc, numSamples, false, false, true);
		tempBufferC.setSize  (nc, numSamples, false, false, true);
		globalDryBuffer.setSize (nc, numSamples, false, false, true);
		loaderDryBuffer.setSize (nc, numSamples, false, false, true);
	}

	// Get global parameters (cached pointers — relaxed atomic, no hash lookup)
	const bool enableA = loadRelaxedBool (pEnableA);
	const bool enableB = loadRelaxedBool (pEnableB);
	const bool enableC = loadRelaxedBool (pEnableC);

	// A loader is "active" when enabled (no IR dependency in SAT-TR)
	const bool activeA = enableA;
	const bool activeB = enableB;
	const bool activeC = enableC;
	const int diagLoaderIndex = activeC ? 2 : (activeB ? 1 : 0);
	float diagSatDeltaPeak = 0.0f;

	const int route = loadRelaxedInt (pRoute);
	const float globalMix = loadRelaxed (pMix);
	const int   mixMode   = loadRelaxedInt (pMixMode);
	const float dryLevel  = (mixMode == 1) ? loadRelaxed (pDryLevel) : 0.0f;
	const float wetLevel  = (mixMode == 1) ? loadRelaxed (pWetLevel) : 0.0f;

	// ── Limiter ──
	const int limMode = loadRelaxedInt (pLimMode);
	const float limThreshLin = (limMode != 0)
		? fastDecibelsToGain (loadRelaxed (pLimThreshold, kLimThresholdDefault))
		: 1.0f;

	const int invPol = loadRelaxedInt (pInvPol);
	const int invStr = loadRelaxedInt (pInvStr);

	// Per-loader mode parameters
	const int modeInA  = loadRelaxedInt (pModeInA);
	const int modeOutA = loadRelaxedInt (pModeOutA);
	const int modeInB  = loadRelaxedInt (pModeInB);
	const int modeOutB = loadRelaxedInt (pModeOutB);
	const int modeInC  = loadRelaxedInt (pModeInC);
	const int modeOutC = loadRelaxedInt (pModeOutC);
	const int sumBusA  = loadRelaxedInt (pSumBusA);
	const int sumBusB  = loadRelaxedInt (pSumBusB);
	const int sumBusC  = loadRelaxedInt (pSumBusC);
	const float mixA = loadRelaxed (pMixA);
	const float mixB = loadRelaxed (pMixB);
	const float mixC = loadRelaxed (pMixC);
	const auto satTypeA = SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeA)));
	const auto satTypeB = SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeB)));
	const auto satTypeC = SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeC)));
	const bool satRawA = loadRelaxedBool (pSatRawA);
	const bool satRawB = loadRelaxedBool (pSatRawB);
	const bool satRawC = loadRelaxedBool (pSatRawC);

	// Report oversampling latency to host (only when order changes)
	{
		const int osOrder = loadRelaxedInt (pOversample);
		if (osOrder != currentOsOrder_)
		{
			currentOsOrder_ = osOrder;
			if (osOrder > 0 && osOrder <= 4)
				setLatencySamples ((int) oversamplers_[0][osOrder - 1]->getLatencyInSamples());
			else
				setLatencySamples (0);
		}
	}

	// Apply input gain
	const float inputGain = fastDecibelsToGain (loadRelaxed (pInput));
	buffer.applyGain (inputGain);

	// ── DIAGNOSTIC LOG (throttled ~1s) ──
#if CABTR_DSP_DEBUG_LOG
	{
		static int diagBlockCount = 0;
		++diagBlockCount;
		// Log every ~1 second (sampleRate / numSamples = blocks per second)
		const int blocksPerSecond = juce::jmax (1, (int)(currentSampleRate / juce::jmax (1, numSamples)));
		if (diagBlockCount >= blocksPerSecond)
		{
			diagBlockCount = 0;

			// Peak levels of input (post input gain)
			float peakL = 0.0f, peakR = 0.0f;
			if (buffer.getNumChannels() >= 1)
				peakL = buffer.getMagnitude (0, 0, numSamples);
			if (buffer.getNumChannels() >= 2)
				peakR = buffer.getMagnitude (1, 0, numSamples);

			const float inputDb = loadRelaxed (pInput);
			const float outputDb = loadRelaxed (pOutput);
			const float outA_dB = loadRelaxed (pOutA);
			const float outB_dB = loadRelaxed (pOutB);
			const float outC_dB = loadRelaxed (pOutC);
			const float inA_dB = loadRelaxed (pInA);
			const float inB_dB = loadRelaxed (pInB);
			const float inC_dB = loadRelaxed (pInC);

			juce::String diag;
			diag << "BLOCK numSamples=" << numSamples
			     << " sRate=" << (int) currentSampleRate
			     << " ch=" << buffer.getNumChannels()
			     << " | inPeak L=" << juce::String (peakL, 4)
			     << " R=" << juce::String (peakR, 4)
			     << " | route=" << route
			     << " globalMix=" << juce::String (globalMix, 3)
			     << " | Input=" << juce::String (inputDb, 2) << "dB"
			     << " Output=" << juce::String (outputDb, 2) << "dB"
			     << " | enableA=" << (int) enableA << " activeA=" << (int) activeA
			     << " enableB=" << (int) enableB << " activeB=" << (int) activeB
			     << " enableC=" << (int) enableC << " activeC=" << (int) activeC
			     << " | InA=" << juce::String (inA_dB, 2) << " OutA=" << juce::String (outA_dB, 2)
			     << " InB=" << juce::String (inB_dB, 2) << " OutB=" << juce::String (outB_dB, 2)
			     << " InC=" << juce::String (inC_dB, 2) << " OutC=" << juce::String (outC_dB, 2)
			     << " | mixA=" << juce::String (mixA, 3) << " mixB=" << juce::String (mixB, 3) << " mixC=" << juce::String (mixC, 3)
			     << " | tempBufA=" << tempBufferA.getNumSamples() << " tempBufB=" << tempBufferB.getNumSamples();
			LOG_IR_EVENT (diag);
		}
	}
#endif

	// Capture dry signal AFTER input gain, but BEFORE any loader processing
	// Used for global MIX: dry is unaffected by convolution, filters, mode, etc.
	const bool needsDry = (mixMode == 1) ? true : (globalMix < 0.999f);
	if (needsDry)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			globalDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// ── Helper lambdas ──
	// Save dry copy before processing a loader (for per-loader mix)
	auto saveDry = [&] (const juce::AudioBuffer<float>& src)
	{
		for (int ch = 0; ch < src.getNumChannels(); ++ch)
			loaderDryBuffer.copyFrom (ch, 0, src, ch, 0, numSamples);
	};

	// Per-loader dry/wet blend (loaderDryBuffer must contain the pre-process signal)
	auto applyLoaderMix = [&] (juce::AudioBuffer<float>& buf, float mixVal)
	{
		if (mixVal < 0.999f)
		{
			const float wet = mixVal;
			const float dry = 1.0f - mixVal;
			for (int ch = 0; ch < buf.getNumChannels(); ++ch)
			{
				auto* wetData = buf.getWritePointer (ch);
				const auto* dryData = loaderDryBuffer.getReadPointer (ch);
				juce::FloatVectorOperations::multiply (wetData, wet, numSamples);
				juce::FloatVectorOperations::addWithMultiply (wetData, dryData, dry, numSamples);
			}
		}
	};

	// Process one loader with mode in/out and per-loader mix
	auto processOne = [&] (IRLoaderState& state, juce::AudioBuffer<float>& buf,
	                        int loaderIndex, int modeIn, int modeOut, float loaderMix,
	                        bool skipAutoGain = false)
	{
		saveDry (buf);
		applyMidSideMode (buf, modeIn, numSamples);
		processLoader (state, buf, loaderIndex, skipAutoGain);
		applyMidSideMode (buf, modeOut, numSamples);
		if (loaderIndex == diagLoaderIndex)
		{
			const int chCount = juce::jmin (buf.getNumChannels(), loaderDryBuffer.getNumChannels());
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float* wetData = buf.getReadPointer (ch);
				const float* dryData = loaderDryBuffer.getReadPointer (ch);
				for (int n = 0; n < numSamples; ++n)
				{
					const float d = std::abs (wetData[n] - dryData[n]);
					if (d > diagSatDeltaPeak)
						diagSatDeltaPeak = d;
				}
			}
		}
		applyLoaderMix (buf, loaderMix);
	};

	// Count how many loaders are active (for parallel compensation)
	auto countEnabled = [&] (bool a, bool b, bool c) -> int
	{
		return (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
	};

	// ── ROUTING ──
	// Route 0: A→B→C (full series)
	// Route 1: A|B|C  (all parallel)
	// Route 2: A→B|C  (series A→B, C parallel)
	// Route 3: A|B→C  (A parallel, series B→C)

	if (route == 1) // A|B|C — all parallel
	{
		const int numActive = countEnabled (activeA, activeB, activeC);
		if (numActive >= 2)
		{
			// Copy input into each temp buffer for active loaders
			if (activeA)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (activeB)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (activeC)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);

			if (activeA) processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);

			// Sum active buffers — M/S bus-aware
			const bool anyMSBus = (activeA && sumBusA != 0)
			                   || (activeB && sumBusB != 0)
			                   || (activeC && sumBusC != 0);

			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				// M/S bus routing: each loader contributes to ST, →M, or →S bus
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);

				const float* srcL[3] = { nullptr, nullptr, nullptr };
				const float* srcR[3] = { nullptr, nullptr, nullptr };
				int buses[3] = { sumBusA, sumBusB, sumBusC };
				bool active[3] = { activeA, activeB, activeC };

				if (activeA) { srcL[0] = tempBufferA.getReadPointer (0); srcR[0] = tempBufferA.getReadPointer (1); }
				if (activeB) { srcL[1] = tempBufferB.getReadPointer (0); srcR[1] = tempBufferB.getReadPointer (1); }
				if (activeC) { srcL[2] = tempBufferC.getReadPointer (0); srcR[2] = tempBufferC.getReadPointer (1); }

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f;
					float midBus = 0.0f, sideBus = 0.0f;

					for (int k = 0; k < 3; ++k)
					{
						if (!active[k]) continue;
						injectMSBus (srcL[k][i], srcR[k][i], buses[k],
						             stL, stR, midBus, sideBus);
					}

					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				// Fast path: all ST — simple L+R addition (no M/S overhead)
				buffer.clear();
				if (activeA)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
				if (activeB)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
				if (activeC)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
			}

			// Parallel compensation: 1/sqrt(N)
			buffer.applyGain (1.0f / std::sqrt (static_cast<float> (numActive)));
		}
		else if (activeA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		else if (activeB)
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		else if (activeC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 2) // A→B|C — series A→B, C parallel
	{
		const bool seriesActive = activeA || activeB;
		if (seriesActive && activeC)
		{
			// Copy input for parallel path C
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Series path: A→B stays in buffer (skip auto-gain on A when B follows)
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA, activeB);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			// Parallel path C
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);
			// Sum both paths and compensate — M/S bus-aware
			// Series path bus = sumBusB (last in chain), parallel path bus = sumBusC
			const int seriesBus = sumBusB;
			const int parallelBus = sumBusC;
			if ((seriesBus != 0 || parallelBus != 0) && buffer.getNumChannels() >= 2)
			{
				auto* bL = buffer.getWritePointer (0);
				auto* bR = buffer.getWritePointer (1);
				const auto* cL = tempBufferC.getReadPointer (0);
				const auto* cR = tempBufferC.getReadPointer (1);
				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], seriesBus, stL, stR, midBus, sideBus);
					injectMSBus (cL[i], cR[i], parallelBus, stL, stR, midBus, sideBus);
					bL[i] = stL + midBus + sideBus;
					bR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
			}
			buffer.applyGain (kSqrt2Over2); // -3dB for 2 parallel paths
		}
		else
		{
			// Only one path active — series or C alone
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 3) // A|B→C — A parallel, series B→C
	{
		const bool seriesActive = activeB || activeC;
		if (activeA && seriesActive)
		{
			// Copy input for parallel path A
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Parallel path A
			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			// Series path: B→C stays in buffer (skip auto-gain on B when C follows)
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB, activeC);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
			// Sum both paths and compensate — M/S bus-aware
			// Parallel path bus = sumBusA, series path bus = sumBusC (last in chain)
			const int parallelBus = sumBusA;
			const int seriesBus = sumBusC;
			if ((parallelBus != 0 || seriesBus != 0) && buffer.getNumChannels() >= 2)
			{
				auto* bL = buffer.getWritePointer (0);
				auto* bR = buffer.getWritePointer (1);
				const auto* aL = tempBufferA.getReadPointer (0);
				const auto* aR = tempBufferA.getReadPointer (1);
				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], seriesBus, stL, stR, midBus, sideBus);
					injectMSBus (aL[i], aR[i], parallelBus, stL, stR, midBus, sideBus);
					bL[i] = stL + midBus + sideBus;
					bR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
			}
			buffer.applyGain (kSqrt2Over2); // -3dB for 2 parallel paths
		}
		else
		{
			// Only one path active
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else // route == 0: A→B→C (full series)
	{
		// Skip auto-gain on non-final loaders so the hot saturated signal
		// feeds into the next stage, preserving harmonic stacking.
		const int lastActive = activeC ? 2 : (activeB ? 1 : 0);
		if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA, lastActive != 0);
		if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB, lastActive != 1);
		if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC, false);
	}

	// ── MATCH: Tilt EQ targeting a spectral profile ──
	// Target slopes: White=0, Pink=-3, Brown=-6, Bright=+3, Bright+=+6 dB/oct
	{
		const int matchProfile = loadRelaxedInt (pMatch);

		if (matchProfile != 0) // 0 = None
		{
			const float targetSlope = kTargetSlopes[juce::jlimit (0, kNumTargetSlopes - 1, matchProfile)];

			// No IR slope to compensate — apply target slope directly
			const float compensatingSlope = targetSlope;

			// Update target coefficients when profile or slope changes
			if (matchProfile != tiltLastProfile_ || std::abs (compensatingSlope - tiltLastSlope_) > 0.05f)
			{
				tiltLastProfile_ = matchProfile;
				tiltLastSlope_ = compensatingSlope;
				computeTiltShelfCoeffs (getSampleRate(), compensatingSlope,
				                       tiltTargetB0_, tiltTargetB1_, tiltTargetA1_);
			}

			// Smooth coefficients towards target (~30ms ramp) to avoid zipper noise
			const float smoothCoeff = cachedTiltSmoothCoeff_;

			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				float s = tiltState_[ch];
				float b0 = tiltB0_, b1 = tiltB1_, a1 = tiltA1_;
				const float tb0 = tiltTargetB0_, tb1 = tiltTargetB1_, ta1 = tiltTargetA1_;

				for (int i = 0; i < numSamples; ++i)
				{
					b0 += (tb0 - b0) * smoothCoeff;
					b1 += (tb1 - b1) * smoothCoeff;
					a1 += (ta1 - a1) * smoothCoeff;

					const float x = data[i];
					const float y = b0 * x + s;
					s = b1 * x - a1 * y;
					data[i] = y;
				}
				tiltState_[ch] = s;
			}
			// Store smoothed coefficients for next block
			const float blendFactor = 1.0f - std::pow (1.0f - smoothCoeff, static_cast<float> (numSamples));
			tiltB0_ += (tiltTargetB0_ - tiltB0_) * blendFactor;
			tiltB1_ += (tiltTargetB1_ - tiltB1_) * blendFactor;
			tiltA1_ += (tiltTargetA1_ - tiltA1_) * blendFactor;
		}
		else
		{
			// Reset state when match is off
			if (tiltLastProfile_ != 0)
			{
				tiltState_[0] = tiltState_[1] = 0.0f;
				tiltLastProfile_ = 0;
				tiltLastSlope_ = 0.0f;
				tiltB0_ = tiltTargetB0_ = 1.0f;
				tiltB1_ = tiltTargetB1_ = 0.0f;
				tiltA1_ = tiltTargetA1_ = 0.0f;
			}
		}
	}

	// ── NORM: static peak-normalize wet signal to target level ──
	// Captures maximum peak (no release) and applies fixed gain.
	// Toggling NORM off/on resets the peak measurement.
	// A warmup period (~150 ms) prevents boost during startup / 
	// session restore, where early peaks are unreliable and could cause a spike.
	{
		const int normIdx = loadRelaxedInt (pTrim);
		if (normIdx > 0)
		{
			const float target = kNormTargets[juce::jlimit (0, kNumNormTargets - 1, normIdx)];

			// Measure block peak across all channels
			float blockPeak = 0.0f;
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				blockPeak = std::max (blockPeak, buffer.getMagnitude (ch, 0, numSamples));

			// Peak-hold: only goes up, never down (static normalization)
			if (blockPeak > normPeakFollower_)
				normPeakFollower_ = blockPeak;

			// Calculate and apply gain once peak exceeds threshold (-60 dB)
			if (normPeakFollower_ > 0.001f)
			{
				// Accumulate warmup time (samples since first signal)
				normWarmupSamples_ += numSamples;
				const int warmupThreshold = static_cast<int> (getSampleRate() * 0.15f); // 150 ms
				const bool warmingUp = (normWarmupSamples_ < warmupThreshold);

				const float desiredGain = target / normPeakFollower_;
				const float clampedGain = juce::jlimit (0.01f, kMaxNormBoost, desiredGain);

				// During warmup: cap at unity (only allow cut, never boost).
				// After warmup: peak follower is reliable — allow full range.
				const float effectiveGain = warmingUp ? juce::jmin (1.0f, clampedGain)
				                                      : clampedGain;

				// Bi-directional smoothing: fast ramp-down (10 ms) when a louder
				// peak is captured, slower ramp-up (20 ms) for boost convergence.
				const float baseCoeff = (effectiveGain < normSmoothedGain_) ? cachedNormFastCoeff_ : cachedNormSlowCoeff_;
				const float coeff = 1.0f - std::pow (1.0f - baseCoeff, static_cast<float> (numSamples));
				normSmoothedGain_ += (effectiveGain - normSmoothedGain_) * coeff;

				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), normSmoothedGain_, numSamples);
			}
		}
		else
		{
			// Reset state when off — next enable will re-measure
			normPeakFollower_  = 0.0f;
			normSmoothedGain_  = 1.0f;
			normWarmupSamples_ = 0;
		}
	}

	// ── User limiter (WET: on processed signal, before global dry/wet mix) ──
	if (limMode == 1 && buffer.getNumChannels() >= 2)
		applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples, limThreshLin);

	// ── Invert Polarity / Stereo (WET mode: after Limiter WET, before mix) ──
	{
		const int nc = buffer.getNumChannels();
		if (invPol == 1)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 1 && nc >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Wet-only DC blocker.
	// Dry/default CLEAN paths must remain bit-transparent unless the user
	// explicitly engages processing that should shape tone. Keep this out of the
	// common post-mix path so `global mix = 0` and plain CLEAN stay unfiltered.
	{
		const bool wetAudible = (mixMode == 0) ? (globalMix > 0.001f) : (wetLevel > 0.001f);
		const bool wetHasSaturation =
			(activeA && mixA > 0.001f && satTypeA != SatEngine::Model::Clean) ||
			(activeB && mixB > 0.001f && satTypeB != SatEngine::Model::Clean) ||
			(activeC && mixC > 0.001f && satTypeC != SatEngine::Model::Clean);
		const bool wetAllRaw =
			(!activeA || mixA <= 0.001f || satTypeA == SatEngine::Model::Clean || satRawA) &&
			(!activeB || mixB <= 0.001f || satTypeB == SatEngine::Model::Clean || satRawB) &&
			(!activeC || mixC <= 0.001f || satTypeC == SatEngine::Model::Clean || satRawC);

		if (wetAudible && wetHasSaturation && !wetAllRaw)
		{
			const float R = cachedDcBlockR_;
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				float xPrev = dcBlockX_[ch];
				float yPrev = dcBlockY_[ch];
				for (int i = 0; i < numSamples; ++i)
				{
					const float x = data[i];
					const float y = x - xPrev + R * yPrev;
					xPrev = x;
					yPrev = y;
					data[i] = y;
				}
				dcBlockX_[ch] = xPrev;
				dcBlockY_[ch] = yPrev;
			}
		}
		else
		{
			dcBlockX_[0] = dcBlockX_[1] = 0.0f;
			dcBlockY_[0] = dcBlockY_[1] = 0.0f;
		}
	}

	// Global MIX: blend unprocessed dry with fully processed wet
	// dry = input after gain, wet = after all loader processing (mode + convolution + effects)
	if (needsDry)
	{
		float wet, dry;
		if (mixMode == 0)  // INSERT: classic crossfade
		{
			wet = globalMix;
			dry = 1.0f - globalMix;
		}
		else  // SEND: independent dry + wet levels
		{
			dry = dryLevel;
			wet = wetLevel;
		}
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* wetData = buffer.getWritePointer (ch);
			const auto* dryData = globalDryBuffer.getReadPointer (ch);
			juce::FloatVectorOperations::multiply (wetData, wet, numSamples);
			juce::FloatVectorOperations::addWithMultiply (wetData, dryData, dry, numSamples);
		}
	}

	// ── Flush denormals in global filter states (per-block, near-zero cost) ──
	{
		constexpr float kDnr = 1e-20f;
		if (std::abs (tiltState_[0]) < kDnr) tiltState_[0] = 0.0f;
		if (std::abs (tiltState_[1]) < kDnr) tiltState_[1] = 0.0f;
		if (std::abs (dcBlockX_[0])  < kDnr) dcBlockX_[0]  = 0.0f;
		if (std::abs (dcBlockX_[1])  < kDnr) dcBlockX_[1]  = 0.0f;
		if (std::abs (dcBlockY_[0])  < kDnr) dcBlockY_[0]  = 0.0f;
		if (std::abs (dcBlockY_[1])  < kDnr) dcBlockY_[1]  = 0.0f;
	}
	stateA.satState.flushDenormals();
	stateB.satState.flushDenormals();
	stateC.satState.flushDenormals();

	// Apply output gain
	const float outputGain = fastDecibelsToGain (loadRelaxed (pOutput));
	buffer.applyGain (outputGain);

	// Post-prepare fade-in: suppress startup filter/modulation transients
	if (fadeInSamplesRemaining_ > 0)
	{
		const int fadeThisBlock = juce::jmin (fadeInSamplesRemaining_, numSamples);
		const float invTotal = 1.0f / static_cast<float> (fadeInTotalSamples_);
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int i = 0; i < fadeThisBlock; ++i)
			{
				const int pos = fadeInTotalSamples_ - fadeInSamplesRemaining_ + i;
				data[i] *= static_cast<float> (pos) * invTotal;
			}
		}
		fadeInSamplesRemaining_ -= fadeThisBlock;
	}

	// ── User limiter (GLOBAL: after output gain, before safety clip) ──
	if (limMode == 2 && buffer.getNumChannels() >= 2)
		applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples, limThreshLin);

	// ── Invert Polarity / Stereo (GLOBAL mode: after Limiter GLOBAL, before safety clip) ──
	{
		const int nc = buffer.getNumChannels();
		if (invPol == 2)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 2 && nc >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Safety soft-limiter: prevent catastrophic output (NaN/Inf runaway).
	// Soft-knee tanh at +24 dBFS (15.85), hard clip at +30 dBFS (31.62).
	// Engages only during extreme settings (RAW + SERIES=6 + high drive).
	{
#if CABTR_DSP_DEBUG_LOG
		// Log output levels before safety limiter (throttled — same ~1s cadence)
		{
			static int diagOutCount = 0;
			++diagOutCount;
			const int bps = juce::jmax (1, (int)(currentSampleRate / juce::jmax (1, numSamples)));
			if (diagOutCount >= bps)
			{
				diagOutCount = 0;
				float outPeakL = 0.0f, outPeakR = 0.0f;
				if (buffer.getNumChannels() >= 1) outPeakL = buffer.getMagnitude (0, 0, numSamples);
				if (buffer.getNumChannels() >= 2) outPeakR = buffer.getMagnitude (1, 0, numSamples);
				juce::String d;
				d << "OUTPUT peakL=" << juce::String (outPeakL, 4) << " peakR=" << juce::String (outPeakR, 4)
				  << " outputGain=" << juce::String (outputGain, 6)
				  << " outputDb=" << juce::String (loadRelaxed (pOutput), 2)
				  << " fadeRemaining=" << fadeInSamplesRemaining_;
				LOG_IR_EVENT (d);
			}
		}
#endif
		constexpr float kSoftKnee = 15.849f;  // +24 dBFS — soft limiting starts here
		constexpr float kHardClip = 31.623f;   // +30 dBFS — absolute maximum
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int n = 0; n < numSamples; ++n)
			{
				float s = data[n];
				// NaN/Inf guard
				if (! std::isfinite (s)) { data[n] = 0.0f; continue; }
				const float a = std::abs (s);
				if (a > kSoftKnee)
				{
					// Soft-knee: tanh compression above threshold, then hard clip
					const float excess = (a - kSoftKnee) / kSoftKnee;
					const float compressed = kSoftKnee + kSoftKnee * std::tanh (excess);
					s = (s >= 0.0f ? compressed : -compressed);
					s = juce::jlimit (-kHardClip, kHardClip, s);
				}
				data[n] = s;
			}
		}
	}

	// Peak output + clip detection (post output gain)
	// (removed — profiling disabled for release)

#if SAT_DSP_DIAG
	// ── Final peak + diagnostics snapshot ──
	{
		float peakFinal = 0.0f;
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			const auto* data = buffer.getReadPointer (ch);
			for (int n = 0; n < numSamples; ++n)
			{
				const float a = std::abs (data[n]);
				if (a > peakFinal) peakFinal = a;
			}
		}

		const auto _diagEnd = std::chrono::steady_clock::now();
		const double blockUs = std::chrono::duration<double, std::micro> (_diagEnd - _diagStart).count();
		const double availUs = (numSamples / (double) getSampleRate()) * 1.0e6;

		SatDiag::BlockSnap snap;
		snap.blockTimeUs   = blockUs;
		snap.numSamples    = numSamples;
		snap.sampleRate    = (float) getSampleRate();
		snap.cpuPercent    = (availUs > 0.0) ? (blockUs / availUs * 100.0) : 0.0;
		snap.peakIn        = _diagCollector.peakIn;
		snap.peakOut       = _diagCollector.peakOut;
		snap.tapeCorePeak   = _diagCollector.tapeCorePeak;
		snap.lastPassInPeak = _diagCollector.lastPassInPeak;
		snap.triodeBlockPeak = _diagCollector.triodeBlockPeak;
		snap.tapeClipPeak   = _diagCollector.tapeClipPeak;
		snap.tapeDcPeak     = _diagCollector.tapeDcPeak;
		snap.tapeLimPeak    = _diagCollector.tapeLimPeak;
		// pkPreAG: compute from pkOut / autoGain (feedPreAG not called from audio thread)
		// This gives the peak output BEFORE auto-gain compensation was applied.
		snap.peakPreAG     = 0.0f;  // will be filled after autoGain is captured
		snap.peakFinal     = peakFinal;
		snap.maxDelta      = _diagCollector.maxDelta;
		snap.clickCount    = _diagCollector.clickCount;
		snap.nanCount      = _diagCollector.nanCount;
		snap.infCount      = _diagCollector.infCount;
		snap.denormalCount = _diagCollector.denormals;
		snap.adaaFallbacks = _diagCollector.adaaFB;
		snap.adaaBlends    = _diagCollector.adaaBlend;
		snap.adaaFull      = _diagCollector.adaaFull;
		snap.adaaLastDx    = _diagCollector.lastDx;
		snap.adaaLastK     = _diagCollector.lastK;
		snap.timestampMs   = juce::Time::currentTimeMillis();

		// Grab active loader index + state for model info + feedback states
		const int diagIdx = diagLoaderIndex;
		const IRLoaderState* activeState = diagIdx == 2 ? &stateC : (diagIdx == 1 ? &stateB : &stateA);

		auto diagPick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
			-> std::atomic<float>* { return diagIdx == 0 ? a : (diagIdx == 1 ? b : c); };

		snap.model        = static_cast<int> (SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (
			loadRelaxedInt (diagPick (pSatTypeA, pSatTypeB, pSatTypeC)))));
		snap.seriesCount  = juce::jlimit (1, 4, loadRelaxedInt (diagPick (pSeriesA, pSeriesB, pSeriesC)));
		snap.osOrder      = loadRelaxedInt (pOversample);
		snap.girth        = loadRelaxed (diagPick (pSatGirthA, pSatGirthB, pSatGirthC));
		snap.mod          = loadRelaxed (diagPick (pSatModA,   pSatModB,   pSatModC));
		snap.bias         = loadRelaxed (diagPick (pSatBiasA,  pSatBiasB,  pSatBiasC));
		snap.react        = loadRelaxed (diagPick (pSatSagA,   pSatSagB,   pSatSagC));
		snap.route        = route;
		snap.diagLoader   = diagIdx;
		snap.enableMask   = (enableA ? 1 : 0) | (enableB ? 2 : 0) | (enableC ? 4 : 0);
		snap.globalMix    = globalMix;
		snap.mixA         = mixA;
		snap.mixB         = mixB;
		snap.mixC         = mixC;
		snap.satDeltaPeak = diagSatDeltaPeak;

		const auto& ss = activeState->satState;
		snap.drive = ss.sDrive;
		snap.fuzzFeedback    = ss.fuzzFeedback[0][0];
		snap.doomFeedback    = ss.doomFeedback[0][0];
		snap.destroyFeedback = ss.destroyFeedback[0][0];
		snap.autoGainVal     = ss.blockCoeffs.autoGain;
		const int dbgPass = juce::jlimit (0, SatEngine::kMaxSeries - 1,
		                                  snap.seriesCount - 1);
		snap.transPrePeak    = ss.transistorDbgPre[dbgPass][0];
		snap.transCoreInPeak = ss.transistorDbgCoreIn[dbgPass][0];
		snap.transCoreOutPeak = ss.transistorDbgCoreOut[dbgPass][0];
		snap.transRailInPeak = ss.transistorDbgRailIn[dbgPass][0];
		snap.transRailOutPeak = ss.transistorDbgRailOut[dbgPass][0];
		snap.transPostPeak   = ss.transistorDbgPost[dbgPass][0];
		snap.transInputPad   = ss.transistorDbgInputPad[dbgPass][0];
		snap.transSatK       = ss.transistorDbgSatK[dbgPass][0];
		snap.transRailThresh = ss.transistorDbgRailThresh[dbgPass][0];
		// Now compute pkPreAG = pkOut / autoGain (the peak before gain compensation)
		if (snap.autoGainVal > 0.001f)
			snap.peakPreAG = snap.peakOut / snap.autoGainVal;
		float maxSagEnv = 0.0f;
		for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
			maxSagEnv = std::max (maxSagEnv, ss.sagEnvelope[sp][0]);
		snap.sagEnvelope     = maxSagEnv;
		snap.sagLastPass     = ss.sagEnvelope[dbgPass][0];
		snap.yinFreq         = ss.yinSmoothedFreq;

		// Max filter state magnitude (check for stuck/denormal filters)
		float mf = 0.0f;
		for (int f = 0; f < 6; ++f)
			mf = std::max (mf, std::abs (ss.doomDC[0][f][0]));
		mf = std::max (mf, std::abs (ss.fuzzCoupDC[0][0]));
		mf = std::max (mf, std::abs (ss.fuzzToneLPF[0][0]));
		mf = std::max (mf, std::abs (ss.destroyXfmrLP[0][0]));
		mf = std::max (mf, std::abs (ss.destroyRectHP[0][0]));
		snap.maxFilterState = mf;

		float mdc = 0.0f;
		for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
		{
			mdc = std::max (mdc, std::abs (ss.interStageDCx[sp][0]));
			mdc = std::max (mdc, std::abs (ss.interStageDCy[sp][0]));
		}
		for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
		{
			mdc = std::max (mdc, std::abs (ss.dcX[sp][0]));
			mdc = std::max (mdc, std::abs (ss.dcY[sp][0]));
		}
		snap.maxDcState = mdc;

		SatDiag::getDiagRing().push (snap);
	}
#endif
}

//==============================================================================
// Measure spectral slope of an IR in dB/octave via FFT + linear regression
// Analyses magnitude spectrum from 100 Hz to 10 kHz
void CABTRAudioProcessor::processLoader (IRLoaderState& state, 
                                          juce::AudioBuffer<float>& buffer,
                                          int loaderIndex,
                                          bool skipAutoGain)
{
#if SAT_DSP_DIAG
	SatDiag::Collector* diagCollectorPtr = &_diagCollector;
#else
	SatDiag::Collector* diagCollectorPtr = nullptr;
#endif
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	

	
	// Get runtime parameters (cached pointers — no hash lookup)
	// loaderIndex: 0=A, 1=B, 2=C
	auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
		-> std::atomic<float>* { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	const float hpFreq = loadRelaxed (pick (pHpFreqA, pHpFreqB, pHpFreqC));
	const float lpFreq = loadRelaxed (pick (pLpFreqA, pLpFreqB, pLpFreqC));
	const bool  hpOn   = loadRelaxedBool (pick (pHpOnA,   pHpOnB,   pHpOnC));
	const bool  lpOn   = loadRelaxedBool (pick (pLpOnA,   pLpOnB,   pLpOnC));
	const int   hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    loadRelaxedInt (pick (pHpSlopeA, pHpSlopeB, pHpSlopeC)));
	const int   lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    loadRelaxedInt (pick (pLpSlopeA, pLpSlopeB, pLpSlopeC)));
	const int   seriesCount = juce::jlimit (1, 4, loadRelaxedInt (pick (pSeriesA, pSeriesB, pSeriesC)));
	const float varAmt = loadRelaxed (pick (pVarA, pVarB, pVarC));
	const float pan = loadRelaxed (pick (pPanA, pPanB, pPanC));
	const float fred = loadRelaxed (pick (pFredA, pFredB, pFredC));
	const float pos = loadRelaxed (pick (pPosA, pPosB, pPosC));
	const float outDb = loadRelaxed (pick (pOutA, pOutB, pOutC));
	const float inDb  = loadRelaxed (pick (pInA,  pInB,  pInC));
	const float tiltDb = loadRelaxed (pick (pTiltA, pTiltB, pTiltC));
	const bool chaosEnabled = loadRelaxedBool (pick (pChaosA, pChaosB, pChaosC));
	const bool chaosFilterEnabled = loadRelaxedBool (pick (pChaosFilterA, pChaosFilterB, pChaosFilterC));
	const float chaosAmt = loadRelaxed (pick (pChaosAmtA, pChaosAmtB, pChaosAmtC));
	const float chaosSpd = loadRelaxed (pick (pChaosSpdA, pChaosSpdB, pChaosSpdC));
	const float chaosAmtFilter = loadRelaxed (pick (pChaosAmtFilterA, pChaosAmtFilterB, pChaosAmtFilterC));
	const float chaosSpdFilter = loadRelaxed (pick (pChaosSpdFilterA, pChaosSpdFilterB, pChaosSpdFilterC));

	// Filter / Tilt position
	const int fltPos = loadRelaxedInt (pick (pFilterPosA, pFilterPosB, pFilterPosC));
	// 0=F▼T▼  1=F▲T▲  2=F▲T▼  3=F▼T▲

	// Saturation parameters
	const int   satType  = loadRelaxedInt (pick (pSatTypeA,  pSatTypeB,  pSatTypeC));
	const float satDrive = loadRelaxed    (pick (pSatDriveA, pSatDriveB, pSatDriveC));
	const float satGirth = loadRelaxed    (pick (pSatGirthA, pSatGirthB, pSatGirthC));
	const float satMod   = loadRelaxed    (pick (pSatModA,   pSatModB,   pSatModC));
	const float satBias  = loadRelaxed    (pick (pSatBiasA,  pSatBiasB,  pSatBiasC));
	const float satSag   = loadRelaxed    (pick (pSatSagA,   pSatSagB,   pSatSagC));
	const bool  satRaw   = loadRelaxedBool (pick (pSatRawA,   pSatRawB,   pSatRawC));
	const int   osOrder  = loadRelaxedInt (pOversample);
	const float delayMs = loadRelaxed (pick (pDelayA, pDelayB, pDelayC));

	// Expander parameters
	const bool  expEnabled = loadRelaxedBool (pick (pExpA,       pExpB,       pExpC));
	const bool  expPost    = loadRelaxedBool (pick (pExpOrderA,  pExpOrderB,  pExpOrderC));
	const float expRatio   = loadRelaxed     (pick (pExpRatioA,  pExpRatioB,  pExpRatioC));
	const float expThreshDb = loadRelaxed    (pick (pExpThreshA, pExpThreshB, pExpThreshC));
	const float expAtkMs   = loadRelaxed     (pick (pExpAtkA,    pExpAtkB,    pExpAtkC));
	const float expRelMs   = loadRelaxed     (pick (pExpRelA,    pExpRelB,    pExpRelC));
	const auto model = SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (
		juce::jlimit (0, (int) SatEngine::Model::NumModels - 1, satType)));
	const bool filterPre = (fltPos == 1 || fltPos == 2);
	const bool tiltPre   = (fltPos == 1 || fltPos == 3);
	const bool chaosDriveEnabled = chaosEnabled;
	const bool chaosFilterEnabledForProc = chaosFilterEnabled;
	const bool expanderEnabled = expEnabled;
	
	// (FRED processing happens after convolution + filters)
	
	// 1. INPUT GAIN (IN)
	{
		const float inGain = fastDecibelsToGain (inDb);
		if (inGain < 0.999f)
			buffer.applyGain (inGain);
	}

	// 2. OUTPUT GAIN (OUT)
	// Intentionally NOT applied here.
	// Per-loader OUT must not affect the drive into the distortion black box;
	// only IN should change saturation input level. OUT is applied at the end
	// of the loader after the black-box and post-routing stages.

	// ── Tilt EQ lambda (PRE/POST saturation) ──
	auto applyTilt = [&]()
	{
	if (std::abs (tiltDb) > 0.05f)
	{
		if (std::abs (tiltDb - state.lastTiltDb) > 0.02f)
		{
			state.lastTiltDb = tiltDb;
			computeTiltShelfCoeffs (currentSampleRate, tiltDb,
			                       state.tiltTargetB0, state.tiltTargetB1, state.tiltTargetA1);
		}

		const float smoothCoeff = cachedTiltSmoothCoeff_;

		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			float s = state.tiltState[ch];
			float b0 = state.tiltB0, b1 = state.tiltB1, a1 = state.tiltA1;
			const float tb0 = state.tiltTargetB0, tb1 = state.tiltTargetB1, ta1 = state.tiltTargetA1;

			for (int i = 0; i < numSamples; ++i)
			{
				b0 += (tb0 - b0) * smoothCoeff;
				b1 += (tb1 - b1) * smoothCoeff;
				a1 += (ta1 - a1) * smoothCoeff;

				const float x = data[i];
				const float y = b0 * x + s;
				s = b1 * x - a1 * y;
				data[i] = y;
			}

			state.tiltState[ch] = s;
			state.tiltB0 = b0; state.tiltB1 = b1; state.tiltA1 = a1;
		}
	}
	else if (std::abs (state.lastTiltDb) > 0.05f)
	{
		state.lastTiltDb = 0.0f;
		state.tiltB0 = 1.0f; state.tiltB1 = 0.0f; state.tiltA1 = 0.0f;
		state.tiltTargetB0 = 1.0f; state.tiltTargetB1 = 0.0f; state.tiltTargetA1 = 0.0f;
		state.tiltState[0] = state.tiltState[1] = 0.0f;
	}
	}; // applyTilt

	// ── HP + LP Filters lambda (6/12/24 dB/oct) ──
	const bool chaosFilterActive = chaosFilterEnabledForProc && chaosAmtFilter > 0.01f;
	auto applyFilters = [&]()
	{

		// CHAOS FILTER Hermite+Drift: advance per-block, modulate HP/LP target frequencies (±2 oct)
		float hpTarget = hpFreq;
		float lpTarget = lpFreq;
		if (chaosFilterActive)
		{
			const float amountNorm = chaosAmtFilter * 0.01f;
			const float chaosFilterMaxOct = amountNorm * 2.0f;  // ±2 octaves at 100%
			const float shPeriodSamples = (float) currentSampleRate / chaosSpdFilter;
			const float sr = (float) currentSampleRate;

			// Advance Hermite+Drift F engine per-sample (mono S&H)
			for (int i = 0; i < numSamples; ++i)
			{
				advanceChaosEngine (state.chaosFPrev, state.chaosFCurr, state.chaosFNext,
				                    state.chaosFPhase, state.chaosFDriftPhase, state.chaosFDriftFreqHz,
				                    state.chaosFOut[0], state.chaosFRng, shPeriodSamples, amountNorm, sr);
			}

			const float octaveShift = state.chaosFOut[0] * chaosFilterMaxOct;
			const float freqMult = std::exp2 (octaveShift);
			// When HP/LP knobs are off, chaos sweeps the full 20–20k range (ECHO-TR match)
			const float hpBase = hpOn ? hpFreq : kFilterFreqMin;
			const float lpBase = lpOn ? lpFreq : kFilterFreqMax;
			hpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * freqMult);
			lpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * freqMult);
		}

		constexpr float kSmoothCoeff = 0.9955f; // ~5ms @ 44.1kHz
		const float oneMinusCoeff = 1.0f - kSmoothCoeff;

		// EMA smooth toward target frequencies (chaos-modulated or raw)
		state.smoothedHpFreq += (hpTarget - state.smoothedHpFreq) * oneMinusCoeff;
		state.smoothedLpFreq += (lpTarget - state.smoothedLpFreq) * oneMinusCoeff;

		// Recalculate coefficients every 32 samples (not every block)
		if (--state.filterCoeffCountdown <= 0)
		{
			state.filterCoeffCountdown = IRLoaderState::kFilterCoeffUpdateInterval;

			const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;

			// HP: recalc if smoothed frequency or slope changed
			const float clampedHp = juce::jlimit (20.0f, maxFreq, state.smoothedHpFreq);
			if (std::abs (clampedHp - state.lastHpFreq) > 0.01f || hpSlope != state.lastHpSlope)
			{
				if (hpSlope == 0) // 6 dB/oct — first-order
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (
						currentSampleRate, clampedHp);
				}
				else if (hpSlope == 1) // 12 dB/oct — Butterworth biquad
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kSqrt2Over2);
				}
				else // 24 dB/oct — cascaded biquad pair
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q1);
					*state.hpFilter2.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q2);
				}
				state.lastHpFreq = clampedHp;
				state.lastHpSlope = hpSlope;
			}

			// LP: recalc if smoothed frequency or slope changed
			const float clampedLp = juce::jlimit (20.0f, maxFreq, state.smoothedLpFreq);
			if (std::abs (clampedLp - state.lastLpFreq) > 0.01f || lpSlope != state.lastLpSlope)
			{
				if (lpSlope == 0) // 6 dB/oct — first-order
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass (
						currentSampleRate, clampedLp);
				}
				else if (lpSlope == 1) // 12 dB/oct — Butterworth biquad
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kSqrt2Over2);
				}
				else // 24 dB/oct — cascaded biquad pair
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q1);
					*state.lpFilter2.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q2);
				}
				state.lastLpFreq = clampedLp;
				state.lastLpSlope = lpSlope;
			}
		}

		// Apply HP filter (1 or 2 stages depending on slope)
		// Also apply when chaos filter is active, even if HP knob is off (full-range sweep)
		if ((hpOn || chaosFilterActive) && state.smoothedHpFreq >= 21.0f)
		{
			juce::dsp::AudioBlock<float> block (buffer);
			juce::dsp::ProcessContextReplacing<float> context (block);
			state.hpFilter.process (context);
			if (hpSlope == 2) // 24 dB/oct: second stage
				state.hpFilter2.process (context);
		}

		// Apply LP filter (1 or 2 stages depending on slope)
		// Also apply when chaos filter is active, even if LP knob is off (full-range sweep)
		if ((lpOn || chaosFilterActive) && state.smoothedLpFreq <= 19900.0f)
		{
			juce::dsp::AudioBlock<float> block (buffer);
			juce::dsp::ProcessContextReplacing<float> context (block);
			state.lpFilter.process (context);
			if (lpSlope == 2) // 24 dB/oct: second stage
				state.lpFilter2.process (context);
		}
	}; // applyFilters

	// ── PRE-saturation: apply tilt/filter if requested ──
	if (filterPre) applyFilters();
	if (tiltPre)   applyTilt();

	// ── Expander / Noise Gate lambda ──
	auto applyExpander = [&]()
	{
		if (!expanderEnabled || expRatio <= 1.01f)
			return;

		const float threshLin = fastDecibelsToGain (expThreshDb);
		const float sr = (float) currentSampleRate;
		const float attCoeff = std::exp (-1.0f / (sr * juce::jmax (0.00001f, expAtkMs * 0.001f)));
		const float relCoeff = std::exp (-1.0f / (sr * juce::jmax (0.001f,   expRelMs * 0.001f)));
		const float ratio = juce::jlimit (1.0f, 10.0f, expRatio);
		const float slope = ratio - 1.0f;  // downward expansion slope in dB below threshold

		const int chCount = juce::jmin (numChannels, 2);
		float* channelData[2] = { nullptr, nullptr };
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);

		for (int i = 0; i < numSamples; ++i)
		{
			// Stereo-linked peak detection
			float peak = 0.0f;
			for (int ch = 0; ch < chCount; ++ch)
				peak = juce::jmax (peak, std::abs (channelData[ch][i]));

			// Envelope follower (per-channel linked)
			float& env = state.expEnv[0];
			if (peak > env)
				env = attCoeff * env + (1.0f - attCoeff) * peak;
			else
				env = relCoeff * env + (1.0f - relCoeff) * peak;

			// Below-threshold expansion gain
			float gr = 1.0f;
			if (env < threshLin && env > 1.0e-12f)
			{
				// dB domain: gainReduction = slope * (threshDb - envDb)
				// Linear approximation via fast log/exp
				const float envDb = 20.0f * std::log10 (env);
				const float reductionDb = juce::jlimit (0.0f, 120.0f,
				                                        slope * (expThreshDb - envDb));
				gr = fastDecibelsToGain (-reductionDb);
			}

			for (int ch = 0; ch < chCount; ++ch)
				channelData[ch][i] *= gr;
		}
	};

	// ── PRE-saturation expander ──
	if (expanderEnabled && !expPost) applyExpander();

	// 2.5. CHAOS D (micro-delay + gain modulation — BEFORE saturation)
	if (chaosDriveEnabled && chaosAmt > 0.01f)
	{
		const float maxDelaySec = 0.005f; // ±5ms max
		const float amountNorm = chaosAmt * 0.01f; // 0..1
		const float maxDelaySamples = amountNorm * maxDelaySec * (float) currentSampleRate;
		const float shPeriodSamples = (float) currentSampleRate / chaosSpd;
		const float chaosGainMaxDb = amountNorm * 1.0f; // ±1dB at 100%
		const float sr = (float) currentSampleRate;
		
		const int chCount = juce::jmin (numChannels, 2);
		const int delayBufLen = IRLoaderState::kChaosDelayMaxSamples;
		const int mask = delayBufLen - 1;
		
		float* channelData[2] = { nullptr, nullptr };
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		for (int i = 0; i < numSamples; ++i)
		{
			// Advance Hermite+Drift D+G engines (per-channel)
			for (int c = 0; c < chCount; ++c)
			{
				advanceChaosEngine (state.chaosDPrev[c], state.chaosDCurr[c], state.chaosDNext[c],
				                    state.chaosDPhase[c], state.chaosDDriftPhase[c], state.chaosDDriftFreqHz[c],
				                    state.chaosDOut[c], state.chaosDRng[c], shPeriodSamples, amountNorm, sr);
				advanceChaosEngine (state.chaosGPrev[c], state.chaosGCurr[c], state.chaosGNext[c],
				                    state.chaosGPhase[c], state.chaosGDriftPhase[c], state.chaosGDriftFreqHz[c],
				                    state.chaosGOut[c], state.chaosGRng[c], shPeriodSamples, amountNorm, sr);
			}
			// Delay modulation is MONO (same for both channels) to avoid phaser artifacts
			// Gain modulation stays stereo for width
			state.chaosDOut[1] = state.chaosDOut[0];
			if (chCount < 2)
			{
				state.chaosGOut[1] = state.chaosGOut[0];
			}
			
			// Write current sample into delay buffer
			const int wp = state.chaosDelayWritePos;
			for (int ch = 0; ch < chCount; ++ch)
				state.chaosDelayBuffer[ch][wp] = channelData[ch][i];
			
			// Read with per-channel delay using Lagrange interpolation
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float delaySamp = juce::jlimit (0.0f, (float) (delayBufLen - 2),
				                                      maxDelaySamples + state.chaosDOut[ch] * maxDelaySamples);
				const float readPos = (float) wp - delaySamp;
				const int iPos = (int) std::floor (readPos);
				const float frac = readPos - (float) iPos;
				
				const float p0 = state.chaosDelayBuffer[ch][(iPos - 1) & mask];
				const float p1 = state.chaosDelayBuffer[ch][(iPos    ) & mask];
				const float p2 = state.chaosDelayBuffer[ch][(iPos + 1) & mask];
				const float p3 = state.chaosDelayBuffer[ch][(iPos + 2) & mask];
				
				const float c0 = p1;
				const float c1 = p2 - (1.0f / 3.0f) * p0 - 0.5f * p1 - (1.0f / 6.0f) * p3;
				const float c2 = 0.5f * (p0 + p2) - p1;
				const float c3 = (1.0f / 6.0f) * (p3 - p0) + 0.5f * (p1 - p2);
				channelData[ch][i] = ((c3 * frac + c2) * frac + c1) * frac + c0;
				
				// Per-channel gain modulation (±1dB, fast dB→linear)
				const float gainDb  = state.chaosGOut[ch] * chaosGainMaxDb;
				const float ex = gainDb * 0.16609640474f;
				const float exln2 = ex * 0.6931472f;
				const float gainLin = 1.0f + exln2 * (1.0f + exln2 * 0.5f);
				channelData[ch][i] *= gainLin;
			}
			
			state.chaosDelayWritePos = (wp + 1) & mask;
		}
	}

	// 3. SATURATION (with optional oversampling + series chaining)
	if (model != SatEngine::Model::Clean)
	{
#if SAT_DSP_DIAG
		// Capture pre-saturation peak (L channel)
		{
			const float* pL = buffer.getReadPointer (0);
			for (int n = 0; n < numSamples; ++n)
				_diagCollector.feedIn (pL[n]);
		}
#endif

		if (osOrder > 0 && osOrder <= 4)
		{
			auto& os = *oversamplers_[loaderIndex][osOrder - 1];
			auto block = juce::dsp::AudioBlock<float> (buffer);
			auto osBlock = os.processSamplesUp (block);

			float* osL = osBlock.getChannelPointer (0);
			float* osR = osBlock.getChannelPointer (1);
			const int osNumSamples = (int) osBlock.getNumSamples();
			const float osSr = (float) currentSampleRate * (float) os.getOversamplingFactor();

			SatEngine::processBlock (state.satState, osL, osR, osNumSamples,
			                         model, satDrive, satGirth, satMod, satBias, satSag, varAmt, osSr, seriesCount, false, skipAutoGain, satRaw, diagCollectorPtr);

			os.processSamplesDown (block);
		}
		else
		{
			float* dataL = buffer.getWritePointer (0);
			float* dataR = numChannels > 1 ? buffer.getWritePointer (1) : dataL;

			SatEngine::processBlock (state.satState, dataL, dataR, numSamples,
			                         model, satDrive, satGirth, satMod, satBias, satSag, varAmt,
			                         (float) currentSampleRate, seriesCount, true, skipAutoGain, satRaw, diagCollectorPtr);
		}

#if SAT_DSP_DIAG
		// Capture post-saturation peak + click detection (L channel)
		{
			const float* pL = buffer.getReadPointer (0);
			for (int n = 0; n < numSamples; ++n)
				_diagCollector.feedOut (pL[n]);
		}
#endif
	}

	// ── POST-saturation: apply tilt/filter if not already applied ──
	if (!tiltPre)   applyTilt();
	if (!filterPre) applyFilters();

	// ── POST-saturation expander ──
	if (expanderEnabled && expPost) applyExpander();

	// 4. DISTANCE (exponential LPF + gain attenuation)
	// 0% = close/bright (no change), 100% = far/dark (HF reduction + volume drop)
	if (pos > 0.01f)
	{
		// Exponential cutoff: 12kHz * exp(-pos * 2.08) → pos=0→12kHz, pos=1→1.5kHz
		const float cutoff = 12000.0f * std::exp (-pos * kDistDecay);
		constexpr float kPosSmooth = 0.9955f;
		state.smoothedPosFreq += (cutoff - state.smoothedPosFreq) * (1.0f - kPosSmooth);

		// Recalculate if frequency changed significantly
		const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;
		const float clampedPos = juce::jlimit (200.0f, maxFreq, state.smoothedPosFreq);
		if (std::abs (clampedPos - state.lastPosFreq) > 1.0f)
		{
			*state.posFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
				currentSampleRate, clampedPos, kSqrt2Over2);
			state.lastPosFreq = clampedPos;
		}
		
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.posFilter.process (context);

		// Distance gain attenuation: 0dB at pos=0, -6dB at pos=1
		const float distGain = 1.0f - pos * 0.5f;
		buffer.applyGain (distGain);
	}
	else if (state.lastPosFreq > 0.0f)
	{
		state.lastPosFreq = -1.0f; // Mark as inactive
	}
	
	// 5. PAN (cached gains)
	if (numChannels >= 2 && std::abs (pan - 0.5f) > 0.001f)
	{
		if (std::abs (pan - state.lastPan) > 0.001f)
		{
			const float panAngle = pan * 1.5707963f;
			state.lastPanLeft = std::cos (panAngle);
			state.lastPanRight = std::sin (panAngle);
			state.lastPan = pan;
		}
		
		buffer.applyGain (0, 0, numSamples, state.lastPanLeft);
		buffer.applyGain (1, 0, numSamples, state.lastPanRight);
	}
	
	// 5b. DELAY (auto-align compensation)
	if (delayMs > 0.1f)
		applyDelay (buffer, delayMs, loaderIndex);
	
	// 6. ANGLE (off-axis mic simulation)
	// Simulates a second mic at an angle on a guitar cab.
	// Fixed delay of ~159µs (≈5cm path difference), sample-rate independent.
	// First comb null at ~6.3kHz regardless of sample rate.
	// angle=0: pure on-axis (no effect), angle=1: full off-axis blend
	if (fred > 0.001f)
	{
		float* channelData[2] = { nullptr, nullptr };
		const int chCount = juce::jmin (numChannels, 2);
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		// Fractional delay in samples: 159µs × sampleRate
		const float delaySamples = IRLoaderState::kFredDelayMicros * 1e-6f * (float) currentSampleRate;
		const int delayInt = (int) delaySamples;
		const float delayFrac = delaySamples - (float) delayInt;
		const int bufSize = IRLoaderState::kFredDelayBufSize;
		const float compensate = 1.0f / (1.0f + fred);
		
		for (int i = 0; i < numSamples; ++i)
		{
			const int wIdx = state.fredDelayIndex;
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float direct = channelData[ch][i];
				state.fredDelayBuffer[ch][wIdx] = direct;
				
				// Linear interpolation between two delayed samples
				const int rIdx0 = (wIdx - delayInt + bufSize) % bufSize;
				const int rIdx1 = (wIdx - delayInt - 1 + bufSize) % bufSize;
				const float offAxis = state.fredDelayBuffer[ch][rIdx0] * (1.0f - delayFrac)
				                    + state.fredDelayBuffer[ch][rIdx1] * delayFrac;
				
				channelData[ch][i] = (direct + fred * offAxis) * compensate;
			}
			state.fredDelayIndex = (state.fredDelayIndex + 1) % bufSize;
		}
	}

	// 7. OUTPUT GAIN (OUT) — final per-loader output trim
	if (std::abs (outDb) > 0.01f)
	{
		const float outGain = fastDecibelsToGain (outDb);
		buffer.applyGain (outGain);
	}
	
	// ── Flush denormals in filter/tilt states (per-block, near-zero cost) ──
	{
		constexpr float kDnr = 1e-20f;
		if (std::abs (state.tiltState[0])       < kDnr) state.tiltState[0]       = 0.0f;
		if (std::abs (state.tiltState[1])       < kDnr) state.tiltState[1]       = 0.0f;
	}
}

//==============================================================================
void CABTRAudioProcessor::applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, int loaderIndex)
{
	if (delayMs < 0.1f)
		return;
	
	const int numSamples  = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	
	const float targetDelaySamples = delayMs * 0.001f * static_cast<float> (currentSampleRate);
	
	if (targetDelaySamples <= 0.0f)
		return;
	
	auto& delayLine = loaderIndex == 0 ? stateA.delayLine : (loaderIndex == 1 ? stateB.delayLine : stateC.delayLine);
	auto& smoother  = loaderIndex == 0 ? stateA.smoothedDelay : (loaderIndex == 1 ? stateB.smoothedDelay : stateC.smoothedDelay);
	
	smoother.setTargetValue (targetDelaySamples);
	
	for (int i = 0; i < numSamples; ++i)
	{
		const float currentDelay = smoother.getNextValue();
		delayLine.setDelay (currentDelay);
		
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* channelData = buffer.getWritePointer (ch);
			const float input = channelData[i];
			delayLine.pushSample (ch, input);
			channelData[i] = delayLine.popSample (ch);
		}
	}
}

//==============================================================================
void CABTRAudioProcessor::calculateAutoAlignment()
{
	const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
	const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
	const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;

	if (! enabledA || (! enabledB && ! enabledC))
		return;

	// ── Generate synthetic impulse responses ──
	// Process a unit impulse through each active engine's emphasis chain.
	const int irLen = 512;
	const float sr = static_cast<float> (currentSampleRate);

	auto makeIR = [&] (int satType, float satMod) -> juce::AudioBuffer<float>
	{
		juce::AudioBuffer<float> ir (1, irLen);
		ir.clear();
		ir.setSample (0, 0, 1.0f); // Unit impulse

		SatEngine::EmphasisState empSt;
		empSt.reset();

		const auto model = SatEngine::canonicalizeModel (static_cast<SatEngine::Model> (satType));

		SatEngine::EmphCoeffs ec;
		switch (model)
		{
			case SatEngine::Model::Triode:
				ec.preHP  = SatEngine::detail::onePoleCoeff (20.0f,   sr);
				ec.preSh  = SatEngine::detail::onePoleCoeff (3800.0f, sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (9500.0f, sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (30.0f,   sr);
				break;
			case SatEngine::Model::Cascade:
				ec.preHP  = SatEngine::detail::onePoleCoeff (30.0f,   sr);
				ec.preSh  = SatEngine::detail::onePoleCoeff (2600.0f, sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (9000.0f, sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (35.0f,   sr);
				break;
			case SatEngine::Model::PushPull:
				ec.postLP = SatEngine::detail::onePoleCoeff (5000.0f, sr);
				break;
			case SatEngine::Model::Diode:
				ec.preHP  = SatEngine::detail::onePoleCoeff (720.0f,  sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (723.0f,  sr);
				break;
			case SatEngine::Model::Clipper:
				ec.preHP  = SatEngine::detail::onePoleCoeff (720.0f,  sr);
				ec.preSh  = SatEngine::detail::onePoleCoeff (1800.0f, sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (2200.0f, sr);
				break;
			case SatEngine::Model::Tape:
				ec.preHP  = SatEngine::detail::onePoleCoeff (24.0f,    sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (16500.0f, sr);
				break;
			case SatEngine::Model::Fuzz:
				ec.preSh  = SatEngine::detail::onePoleCoeff (2000.0f, sr);
				ec.preHP  = SatEngine::detail::onePoleCoeff (14.0f,   sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (31.0f,   sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (8000.0f, sr);
				break;
			case SatEngine::Model::Doom:
				ec.preSh  = SatEngine::detail::onePoleCoeff (1800.0f, sr);
				ec.preHP  = SatEngine::detail::onePoleCoeff (20.0f,   sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (40.0f,   sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (5000.0f, sr);
				break;
			case SatEngine::Model::Destroy:
				ec.preHP  = SatEngine::detail::onePoleCoeff (20.0f,    sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (20.0f,    sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (14000.0f, sr);
				break;
			case SatEngine::Model::Tundra:
				ec.preHP  = SatEngine::detail::onePoleCoeff (70.0f,   sr);
				ec.preSh  = SatEngine::detail::onePoleCoeff (2300.0f, sr);
				ec.postHP = SatEngine::detail::onePoleCoeff (35.0f,   sr);
				ec.postLP = SatEngine::detail::onePoleCoeff (5800.0f, sr);
				break;
			default: break;
		}

		float* data = ir.getWritePointer (0);
		const float irDrive = 1.0f;
		for (int n = 0; n < irLen; ++n)
		{
			float x = data[n];
			x = SatEngine::preEmphasize (x, empSt, model, irDrive, satMod, ec);
			x = SatEngine::deEmphasize  (x, empSt, model, irDrive, satMod, ec);
			data[n] = x;
		}
		return ir;
	};

	// Energy centroid of impulse response — measures effective group delay
	// Unlike peak-finding cross-correlation, this gives meaningful non-zero
	// values even for minimum-phase IRs where the peak is always at sample 0.
	auto calcCentroid = [&] (const juce::AudioBuffer<float>& ir) -> float
	{
		const float* data = ir.getReadPointer (0);
		const int len = ir.getNumSamples();
		float energySum = 0.0f;
		float weightedSum = 0.0f;
		for (int n = 0; n < len; ++n)
		{
			const float e = data[n] * data[n];
			energySum += e;
			weightedSum += static_cast<float> (n) * e;
		}
		return energySum > 1e-20f ? weightedSum / energySum : 0.0f;
	};

	// Cross-correlation for polarity detection only (sign of correlation)
	auto xcorrSign = [&] (const float* dataA, const juce::AudioBuffer<float>& irX) -> float
	{
		const float* dataX = irX.getReadPointer (0);
		const int len = juce::jmin (irLen, irX.getNumSamples());
		float sum = 0.0f;
		for (int n = 0; n < len; ++n)
			sum += dataA[n] * dataX[n];
		return sum;
	};

	const int typeA = static_cast<int> (parameters.getRawParameterValue (kParamSatTypeA)->load());
	const float modA = parameters.getRawParameterValue (kParamSatModA)->load();
	auto irA = makeIR (typeA, modA);
	const float* dataA = irA.getReadPointer (0);
	const float centroidA = calcCentroid (irA);

	// Reset all delays and inversions first — ALIGN finds optimal from scratch
	if (auto* p = parameters.getParameter (kParamDelayA))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamDelayB))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamDelayC))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamInvA))
		p->setValueNotifyingHost (0.0f);
	if (auto* p = parameters.getParameter (kParamInvB))
		p->setValueNotifyingHost (0.0f);
	if (auto* p = parameters.getParameter (kParamInvC))
		p->setValueNotifyingHost (0.0f);

	float centroidB = centroidA;
	float centroidC = centroidA;
	float corrSignB = 1.0f;
	float corrSignC = 1.0f;

	if (enabledB)
	{
		const int typeB = static_cast<int> (parameters.getRawParameterValue (kParamSatTypeB)->load());
		const float modB = parameters.getRawParameterValue (kParamSatModB)->load();
		auto irB = makeIR (typeB, modB);
		centroidB = calcCentroid (irB);
		corrSignB = xcorrSign (dataA, irB);
	}

	if (enabledC)
	{
		const int typeC = static_cast<int> (parameters.getRawParameterValue (kParamSatTypeC)->load());
		const float modC = parameters.getRawParameterValue (kParamSatModC)->load();
		auto irC = makeIR (typeC, modC);
		centroidC = calcCentroid (irC);
		corrSignC = xcorrSign (dataA, irC);
	}

	// Find maximum centroid — loaders with smaller centroids need delay compensation
	const float maxCentroid = juce::jmax (centroidA,
	                                      enabledB ? centroidB : centroidA,
	                                      enabledC ? centroidC : centroidA);

	// Apply delays: convert centroid difference from samples to ms
	auto setCompDelay = [&] (const char* delayId, float centroid)
	{
		const float delaySamples = maxCentroid - centroid;
		const float delayMs = delaySamples / sr * 1000.0f;
		const float clampedMs = juce::jlimit (0.0f, kDelayMax, delayMs);
		if (auto* p = parameters.getParameter (delayId))
			p->setValueNotifyingHost (p->convertTo0to1 (clampedMs));
	};

	setCompDelay (kParamDelayA, centroidA);
	if (enabledB) setCompDelay (kParamDelayB, centroidB);
	if (enabledC) setCompDelay (kParamDelayC, centroidC);

	// Apply inversion: raw correlation sign directly determines polarity
	// (all INV states were reset above, so raw correlations reflect the true phase relationship)
	if (enabledB)
	{
		const bool needsInvert = (corrSignB < 0.0f);
		if (auto* p = parameters.getParameter (kParamInvB))
			p->setValueNotifyingHost (needsInvert ? 1.0f : 0.0f);
	}
	if (enabledC)
	{
		const bool needsInvert = (corrSignC < 0.0f);
		if (auto* p = parameters.getParameter (kParamInvC))
			p->setValueNotifyingHost (needsInvert ? 1.0f : 0.0f);
	}
}

//==============================================================================
bool CABTRAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* CABTRAudioProcessor::createEditor()
{
    return new CABTRAudioProcessorEditor (*this);
}

//==============================================================================
// TIMER CALLBACK: Check for parameter changes and trigger IR reload on message thread
// This ensures file I/O never happens in audio thread
// Rate-limited: max 1 reload per 300ms to prevent excessive reloads when dragging sliders
//==============================================================================
void CABTRAudioProcessor::timerCallback()
{
#if SAT_DSP_DIAG
	SatDiag::getDiagWriter().drain (SatDiag::getDiagRing());
#endif

	// ALIGN: momentary action — calculate cross-correlation + set delay/inv, then auto-reset
	{
		const float alignVal = parameters.getRawParameterValue (kParamAlign)->load();
		const juce::int64 now = juce::Time::currentTimeMillis();
		constexpr juce::int64 alignCooldownMs = 500;
		if (alignVal > 0.5f && (now - lastAlignTime_ >= alignCooldownMs))
		{
			const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
			const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
			const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;

			if (enabledA && (enabledB || enabledC))
			{
				calculateAutoAlignment();
				lastAlignTime_ = now;
			}

			// Auto-reset the momentary toggle
			if (auto* p = parameters.getParameter (kParamAlign))
				p->setValueNotifyingHost (0.0f);
		}
	}
}

//==============================================================================
// UI state persistence (non-automatable collapse state)
//==============================================================================
void CABTRAudioProcessor::setUiIoExpanded (int loaderIndex, bool expanded)
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
		parameters.state.setProperty (keys[loaderIndex], expanded, nullptr);
}

bool CABTRAudioProcessor::getUiIoExpanded (int loaderIndex) const noexcept
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
	{
		const auto fromState = parameters.state.getProperty (keys[loaderIndex]);
		if (! fromState.isVoid()) return (bool) fromState;
	}
	return false;
}

//==============================================================================
void CABTRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	auto state = parameters.copyState();
	auto xml = state.createXml();
	if (xml != nullptr)
		copyXmlToBinary (*xml, destData);
}

void CABTRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	auto xml = getXmlFromBinary (data, sizeInBytes);
	if (xml != nullptr)
	{
		auto state = juce::ValueTree::fromXml (*xml);
		if (state.isValid())
		{
			parameters.replaceState (state);

			auto remapLegacySatType = [this] (const char* paramId)
			{
				if (auto* raw = parameters.getRawParameterValue (paramId))
				{
					if (juce::roundToInt (raw->load()) == static_cast<int> (SatEngine::Model::PushPull))
					{
						if (auto* param = parameters.getParameter (paramId))
							param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (SatEngine::Model::Triode)));
					}
				}
			};

			remapLegacySatType (kParamSatTypeA);
			remapLegacySatType (kParamSatTypeB);
			remapLegacySatType (kParamSatTypeC);
		}
	}
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CABTRAudioProcessor();
}

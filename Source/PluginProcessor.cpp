#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"
#include "SatDspDiag.h"

// ----------------------------------------------------------------
// DSP utility functions (consistent with ECHO-TR)
// ----------------------------------------------------------------
namespace
{
	constexpr const char* kNamPathA = "namPathA";
	constexpr const char* kNamPathB = "namPathB";
	constexpr const char* kNamPathC = "namPathC";
	constexpr const char* kNamStateNode = "NAM_STATE";
	constexpr int kRtWorkBufferMaxSamples = 65536;

	inline const char* getNamPathPropertyKey (int loaderIndex) noexcept
	{
		constexpr const char* keys[] = { kNamPathA, kNamPathB, kNamPathC };
		return (loaderIndex >= 0 && loaderIndex < 3) ? keys[loaderIndex] : "";
	}

	juce::String readNamPathFromState (const juce::ValueTree& state, int loaderIndex)
	{
		const auto* key = getNamPathPropertyKey (loaderIndex);
		if (key == nullptr || key[0] == '\0')
			return {};

		auto path = state.getProperty (key, "").toString();
		if (path.isNotEmpty())
			return path;

		const auto namState = state.getChildWithName (kNamStateNode);
		if (namState.isValid())
			path = namState.getProperty (key, "").toString();

		return path;
	}

	// Fast dB-to-linear conversion using exp2 instead of pow(10, dB/20).
	// Mathematically equivalent: 10^(dB/20) = 2^(dB * log2(10)/20) = 2^(dB * 0.16609640474)
	inline float fastDecibelsToGain (float dB) noexcept
	{
		return (dB <= -100.0f) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline float gainFaderDecibelsToGain (float dB) noexcept
	{
		return (dB <= SATTRAudioProcessor::kGainFloorDb) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline juce::NormalisableRange<float> makeGainFaderRange() noexcept
	{
		return juce::NormalisableRange<float> (SATTRAudioProcessor::kGainFloorDb,
		                                       SATTRAudioProcessor::kGainMaxDb,
		                                       0.0f,
		                                       SATTRAudioProcessor::kGainSkew);
	}

	// Relaxed atomic load helpers - safe for audio thread (single-writer GUI, single-reader audio).
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

#if JUCE_MSVC
#pragma warning (push)
#pragma warning (disable: 4996) // MSVC warns on C++17 shared_ptr atomic free functions; use is intentionally localized.
#endif
	inline std::shared_ptr<SatNamModel> loadNamModelAtomic (const std::shared_ptr<SatNamModel>* slot) noexcept
	{
		return std::atomic_load_explicit (slot, std::memory_order_acquire);
	}

	inline void storeNamModelAtomic (std::shared_ptr<SatNamModel>* slot, std::shared_ptr<SatNamModel> model) noexcept
	{
		std::atomic_store_explicit (slot, std::move (model), std::memory_order_release);
	}
#if JUCE_MSVC
#pragma warning (pop)
#endif

	inline float measureBufferPeak (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
	{
		float peak = 0.0f;
		const int channels = buffer.getNumChannels();
		for (int ch = 0; ch < channels; ++ch)
			peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));
		return peak;
	}

	inline void updateMeterPeak (std::atomic<float>& meter, float blockPeak) noexcept
	{
		const float previous = meter.load (std::memory_order_relaxed);
		meter.store (juce::jmax (blockPeak, previous * 0.88f), std::memory_order_relaxed);
	}

	struct BiquadCoefficients
	{
		float b0 = 1.0f;
		float b1 = 0.0f;
		float b2 = 0.0f;
		float a1 = 0.0f;
		float a2 = 0.0f;
	};

	inline BiquadCoefficients makeDetectorFirstOrderHighPass (float frequency, float sampleRate) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float k = std::tan (juce::MathConstants<float>::pi * freq / sampleRate);
		const float norm = 1.0f / (1.0f + k);

		BiquadCoefficients c;
		c.b0 = norm;
		c.b1 = -norm;
		c.a1 = (k - 1.0f) * norm;
		return c;
	}

	inline BiquadCoefficients makeDetectorFirstOrderLowPass (float frequency, float sampleRate) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float k = std::tan (juce::MathConstants<float>::pi * freq / sampleRate);
		const float norm = 1.0f / (1.0f + k);

		BiquadCoefficients c;
		c.b0 = k * norm;
		c.b1 = c.b0;
		c.a1 = (k - 1.0f) * norm;
		return c;
	}

	inline BiquadCoefficients makeDetectorHighPass (float frequency, float sampleRate, float q) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float omega = juce::MathConstants<float>::twoPi * freq / sampleRate;
		const float sinOmega = std::sin (omega);
		const float cosOmega = std::cos (omega);
		const float alpha = sinOmega / (2.0f * q);
		const float invA0 = 1.0f / (1.0f + alpha);

		BiquadCoefficients c;
		c.b0 = ((1.0f + cosOmega) * 0.5f) * invA0;
		c.b1 = (-(1.0f + cosOmega)) * invA0;
		c.b2 = c.b0;
		c.a1 = (-2.0f * cosOmega) * invA0;
		c.a2 = (1.0f - alpha) * invA0;
		return c;
	}

	inline BiquadCoefficients makeDetectorLowPass (float frequency, float sampleRate, float q) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float omega = juce::MathConstants<float>::twoPi * freq / sampleRate;
		const float sinOmega = std::sin (omega);
		const float cosOmega = std::cos (omega);
		const float alpha = sinOmega / (2.0f * q);
		const float invA0 = 1.0f / (1.0f + alpha);

		BiquadCoefficients c;
		c.b0 = ((1.0f - cosOmega) * 0.5f) * invA0;
		c.b1 = (1.0f - cosOmega) * invA0;
		c.b2 = c.b0;
		c.a1 = (-2.0f * cosOmega) * invA0;
		c.a2 = (1.0f - alpha) * invA0;
		return c;
	}

	inline BiquadCoefficients makeDetectorHighPassForSlope (float frequency, float sampleRate, int slope, bool secondStage) noexcept
	{
		if (slope <= 0)
			return secondStage ? BiquadCoefficients {} : makeDetectorFirstOrderHighPass (frequency, sampleRate);

		const float q = (slope >= 2)
		              ? (secondStage ? SATTRAudioProcessor::kBW4_Q2 : SATTRAudioProcessor::kBW4_Q1)
		              : SATTRAudioProcessor::kSqrt2Over2;
		return makeDetectorHighPass (frequency, sampleRate, q);
	}

	inline BiquadCoefficients makeDetectorLowPassForSlope (float frequency, float sampleRate, int slope, bool secondStage) noexcept
	{
		if (slope <= 0)
			return secondStage ? BiquadCoefficients {} : makeDetectorFirstOrderLowPass (frequency, sampleRate);

		const float q = (slope >= 2)
		              ? (secondStage ? SATTRAudioProcessor::kBW4_Q2 : SATTRAudioProcessor::kBW4_Q1)
		              : SATTRAudioProcessor::kSqrt2Over2;
		return makeDetectorLowPass (frequency, sampleRate, q);
	}

	inline float processDetectorBiquad (float x,
	                                    SATTRAudioProcessor::LoaderState::ExpSidechainBiquadState& state,
	                                    const BiquadCoefficients& coeffs,
	                                    int channel) noexcept
	{
		auto& z1 = state.z1[channel];
		auto& z2 = state.z2[channel];
		const float y = coeffs.b0 * x + z1;
		z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
		z2 = coeffs.b2 * x - coeffs.a2 * y;
		return y;
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

// ----------------------------------------------------------------
SATTRAudioProcessor::SATTRAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                       .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#else
     :
#endif
      parameters (*this, nullptr, juce::Identifier ("SATTRState"), createParameterLayout())
{
	startTimer (200);
}

SATTRAudioProcessor::~SATTRAudioProcessor()
{
}

// ----------------------------------------------------------------
const juce::String SATTRAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SATTRAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SATTRAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SATTRAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double SATTRAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int SATTRAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int SATTRAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SATTRAudioProcessor::setCurrentProgram (int index)
{
	juce::ignoreUnused (index);
}

const juce::String SATTRAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void SATTRAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
	juce::ignoreUnused (index, newName);
}

// ----------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout SATTRAudioProcessor::createParameterLayout()
{
	juce::AudioProcessorValueTreeState::ParameterLayout layout;

	// ----------------------------------------------------------------
	//  Loader A Parameters
	// ----------------------------------------------------------------
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableA, "Enable A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqA, "HP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqA, "LP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
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
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutA, "Out A", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltA, "Tilt A",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDetailA, "Detail A",
		juce::NormalisableRange<float> (kDetailMin, kDetailMax, 0.001f),
		kDetailDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesA, "Series A",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInstabilityA, "Instability A",
		juce::NormalisableRange<float> (kInstabilityMin, kInstabilityMax, 0.001f),
		kInstabilityDefault));
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
		kParamOffsetA, "Offset A",
		juce::NormalisableRange<float> (kOffsetMin, kOffsetMax, 0.001f, 0.25f), kOffsetDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainA, "Sidechain A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainGainA, "Sidechain Gain A", makeGainFaderRange(), kSidechainGainDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainSmoothA, "Sidechain Smooth A",
		juce::NormalisableRange<float> (kSidechainSmoothMin, kSidechainSmoothMax, 0.001f, 1.0f),
		kSidechainSmoothDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpA, "Sidechain HP A",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpA, "Sidechain LP A",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainHpOnA, "Sidechain HP On A", kSidechainHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainLpOnA, "Sidechain LP On A", kSidechainLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpSlopeA, "Sidechain HP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpSlopeA, "Sidechain LP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainLpSlopeDefault));
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
		kParamModeInA, "Mode In A", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutA, "Mode Out A", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
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
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TRANSISTOR", "DIODE", "OVERDRIVE A", "CLIPPER", "NAM", "OVERDRIVE B" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveA, "Sat Drive A",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamNamSlimA, "NAM Size A",
		juce::NormalisableRange<float> (kNamSlimMin, kNamSlimMax, 0.01f), kNamSlimDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatCharA, "Sat Char A",
		juce::NormalisableRange<float> (kSatCharMin, kSatCharMax, 0.001f), kSatCharDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatTypeCtrlA, "Sat Type A",
		juce::NormalisableRange<float> (kSatTypeCtrlMin, kSatTypeCtrlMax, 0.001f), kSatTypeCtrlDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasA, "Sat Bias A",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagA, "Sat Dynamics A",
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
		kParamExpKneeA, "Exp Knee A",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkA, "Exp Atk A",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelA, "Exp Rel A",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpA, "Exp SC HP A",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpA, "Exp SC LP A",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnA, "Exp SC HP On A", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnA, "Exp SC LP On A", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeA, "Exp SC HP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeA, "Exp SC LP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainA, "Exp SC Gain A", makeGainFaderRange(), kExpScGainDefault));

	// ----------------------------------------------------------------
	//  Loader B Parameters
	// ----------------------------------------------------------------
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableB, "Enable B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqB, "HP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqB, "LP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
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
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutB, "Out B", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltB, "Tilt B",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDetailB, "Detail B",
		juce::NormalisableRange<float> (kDetailMin, kDetailMax, 0.001f),
		kDetailDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesB, "Series B",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInstabilityB, "Instability B",
		juce::NormalisableRange<float> (kInstabilityMin, kInstabilityMax, 0.001f),
		kInstabilityDefault));
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
		kParamOffsetB, "Offset B",
		juce::NormalisableRange<float> (kOffsetMin, kOffsetMax, 0.001f, 0.25f), kOffsetDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainB, "Sidechain B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainGainB, "Sidechain Gain B", makeGainFaderRange(), kSidechainGainDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainSmoothB, "Sidechain Smooth B",
		juce::NormalisableRange<float> (kSidechainSmoothMin, kSidechainSmoothMax, 0.001f, 1.0f),
		kSidechainSmoothDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpB, "Sidechain HP B",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpB, "Sidechain LP B",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainHpOnB, "Sidechain HP On B", kSidechainHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainLpOnB, "Sidechain LP On B", kSidechainLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpSlopeB, "Sidechain HP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpSlopeB, "Sidechain LP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainLpSlopeDefault));
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
		kParamModeInB, "Mode In B", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutB, "Mode Out B", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
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
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TRANSISTOR", "DIODE", "OVERDRIVE A", "CLIPPER", "NAM", "OVERDRIVE B" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveB, "Sat Drive B",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamNamSlimB, "NAM Size B",
		juce::NormalisableRange<float> (kNamSlimMin, kNamSlimMax, 0.01f), kNamSlimDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatCharB, "Sat Char B",
		juce::NormalisableRange<float> (kSatCharMin, kSatCharMax, 0.001f), kSatCharDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatTypeCtrlB, "Sat Type B",
		juce::NormalisableRange<float> (kSatTypeCtrlMin, kSatTypeCtrlMax, 0.001f), kSatTypeCtrlDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasB, "Sat Bias B",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagB, "Sat Dynamics B",
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
		kParamExpKneeB, "Exp Knee B",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkB, "Exp Atk B",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelB, "Exp Rel B",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpB, "Exp SC HP B",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpB, "Exp SC LP B",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnB, "Exp SC HP On B", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnB, "Exp SC LP On B", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeB, "Exp SC HP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeB, "Exp SC LP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainB, "Exp SC Gain B", makeGainFaderRange(), kExpScGainDefault));

	// ----------------------------------------------------------------
	//  Loader C Parameters
	// ----------------------------------------------------------------
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableC, "Enable C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqC, "HP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqC, "LP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
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
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutC, "Out C", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltC, "Tilt C",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDetailC, "Detail C",
		juce::NormalisableRange<float> (kDetailMin, kDetailMax, 0.001f),
		kDetailDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSeriesC, "Series C",
		juce::NormalisableRange<float> (kSeriesMin, kSeriesMax, 1.0f),
		kSeriesDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInstabilityC, "Instability C",
		juce::NormalisableRange<float> (kInstabilityMin, kInstabilityMax, 0.001f),
		kInstabilityDefault));
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
		kParamOffsetC, "Offset C",
		juce::NormalisableRange<float> (kOffsetMin, kOffsetMax, 0.001f, 0.25f), kOffsetDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainC, "Sidechain C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainGainC, "Sidechain Gain C", makeGainFaderRange(), kSidechainGainDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainSmoothC, "Sidechain Smooth C",
		juce::NormalisableRange<float> (kSidechainSmoothMin, kSidechainSmoothMax, 0.001f, 1.0f),
		kSidechainSmoothDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpC, "Sidechain HP C",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpC, "Sidechain LP C",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.01f, 0.35f),
		kSidechainLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainHpOnC, "Sidechain HP On C", kSidechainHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamSidechainLpOnC, "Sidechain LP On C", kSidechainLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHpSlopeC, "Sidechain HP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLpSlopeC, "Sidechain LP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kSidechainLpSlopeDefault));
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
		kParamModeInC, "Mode In C", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutC, "Mode Out C", juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeDefault));
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
            juce::StringArray { "CLEAN", "TAPE", "TUBE", "TRANSISTOR", "DIODE", "OVERDRIVE A", "CLIPPER", "NAM", "OVERDRIVE B" }, kSatTypeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatDriveC, "Sat Drive C",
		juce::NormalisableRange<float> (kSatDriveMin, kSatDriveMax, 0.001f), kSatDriveDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamNamSlimC, "NAM Size C",
		juce::NormalisableRange<float> (kNamSlimMin, kNamSlimMax, 0.01f), kNamSlimDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatCharC, "Sat Char C",
		juce::NormalisableRange<float> (kSatCharMin, kSatCharMax, 0.001f), kSatCharDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatTypeCtrlC, "Sat Type C",
		juce::NormalisableRange<float> (kSatTypeCtrlMin, kSatTypeCtrlMax, 0.001f), kSatTypeCtrlDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatBiasC, "Sat Bias C",
		juce::NormalisableRange<float> (kSatBiasMin, kSatBiasMax, 0.001f), kSatBiasDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSatSagC, "Sat Dynamics C",
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
		kParamExpKneeC, "Exp Knee C",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkC, "Exp Atk C",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelC, "Exp Rel C",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpC, "Exp SC HP C",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpC, "Exp SC LP C",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnC, "Exp SC HP On C", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnC, "Exp SC LP On C", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeC, "Exp SC HP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeC, "Exp SC LP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainC, "Exp SC Gain C", makeGainFaderRange(), kExpScGainDefault));

	// ----------------------------------------------------------------
	//  Global Parameters
	// ----------------------------------------------------------------
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input", makeGainFaderRange(), kInputDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output", makeGainFaderRange(), kOutputDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamRoute, "Route", juce::StringArray { "A>B>C", "A|B|C", "A>B|C", "A|B>C", "(A|B)>C", "A>(B|C)" }, kRouteDefault));
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

	// ----------------------------------------------------------------
	//  UI State Parameters (hidden from automation)
	// ----------------------------------------------------------------
	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiWidth, 1 }, "UI Width", 360, 1080, 360,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiHeight, 1 }, "UI Height", 300, 1500, 752,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiPalette, 1 }, "UI Palette", 0, 1, 0,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterBool> (
		juce::ParameterID { kParamUiFxTail, 1 }, "UI FX Tail", false,
		juce::AudioParameterBoolAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterBool> (
		juce::ParameterID { kParamUiIoFx, 1 }, "UI I/O FX", true,
		juce::AudioParameterBoolAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor0, 1 }, "UI Color 0", 0, 0xFFFFFF, 0x00FF00,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor1, 1 }, "UI Color 1", 0, 0xFFFFFF, 0x000000,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor2, 1 }, "UI Color 2", 0, 0xFFFFFF, 0x0000FF,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor3, 1 }, "UI Color 3", 0, 0xFFFFFF, 0xFF0000,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	return layout;
}

// ----------------------------------------------------------------
void SATTRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	currentSampleRate = sampleRate;
	currentBlockSize = samplesPerBlock;
	hasPreparedToPlay = true;
	
	LOG_DSP_EVENT ("prepareToPlay: sr=" + juce::String (sampleRate) + 
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

	// Prepare offset lines for auto-align
	stateA.delayLine.prepare (spec);
	stateB.delayLine.prepare (spec);
	stateC.delayLine.prepare (spec);
	stateA.dryDelayLine.prepare (spec);
	stateB.dryDelayLine.prepare (spec);
	stateC.dryDelayLine.prepare (spec);
	globalDryDelayLine.prepare (spec);
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	stateA.dryDelayLine.reset();
	stateB.dryDelayLine.reset();
	stateC.dryDelayLine.reset();
	globalDryDelayLine.reset();
	stateA.smoothedDelay.reset (sampleRate, 0.05);
	stateB.smoothedDelay.reset (sampleRate, 0.05);
	stateC.smoothedDelay.reset (sampleRate, 0.05);
	stateA.smoothedDryDelay.reset (sampleRate, 0.05);
	stateB.smoothedDryDelay.reset (sampleRate, 0.05);
	stateC.smoothedDryDelay.reset (sampleRate, 0.05);
	smoothedGlobalDryDelay.reset (sampleRate, 0.05);
	stateA.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateB.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateC.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateA.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
	stateB.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
	stateC.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
	smoothedGlobalDryDelay.setCurrentAndTargetValue (0.0f);

	for (auto* st : { &stateA, &stateB, &stateC })
	{
		auto nam = loadNamModelAtomic (&st->namModel);
		if (nam != nullptr)
			nam->reset (sampleRate, samplesPerBlock);
	}

	LoaderState* loaderStates[] = { &stateA, &stateB, &stateC };
	for (int loader = 0; loader < 3; ++loader)
	{
		if (loaderStates[loader]->pendingNamPath.isNotEmpty())
		{
			juce::String error;
			loadNamModelForLoader (loader, loaderStates[loader]->pendingNamPath, error);
		}
	}
	
	// Prepare temp buffers (pre-allocated for audio thread)
	// Use generous size to handle hosts that send variable/larger blocks
	// (e.g. FL Studio). Runtime guard in processBlock handles anything larger.
	const int bufAlloc = juce::jmax (samplesPerBlock, kRtWorkBufferMaxSamples);
	oversamplingBlockCapacity_ = bufAlloc;
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
	pDetailA = parameters.getRawParameterValue (kParamDetailA);
	pInstabilityA    = parameters.getRawParameterValue (kParamInstabilityA);
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
	pDetailB = parameters.getRawParameterValue (kParamDetailB);
	pInstabilityB    = parameters.getRawParameterValue (kParamInstabilityB);
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
	pDetailC   = parameters.getRawParameterValue (kParamDetailC);
	pInstabilityC      = parameters.getRawParameterValue (kParamInstabilityC);
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
	pInvA        = parameters.getRawParameterValue (kParamInvA);
	pInvB        = parameters.getRawParameterValue (kParamInvB);
	pInvC        = parameters.getRawParameterValue (kParamInvC);
	pInvPol      = parameters.getRawParameterValue (kParamInvPol);
	pInvStr      = parameters.getRawParameterValue (kParamInvStr);
	pMixMode     = parameters.getRawParameterValue (kParamMixMode);
	pDryLevel    = parameters.getRawParameterValue (kParamDryLevel);
	pWetLevel    = parameters.getRawParameterValue (kParamWetLevel);

	// Saturation parameter pointers
	pSatTypeA  = parameters.getRawParameterValue (kParamSatTypeA);
	pSatDriveA = parameters.getRawParameterValue (kParamSatDriveA);
	pNamSlimA  = parameters.getRawParameterValue (kParamNamSlimA);
	pSatCharA = parameters.getRawParameterValue (kParamSatCharA);
	pSatTypeCtrlA   = parameters.getRawParameterValue (kParamSatTypeCtrlA);
	pSatBiasA  = parameters.getRawParameterValue (kParamSatBiasA);
	pSatSagA   = parameters.getRawParameterValue (kParamSatSagA);
	pSatRawA   = parameters.getRawParameterValue (kParamSatRawA);
	pSatTypeB  = parameters.getRawParameterValue (kParamSatTypeB);
	pSatDriveB = parameters.getRawParameterValue (kParamSatDriveB);
	pNamSlimB  = parameters.getRawParameterValue (kParamNamSlimB);
	pSatCharB = parameters.getRawParameterValue (kParamSatCharB);
	pSatTypeCtrlB   = parameters.getRawParameterValue (kParamSatTypeCtrlB);
	pSatBiasB  = parameters.getRawParameterValue (kParamSatBiasB);
	pSatSagB   = parameters.getRawParameterValue (kParamSatSagB);
	pSatRawB   = parameters.getRawParameterValue (kParamSatRawB);
	pSatTypeC  = parameters.getRawParameterValue (kParamSatTypeC);
	pSatDriveC = parameters.getRawParameterValue (kParamSatDriveC);
	pNamSlimC  = parameters.getRawParameterValue (kParamNamSlimC);
	pSatCharC = parameters.getRawParameterValue (kParamSatCharC);
	pSatTypeCtrlC   = parameters.getRawParameterValue (kParamSatTypeCtrlC);
	pSatBiasC  = parameters.getRawParameterValue (kParamSatBiasC);
	pSatSagC   = parameters.getRawParameterValue (kParamSatSagC);
	pSatRawC   = parameters.getRawParameterValue (kParamSatRawC);
	pOversample = parameters.getRawParameterValue (kParamOversample);
	pOffsetA  = parameters.getRawParameterValue (kParamOffsetA);
	pOffsetB  = parameters.getRawParameterValue (kParamOffsetB);
	pOffsetC  = parameters.getRawParameterValue (kParamOffsetC);
	pSidechainA = parameters.getRawParameterValue (kParamSidechainA);
	pSidechainB = parameters.getRawParameterValue (kParamSidechainB);
	pSidechainC = parameters.getRawParameterValue (kParamSidechainC);
	pSidechainGainA = parameters.getRawParameterValue (kParamSidechainGainA);
	pSidechainGainB = parameters.getRawParameterValue (kParamSidechainGainB);
	pSidechainGainC = parameters.getRawParameterValue (kParamSidechainGainC);
	pSidechainSmoothA = parameters.getRawParameterValue (kParamSidechainSmoothA);
	pSidechainSmoothB = parameters.getRawParameterValue (kParamSidechainSmoothB);
	pSidechainSmoothC = parameters.getRawParameterValue (kParamSidechainSmoothC);
	pSidechainHpA = parameters.getRawParameterValue (kParamSidechainHpA);
	pSidechainHpB = parameters.getRawParameterValue (kParamSidechainHpB);
	pSidechainHpC = parameters.getRawParameterValue (kParamSidechainHpC);
	pSidechainLpA = parameters.getRawParameterValue (kParamSidechainLpA);
	pSidechainLpB = parameters.getRawParameterValue (kParamSidechainLpB);
	pSidechainLpC = parameters.getRawParameterValue (kParamSidechainLpC);
	pSidechainHpOnA = parameters.getRawParameterValue (kParamSidechainHpOnA);
	pSidechainHpOnB = parameters.getRawParameterValue (kParamSidechainHpOnB);
	pSidechainHpOnC = parameters.getRawParameterValue (kParamSidechainHpOnC);
	pSidechainLpOnA = parameters.getRawParameterValue (kParamSidechainLpOnA);
	pSidechainLpOnB = parameters.getRawParameterValue (kParamSidechainLpOnB);
	pSidechainLpOnC = parameters.getRawParameterValue (kParamSidechainLpOnC);
	pSidechainHpSlopeA = parameters.getRawParameterValue (kParamSidechainHpSlopeA);
	pSidechainHpSlopeB = parameters.getRawParameterValue (kParamSidechainHpSlopeB);
	pSidechainHpSlopeC = parameters.getRawParameterValue (kParamSidechainHpSlopeC);
	pSidechainLpSlopeA = parameters.getRawParameterValue (kParamSidechainLpSlopeA);
	pSidechainLpSlopeB = parameters.getRawParameterValue (kParamSidechainLpSlopeB);
	pSidechainLpSlopeC = parameters.getRawParameterValue (kParamSidechainLpSlopeC);
	pExpA       = parameters.getRawParameterValue (kParamExpA);
	pExpOrderA  = parameters.getRawParameterValue (kParamExpOrderA);
	pExpRatioA  = parameters.getRawParameterValue (kParamExpRatioA);
	pExpThreshA = parameters.getRawParameterValue (kParamExpThreshA);
	pExpKneeA   = parameters.getRawParameterValue (kParamExpKneeA);
	pExpAtkA    = parameters.getRawParameterValue (kParamExpAtkA);
	pExpRelA    = parameters.getRawParameterValue (kParamExpRelA);
	pExpScHpA   = parameters.getRawParameterValue (kParamExpScHpA);
	pExpScLpA   = parameters.getRawParameterValue (kParamExpScLpA);
	pExpScHpOnA = parameters.getRawParameterValue (kParamExpScHpOnA);
	pExpScLpOnA = parameters.getRawParameterValue (kParamExpScLpOnA);
	pExpScHpSlopeA = parameters.getRawParameterValue (kParamExpScHpSlopeA);
	pExpScLpSlopeA = parameters.getRawParameterValue (kParamExpScLpSlopeA);
	pExpScGainA = parameters.getRawParameterValue (kParamExpScGainA);
	pExpB       = parameters.getRawParameterValue (kParamExpB);
	pExpOrderB  = parameters.getRawParameterValue (kParamExpOrderB);
	pExpRatioB  = parameters.getRawParameterValue (kParamExpRatioB);
	pExpThreshB = parameters.getRawParameterValue (kParamExpThreshB);
	pExpKneeB   = parameters.getRawParameterValue (kParamExpKneeB);
	pExpAtkB    = parameters.getRawParameterValue (kParamExpAtkB);
	pExpRelB    = parameters.getRawParameterValue (kParamExpRelB);
	pExpScHpB   = parameters.getRawParameterValue (kParamExpScHpB);
	pExpScLpB   = parameters.getRawParameterValue (kParamExpScLpB);
	pExpScHpOnB = parameters.getRawParameterValue (kParamExpScHpOnB);
	pExpScLpOnB = parameters.getRawParameterValue (kParamExpScLpOnB);
	pExpScHpSlopeB = parameters.getRawParameterValue (kParamExpScHpSlopeB);
	pExpScLpSlopeB = parameters.getRawParameterValue (kParamExpScLpSlopeB);
	pExpScGainB = parameters.getRawParameterValue (kParamExpScGainB);
	pExpC       = parameters.getRawParameterValue (kParamExpC);
	pExpOrderC  = parameters.getRawParameterValue (kParamExpOrderC);
	pExpRatioC  = parameters.getRawParameterValue (kParamExpRatioC);
	pExpThreshC = parameters.getRawParameterValue (kParamExpThreshC);
	pExpKneeC   = parameters.getRawParameterValue (kParamExpKneeC);
	pExpAtkC    = parameters.getRawParameterValue (kParamExpAtkC);
	pExpRelC    = parameters.getRawParameterValue (kParamExpRelC);
	pExpScHpC   = parameters.getRawParameterValue (kParamExpScHpC);
	pExpScLpC   = parameters.getRawParameterValue (kParamExpScLpC);
	pExpScHpOnC = parameters.getRawParameterValue (kParamExpScHpOnC);
	pExpScLpOnC = parameters.getRawParameterValue (kParamExpScLpOnC);
	pExpScHpSlopeC = parameters.getRawParameterValue (kParamExpScHpSlopeC);
	pExpScLpSlopeC = parameters.getRawParameterValue (kParamExpScLpSlopeC);
	pExpScGainC = parameters.getRawParameterValue (kParamExpScGainC);

	lastInputGain_ = gainFaderDecibelsToGain (loadRelaxed (pInput, 0.0f));
	lastOutputGain_ = gainFaderDecibelsToGain (loadRelaxed (pOutput, 0.0f));
	if (loadRelaxedInt (pMixMode, kMixModeDefault) == 0)
	{
		const float globalMix = loadRelaxed (pMix, kGlobalMixDefault);
		lastGlobalWetMix_ = globalMix;
		lastGlobalDryMix_ = 1.0f - globalMix;
	}
	else
	{
		lastGlobalDryMix_ = loadRelaxed (pDryLevel, kDryLevelDefault);
		lastGlobalWetMix_ = loadRelaxed (pWetLevel, kWetLevelDefault);
	}
	lastLimiterThresholdLin_ = fastDecibelsToGain (loadRelaxed (pLimThreshold, kLimThresholdDefault));
	for (int loader = 0; loader < 3; ++loader)
	{
		sidechainEnv_[loader] = 0.0f;
		sidechainDriveAmount_[loader] = 0.0f;
		sidechainLastGain_[loader] = gainFaderDecibelsToGain (loadRelaxed ((loader == 0) ? pSidechainGainA : (loader == 1 ? pSidechainGainB : pSidechainGainC), kSidechainGainDefault));
		sidechainHpFilters_[loader].reset();
		sidechainHpFilters2_[loader].reset();
		sidechainLpFilters_[loader].reset();
		sidechainLpFilters2_[loader].reset();
		for (int ch = 0; ch < 2; ++ch)
		{
			sidechainDcPrevIn_[loader][ch] = 0.0f;
			sidechainDcPrevOut_[loader][ch] = 0.0f;
		}
	}

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
		cachedOutputDcBlockR_  = 1.0f - (juce::MathConstants<float>::twoPi * 2.0f / sr);
		// NORM AGC: per-block coefficients depend on numSamples, but the base tau is fixed.
		// We store the per-sample versions; processBlock scales by numSamples.
		cachedNormFastCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.01f)); // 10ms ramp-down
		cachedNormSlowCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.02f)); // 20ms ramp-up
	}
	dcBlockX_[0] = dcBlockX_[1] = 0.0f;
	dcBlockY_[0] = dcBlockY_[1] = 0.0f;

	// Fade-in to suppress startup filter/modulation transients (~5ms)
	fadeInTotalSamples_     = juce::jmax (64, (int) (sampleRate * 0.005));
	fadeInSamplesRemaining_ = fadeInTotalSamples_;

	auto initLoaderSmoothing = [] (LoaderState& state, float inDb, float outDb, float mixVal, float posVal) noexcept
	{
		state.lastInGain = gainFaderDecibelsToGain (inDb);
		state.lastOutGain = gainFaderDecibelsToGain (outDb);
		state.lastMix = mixVal;
		state.lastPosGain = 1.0f - juce::jlimit (0.0f, 1.0f, posVal) * 0.5f;
		state.lastSidechainPreGain = 1.0f;
	};
	initLoaderSmoothing (stateA, loadRelaxed (pInA,  kInDefault), loadRelaxed (pOutA, kOutDefault),
	                     loadRelaxed (pMixA, kGlobalMixDefault), loadRelaxed (pPosA, kPosDefault));
	initLoaderSmoothing (stateB, loadRelaxed (pInB,  kInDefault), loadRelaxed (pOutB, kOutDefault),
	                     loadRelaxed (pMixB, kGlobalMixDefault), loadRelaxed (pPosB, kPosDefault));
	initLoaderSmoothing (stateC, loadRelaxed (pInC,  kInDefault), loadRelaxed (pOutC, kOutDefault),
	                     loadRelaxed (pMixC, kGlobalMixDefault), loadRelaxed (pPosC, kPosDefault));

	// Reset FRED and CHAOS state for all loaders
	for (auto* state : { &stateA, &stateB, &stateC })
	{
		std::memset (state->fredDelayBuffer, 0, sizeof (state->fredDelayBuffer));
		state->fredDelayIndex = 0;
		std::memset (state->chaosDelayBuffer, 0, sizeof (state->chaosDelayBuffer));
		state->chaosDelayWritePos = 0;
		state->chaosDriveParamSmoothReady = false;
		state->chaosFilterParamSmoothReady = false;
		for (int c = 0; c < 2; ++c)
		{
			state->chaosDelaySmoothedSamples[c] = 0.0f;
			state->chaosDelaySmoothReady[c] = false;
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
	std::atomic<float>* chaosDriveAmtPtrs[] = { pChaosAmtA, pChaosAmtB, pChaosAmtC };
	std::atomic<float>* chaosDriveSpdPtrs[] = { pChaosSpdA, pChaosSpdB, pChaosSpdC };
	std::atomic<float>* chaosFilterAmtPtrs[] = { pChaosAmtFilterA, pChaosAmtFilterB, pChaosAmtFilterC };
	std::atomic<float>* chaosFilterSpdPtrs[] = { pChaosSpdFilterA, pChaosSpdFilterB, pChaosSpdFilterC };
	std::atomic<float>* expScGainPtrs[] = { pExpScGainA, pExpScGainB, pExpScGainC };
	LoaderState* states[] = { &stateA, &stateB, &stateC };
	for (int i = 0; i < 3; ++i)
	{
		states[i]->smoothedHpFreq = hpPtrs[i]->load();
		states[i]->smoothedLpFreq = lpPtrs[i]->load();
		states[i]->chaosDriveAmtSmoothed = chaosDriveAmtPtrs[i]->load();
		states[i]->chaosDriveSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosDriveSpdPtrs[i]->load());
		states[i]->chaosFilterAmtSmoothed = chaosFilterAmtPtrs[i]->load();
		states[i]->chaosFilterSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosFilterSpdPtrs[i]->load());
		states[i]->expScHpState.reset();
		states[i]->expScHpState2.reset();
		states[i]->expScLpState.reset();
		states[i]->expScLpState2.reset();
		states[i]->expScLastGain = gainFaderDecibelsToGain (loadRelaxed (expScGainPtrs[i], kExpScGainDefault));
		states[i]->satState.reset();
	}

	// Initialize oversampling objects (all factors - all loaders)
	for (int ldr = 0; ldr < 3; ++ldr)
	{
		for (int order = 1; order <= 4; ++order)
		{
			oversamplers_[ldr][order - 1] = std::make_unique<juce::dsp::Oversampling<float>> (
				2, (size_t) order,
				juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
			oversamplers_[ldr][order - 1]->initProcessing ((size_t) bufAlloc);

			oversamplersMono_[ldr][order - 1] = std::make_unique<juce::dsp::Oversampling<float>> (
				1, (size_t) order,
				juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
			oversamplersMono_[ldr][order - 1]->initProcessing ((size_t) bufAlloc);
		}
	}
	currentOsOrder_ = 0;
	currentReportedLatencySamples_ = -1;
}

void SATTRAudioProcessor::releaseResources()
{
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	stateA.dryDelayLine.reset();
	stateB.dryDelayLine.reset();
	stateC.dryDelayLine.reset();
	globalDryDelayLine.reset();
	dcBlockX_[0] = dcBlockX_[1] = 0.0f;
	dcBlockY_[0] = dcBlockY_[1] = 0.0f;
	currentReportedLatencySamples_ = -1;
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SATTRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
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

    const auto sidechainLayout = layouts.getChannelSet (true, 1);
    if (! sidechainLayout.isDisabled()
     && sidechainLayout != juce::AudioChannelSet::mono()
     && sidechainLayout != juce::AudioChannelSet::stereo())
        return false;
   #endif

    return true;
  #endif
}
#endif

// ----------------------------------------------------------------
// DSP NOTES:
//
// 1. MODE PROCESSING: Mid/Side conversion
//    - M/S encodes stereo as M=(L+R)*0.5 and S=(L-R)*0.5
//    - MID = (L+R) * 0.5  - keeps mono/mid sources at the same level
//    - SIDE = (L-R) * 0.5 - keeps side sources at the same level
//    - L+R = standard stereo pass-through
//
// 2. ROUTING:
//    - PARALLEL (A|B|C): Independent processing, summed output
//    - SERIES (A->B->C): Output of one loader feeds the next
//    - HYBRID: A->B|C and A|B->C
//
// 3. SIMD OPTIMIZATION: FloatVectorOperations for buffer operations
//    - applyGain, multiply, add use SIMD when available
//    - Significant speedup on modern CPUs
// ----------------------------------------------------------------

static inline void injectMSBus (float l, float r, int bus,
                                float& stL, float& stR,
                                float& midBus, float& sideBus)
{
	if (bus == 0)      { stL += l; stR += r; }
	else if (bus == 1) { midBus += (l + r) * 0.5f; }
	else               { sideBus += (l - r) * 0.5f; }
}

void SATTRAudioProcessor::applyMidSideInputMode (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
{
	if (modeVal >= 1 && modeVal <= 3 && buf.getNumChannels() >= 2)
	{
		auto* L = buf.getWritePointer (0);
		auto* R = buf.getWritePointer (1);
		for (int i = 0; i < nSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeVal == 1) // M/S encode, gain-transparent with M/S output
			{
				L[i] = (l + r) * 0.5f;
				R[i] = (l - r) * 0.5f;
			}
			else if (modeVal == 2) // MID = (L+R) * 0.5
			{
				const float mid = (l + r) * 0.5f;
				L[i] = R[i] = mid;
			}
			else // SIDE = (L-R) * 0.5
			{
				const float side = (l - r) * 0.5f;
				L[i] = R[i] = side;
			}
		}
	}
}

void SATTRAudioProcessor::updateReportedLatency()
{
	const int osOrder = loadRelaxedInt (pOversample);
	int reportedLatencySamples = 0;
	if (osOrder > 0 && osOrder <= 4 && oversamplers_[0][osOrder - 1] != nullptr)
		reportedLatencySamples += static_cast<int> (std::round (oversamplers_[0][osOrder - 1]->getLatencyInSamples()));

	if (reportedLatencySamples != currentReportedLatencySamples_)
	{
		currentReportedLatencySamples_ = reportedLatencySamples;
		currentOsOrder_ = osOrder;
		setLatencySamples (reportedLatencySamples);
	}
}

void SATTRAudioProcessor::processBlock (juce::AudioBuffer<float>& ioBuffer, juce::MidiBuffer& midiMessages)
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
		ioBuffer.clear (i, 0, ioBuffer.getNumSamples());

	const int numSamples = ioBuffer.getNumSamples();
	if (numSamples == 0)
		return;

	auto buffer = getBusBuffer (ioBuffer, false, 0);

	// Keep work buffers logically matched to the current block without reallocating
	// in normal use. The rare fallback preserves behaviour for oversized host blocks.
	auto ensureWorkBuffer = [numSamples] (juce::AudioBuffer<float>& work, int channels)
	{
		jassert (work.getNumChannels() >= channels);
		jassert (work.getNumSamples() >= numSamples);
		work.setSize (channels, numSamples, false, false, true);
	};
	{
		const int nc = buffer.getNumChannels();
		ensureWorkBuffer (tempBufferA, nc);
		ensureWorkBuffer (tempBufferB, nc);
		ensureWorkBuffer (tempBufferC, nc);
		ensureWorkBuffer (globalDryBuffer, nc);
		ensureWorkBuffer (loaderDryBuffer, nc);
	}

	// Get global parameters (cached pointers - relaxed atomic, no hash lookup)
	const bool enableA = loadRelaxedBool (pEnableA);
	const bool enableB = loadRelaxedBool (pEnableB);
	const bool enableC = loadRelaxedBool (pEnableC);

	const int route = loadRelaxedInt (pRoute);
	const float globalMix = loadRelaxed (pMix);
	const int   mixMode   = loadRelaxedInt (pMixMode);
	const float dryLevel  = (mixMode == 1) ? loadRelaxed (pDryLevel) : 0.0f;
	const float wetLevel  = (mixMode == 1) ? loadRelaxed (pWetLevel) : 0.0f;
	const float globalWetEnd = (mixMode == 0) ? globalMix : wetLevel;
	const float globalDryEnd = (mixMode == 0) ? (1.0f - globalMix) : dryLevel;
	constexpr float kMixBypassEps = 1.0e-4f;
	const float offsetA = loadRelaxed (pOffsetA);
	const float offsetB = loadRelaxed (pOffsetB);
	const float offsetC = loadRelaxed (pOffsetC);
	const bool needsGlobalDry = std::abs (globalDryEnd) > kMixBypassEps
	                         || std::abs (lastGlobalDryMix_) > kMixBypassEps;
	const bool needsGlobalWetGain = std::abs (globalWetEnd - 1.0f) > kMixBypassEps
	                             || std::abs (lastGlobalWetMix_ - globalWetEnd) > kMixBypassEps;

	// -- Limiter --
	const int limMode = loadRelaxedInt (pLimMode);
	const float limThreshLinStart = lastLimiterThresholdLin_;
	const float limThreshLin = (limMode != 0)
		? fastDecibelsToGain (loadRelaxed (pLimThreshold, kLimThresholdDefault))
		: 1.0f;
	lastLimiterThresholdLin_ = limThreshLin;

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
	const bool invA = loadRelaxedBool (pInvA);
	const bool invB = loadRelaxedBool (pInvB);
	const bool invC = loadRelaxedBool (pInvC);
	const auto satTypeA = static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeA));
	const auto satTypeB = static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeB));
	const auto satTypeC = static_cast<SatEngine::Model> (loadRelaxedInt (pSatTypeC));
	const bool namReadyA = satTypeA != SatEngine::Model::NAM || isNamModelLoadedForLoader (0);
	const bool namReadyB = satTypeB != SatEngine::Model::NAM || isNamModelLoadedForLoader (1);
	const bool namReadyC = satTypeC != SatEngine::Model::NAM || isNamModelLoadedForLoader (2);
	const bool activeA = enableA && namReadyA;
	const bool activeB = enableB && namReadyB;
	const bool activeC = enableC && namReadyC;
#if SAT_DSP_DIAG
	const int diagLoaderIndex = activeC ? 2 : (activeB ? 1 : 0);
	float diagSatDeltaPeak = 0.0f;
#endif
	const bool sidechainA = activeA && loadRelaxedBool (pSidechainA);
	const bool sidechainB = activeB && loadRelaxedBool (pSidechainB);
	const bool sidechainC = activeC && loadRelaxedBool (pSidechainC);
	const bool anySidechainEnabled = sidechainA || sidechainB || sidechainC;

	const bool dryAlignMode = dryAlignRuntimeActive_.load (std::memory_order_acquire);
	auto loaderDryAlignSamples = [&] (const LoaderState& state, bool active, float loaderMix, float offsetMs) noexcept
	{
		if (! dryAlignMode || ! active || loaderMix <= kMixBypassEps)
			return 0.0f;

		const float delaySamples = offsetMs * 0.001f * static_cast<float> (currentSampleRate);
		return juce::jmax (0.0f, state.dryAnchorSamples.load() + delaySamples);
	};

	const float dryAlignSamplesA = loaderDryAlignSamples (stateA, activeA, mixA, offsetA);
	const float dryAlignSamplesB = loaderDryAlignSamples (stateB, activeB, mixB, offsetB);
	const float dryAlignSamplesC = loaderDryAlignSamples (stateC, activeC, mixC, offsetC);

	auto averageDryAlignBranches = [] (float a, bool hasA, float b, bool hasB, float c, bool hasC) noexcept
	{
		float sum = 0.0f;
		int count = 0;
		if (hasA) { sum += a; ++count; }
		if (hasB) { sum += b; ++count; }
		if (hasC) { sum += c; ++count; }
		return count > 0 ? sum / static_cast<float> (count) : 0.0f;
	};

	const bool hasDryAlignA = dryAlignMode && activeA && mixA > kMixBypassEps;
	const bool hasDryAlignB = dryAlignMode && activeB && mixB > kMixBypassEps;
	const bool hasDryAlignC = dryAlignMode && activeC && mixC > kMixBypassEps;
	float globalDryAlignSamples = 0.0f;
	if (route == 0) // A->B->C
		globalDryAlignSamples = dryAlignSamplesA + dryAlignSamplesB + dryAlignSamplesC;
	else if (route == 1) // A|B|C
		globalDryAlignSamples = averageDryAlignBranches (dryAlignSamplesA, hasDryAlignA,
		                                                 dryAlignSamplesB, hasDryAlignB,
		                                                 dryAlignSamplesC, hasDryAlignC);
	else if (route == 2) // A->B | C
	{
		const float series = dryAlignSamplesA + dryAlignSamplesB;
		globalDryAlignSamples = averageDryAlignBranches (series, hasDryAlignA || hasDryAlignB,
		                                                 0.0f, false,
		                                                 dryAlignSamplesC, hasDryAlignC);
	}
	else if (route == 3) // A | B->C
	{
		const float series = dryAlignSamplesB + dryAlignSamplesC;
		globalDryAlignSamples = averageDryAlignBranches (dryAlignSamplesA, hasDryAlignA,
		                                                 series, hasDryAlignB || hasDryAlignC,
		                                                 0.0f, false);
	}
	else if (route == 4) // (A|B)->C
	{
		const float parallel = averageDryAlignBranches (dryAlignSamplesA, hasDryAlignA,
		                                                dryAlignSamplesB, hasDryAlignB,
		                                                0.0f, false);
		globalDryAlignSamples = parallel + dryAlignSamplesC;
	}
	else if (route == 5) // A->(B|C)
	{
		const float parallel = averageDryAlignBranches (dryAlignSamplesB, hasDryAlignB,
		                                                dryAlignSamplesC, hasDryAlignC,
		                                                0.0f, false);
		globalDryAlignSamples = dryAlignSamplesA + parallel;
	}

	const bool sidechainActive[3] = { sidechainA, sidechainB, sidechainC };
	std::atomic<float>* sidechainGainParams[3] = { pSidechainGainA, pSidechainGainB, pSidechainGainC };
	std::atomic<float>* sidechainSmoothParams[3] = { pSidechainSmoothA, pSidechainSmoothB, pSidechainSmoothC };
	std::atomic<float>* sidechainHpParams[3] = { pSidechainHpA, pSidechainHpB, pSidechainHpC };
	std::atomic<float>* sidechainLpParams[3] = { pSidechainLpA, pSidechainLpB, pSidechainLpC };
	std::atomic<float>* sidechainHpOnParams[3] = { pSidechainHpOnA, pSidechainHpOnB, pSidechainHpOnC };
	std::atomic<float>* sidechainLpOnParams[3] = { pSidechainLpOnA, pSidechainLpOnB, pSidechainLpOnC };
	std::atomic<float>* sidechainHpSlopeParams[3] = { pSidechainHpSlopeA, pSidechainHpSlopeB, pSidechainHpSlopeC };
	std::atomic<float>* sidechainLpSlopeParams[3] = { pSidechainLpSlopeA, pSidechainLpSlopeB, pSidechainLpSlopeC };
	auto scBuffer = (anySidechainEnabled && getBusCount (true) > 1)
		? getBusBuffer (ioBuffer, true, 1)
		: juce::AudioBuffer<float> {};
	const bool sidechainBusAvailable = scBuffer.getNumChannels() > 0;

	const float sr = juce::jmax (1.0f, (float) currentSampleRate);
	for (int loader = 0; loader < 3; ++loader)
	{
		if (! sidechainActive[loader])
		{
			sidechainEnv_[loader] = 0.0f;
			sidechainDriveAmount_[loader] = 0.0f;
			sidechainLastGain_[loader] = 1.0f;
			sidechainHpFilters_[loader].reset();
			sidechainHpFilters2_[loader].reset();
			sidechainLpFilters_[loader].reset();
			sidechainLpFilters2_[loader].reset();
			for (int ch = 0; ch < 2; ++ch)
			{
				sidechainDcPrevIn_[loader][ch] = 0.0f;
				sidechainDcPrevOut_[loader][ch] = 0.0f;
			}
			continue;
		}

		const float smooth = juce::jlimit (kSidechainSmoothMin, kSidechainSmoothMax,
		                                   loadRelaxed (sidechainSmoothParams[loader], kSidechainSmoothDefault));
		const float scGainDb = juce::jlimit (kSidechainGainMin, kSidechainGainMax,
		                                     loadRelaxed (sidechainGainParams[loader], kSidechainGainDefault));
		const float maxDetectorFreq = juce::jmin (kSidechainFilterFreqMax, sr * 0.45f);
		float hpHz = juce::jlimit (kSidechainFilterFreqMin, maxDetectorFreq,
		                           loadRelaxed (sidechainHpParams[loader], kSidechainHpDefault));
		const float lpHz = juce::jlimit (kSidechainFilterFreqMin, maxDetectorFreq,
		                                loadRelaxed (sidechainLpParams[loader], kSidechainLpDefault));
		if (hpHz >= lpHz)
			hpHz = juce::jmax (kSidechainFilterFreqMin, lpHz * 0.95f);
		const bool hpOn = loadRelaxedBool (sidechainHpOnParams[loader], kSidechainHpOnDefault);
		const bool lpOn = loadRelaxedBool (sidechainLpOnParams[loader], kSidechainLpOnDefault);
		const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
		                                  loadRelaxedInt (sidechainHpSlopeParams[loader], kSidechainHpSlopeDefault));
		const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
		                                  loadRelaxedInt (sidechainLpSlopeParams[loader], kSidechainLpSlopeDefault));

		if (! sidechainBusAvailable)
		{
			sidechainHpFilters_[loader].reset();
			sidechainHpFilters2_[loader].reset();
			sidechainLpFilters_[loader].reset();
			sidechainLpFilters2_[loader].reset();
			for (int ch = 0; ch < 2; ++ch)
			{
				sidechainDcPrevIn_[loader][ch] = 0.0f;
				sidechainDcPrevOut_[loader][ch] = 0.0f;
			}
		}

		const BiquadCoefficients hp = hpOn ? makeDetectorHighPassForSlope (hpHz, sr, hpSlope, false) : BiquadCoefficients {};
		const BiquadCoefficients hp2 = (hpOn && hpSlope >= 2) ? makeDetectorHighPassForSlope (hpHz, sr, hpSlope, true) : BiquadCoefficients {};
		const BiquadCoefficients lp = lpOn ? makeDetectorLowPassForSlope (lpHz, sr, lpSlope, false) : BiquadCoefficients {};
		const BiquadCoefficients lp2 = (lpOn && lpSlope >= 2) ? makeDetectorLowPassForSlope (lpHz, sr, lpSlope, true) : BiquadCoefficients {};
		if (! hpOn)
		{
			sidechainHpFilters_[loader].reset();
			sidechainHpFilters2_[loader].reset();
		}
		else if (hpSlope < 2)
		{
			sidechainHpFilters2_[loader].reset();
		}
		if (! lpOn)
		{
			sidechainLpFilters_[loader].reset();
			sidechainLpFilters2_[loader].reset();
		}
		else if (lpSlope < 2)
		{
			sidechainLpFilters2_[loader].reset();
		}

		double sumSquares = 0.0;
		float peak = 0.0f;
		if (sidechainBusAvailable)
		{
			const int scChannels = juce::jmin (2, scBuffer.getNumChannels());
			float scGain = sidechainLastGain_[loader];
			const float scGainTarget = gainFaderDecibelsToGain (scGainDb);
			const float scGainStep = (scGainTarget - scGain) / (float) juce::jmax (1, numSamples);
			for (int i = 0; i < numSamples; ++i)
			{
				for (int ch = 0; ch < scChannels; ++ch)
				{
					const float raw = scBuffer.getReadPointer (ch)[i];
					float detectorSample = raw - sidechainDcPrevIn_[loader][ch]
						+ cachedDcBlockR_ * sidechainDcPrevOut_[loader][ch];
					sidechainDcPrevIn_[loader][ch] = raw;
					sidechainDcPrevOut_[loader][ch] = detectorSample;
					detectorSample *= scGain;
					if (hpOn)
					{
						detectorSample = processDetectorBiquad (detectorSample, sidechainHpFilters_[loader], hp, ch);
						if (hpSlope >= 2)
							detectorSample = processDetectorBiquad (detectorSample, sidechainHpFilters2_[loader], hp2, ch);
					}
					if (lpOn)
					{
						detectorSample = processDetectorBiquad (detectorSample, sidechainLpFilters_[loader], lp, ch);
						if (lpSlope >= 2)
							detectorSample = processDetectorBiquad (detectorSample, sidechainLpFilters2_[loader], lp2, ch);
					}
					sumSquares += (double) detectorSample * (double) detectorSample;
					peak = juce::jmax (peak, std::abs (detectorSample));
				}
				scGain += scGainStep;
			}
			sidechainLastGain_[loader] = scGainTarget;
		}

		const float rms = sidechainBusAvailable
			? (float) std::sqrt (sumSquares / (double) juce::jmax (1, juce::jmin (2, scBuffer.getNumChannels()) * numSamples))
			: 0.0f;
		const float target = juce::jlimit (0.0f, 1.0f, juce::jmax (rms * 4.0f, peak * 0.75f));

		float smoothEffective = smooth;
		if (smooth <= 0.25f)
		{
			const float t = smooth / 0.25f;
			smoothEffective = 0.25f * (2.0f * t * t - t * t * t);
		}
		const bool directAtZeroSmooth = smoothEffective <= 0.000001f;

		if (directAtZeroSmooth)
		{
			sidechainEnv_[loader] = target;
		}
		else
		{
			const float legacySmooth = juce::jmin (1.0f, smoothEffective * 2.0f);
			const float extendedBlend = juce::jlimit (0.0f, 1.0f, (smoothEffective - 0.5f) * 2.0f);
			const float smoothShape = (smoothEffective <= 0.5f) ? legacySmooth : (1.0f + 0.5f * extendedBlend);
			const float tau = 0.001f * smoothShape + smoothShape * smoothShape * 0.040f;
			const float coeff = 1.0f - std::exp (-(float) numSamples / (sr * tau));
			sidechainEnv_[loader] += (target - sidechainEnv_[loader]) * juce::jlimit (0.0f, 1.0f, coeff);
		}
		sidechainDriveAmount_[loader] = juce::jlimit (0.0f, 1.0f, sidechainEnv_[loader]);
	}

	updateReportedLatency();

	// Apply input gain
	const float inputGain = gainFaderDecibelsToGain (loadRelaxed (pInput));
	for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		buffer.applyGainRamp (ch, 0, numSamples, lastInputGain_, inputGain);
	lastInputGain_ = inputGain;
	updateMeterPeak (globalInputMeterPeak_, measureBufferPeak (buffer, numSamples));

	// -- DIAGNOSTIC LOG (throttled ~1s) --
#if SATTR_DSP_DEBUG_LOG
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
			LOG_DSP_EVENT (diag);
		}
	}
#endif

	// Capture dry signal AFTER input gain, but BEFORE any loader processing
	// Used for global MIX: dry is unaffected by loader processing, filters, mode, etc.
	if (needsGlobalDry)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			globalDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
		applyGlobalDryAlignDelay (globalDryBuffer, globalDryAlignSamples);
	}

	// -- Helper lambdas --
	// Save dry copy before processing a loader only when mix or diagnostics need it.
	auto saveDry = [&] (const juce::AudioBuffer<float>& src)
	{
		for (int ch = 0; ch < src.getNumChannels(); ++ch)
			loaderDryBuffer.copyFrom (ch, 0, src, ch, 0, numSamples);
	};

	// Process one loader with mode in/out and per-loader mix
	auto processOne = [&] (LoaderState& state, juce::AudioBuffer<float>& buf,
	                        int loaderIndex, int modeIn, int modeOut, float loaderMix)
	{
		const float wetStart = state.lastMix;
		const float wetEnd   = loaderMix;
		const float dryStart = 1.0f - wetStart;
		const float dryEnd   = 1.0f - wetEnd;
		const bool needsLoaderMix = std::abs (wetStart - wetEnd) > kMixBypassEps
		                         || std::abs (wetEnd - 1.0f) > kMixBypassEps;
#if SAT_DSP_DIAG
		const bool needsLoaderDryForDiag = (loaderIndex == diagLoaderIndex);
#else
		constexpr bool needsLoaderDryForDiag = false;
#endif
		const bool needsLoaderDry = needsLoaderMix || needsLoaderDryForDiag;

		if (! needsLoaderDryForDiag
			&& std::abs (wetStart) <= kMixBypassEps
			&& std::abs (wetEnd) <= kMixBypassEps)
		{
			// A fully dry, stable loader contributes the input unchanged. Avoid
			// spending oversampling/series CPU on a wet path that will be muted.
			state.lastMix = wetEnd;
			return;
		}

		if (needsLoaderDry)
		{
			saveDry (buf);
			const float dryDelaySamples = loaderIndex == 0 ? dryAlignSamplesA : (loaderIndex == 1 ? dryAlignSamplesB : dryAlignSamplesC);
			applyDryAlignDelay (loaderDryBuffer, dryDelaySamples, loaderIndex);
		}

		applyMidSideInputMode (buf, modeIn, numSamples);
		const bool monoCoreOptimized = (modeIn == 2 || modeIn == 3) && buf.getNumChannels() >= 2;
		processLoader (state, buf, loaderIndex, monoCoreOptimized);
		applyMidSideOutputMode (buf, modeOut, numSamples);

		const bool invertWet = loaderIndex == 0 ? invA : (loaderIndex == 1 ? invB : invC);
		if (invertWet)
		{
			for (int ch = 0; ch < buf.getNumChannels(); ++ch)
				juce::FloatVectorOperations::multiply (buf.getWritePointer (ch), -1.0f, numSamples);
		}
#if SAT_DSP_DIAG
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
#endif

		if (needsLoaderMix)
		{
			for (int ch = 0; ch < buf.getNumChannels(); ++ch)
			{
				buf.applyGainRamp (ch, 0, numSamples, wetStart, wetEnd);
				buf.addFromWithRamp (ch, 0, loaderDryBuffer.getReadPointer (ch), numSamples,
				                     dryStart, dryEnd);
			}
		}
		state.lastMix = wetEnd;
	};

	// Count how many loaders are active for parallel routing decisions.
	auto countEnabled = [&] (bool a, bool b, bool c) -> int
	{
		return (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
	};

	// -- ROUTING --
	// Route 0: A->B->C   (full series)
	// Route 1: A|B|C     (all parallel)
	// Route 2: A->B|C    (series A->B, C parallel)
	// Route 3: A|B->C    (A parallel, series B->C)
	// Route 4: (A|B)->C  (A and B parallel, then C in series)
	// Route 5: A->(B|C)  (A in series, then B and C in parallel)

	if (route == 1) // A|B|C - all parallel
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

			// Sum active buffers - M/S bus-aware
			const bool anyMSBus = (activeA && sumBusA != 0)
			                   || (activeB && sumBusB != 0)
			                   || (activeC && sumBusC != 0);

			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				// M/S bus routing: each loader contributes to ST, +M, or -S bus
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
				// Fast path: all ST - simple L+R addition (no M/S overhead)
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

		}
		else if (activeA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		else if (activeB)
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		else if (activeC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 2) // A->B|C - series A->B, C parallel
	{
		const bool seriesActive = activeA || activeB;
		if (seriesActive && activeC)
		{
			// Copy input for parallel path C
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Series path: A->B stays in buffer
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			// Parallel path C
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);
			// Sum both paths - M/S bus-aware.
			// Follow the actual last active stage on the series side.
			const int seriesBus = activeB ? sumBusB : sumBusA;
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
		}
		else
		{
			// Only one path active - series or C alone
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 3) // A|B->C - A parallel, series B->C
	{
		const bool seriesActive = activeB || activeC;
		if (activeA && seriesActive)
		{
			// Copy input for parallel path A
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Parallel path A
			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			// Series path: B->C stays in buffer
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
			// Sum both paths - M/S bus-aware.
			// Follow the actual last active stage on the series side.
			const int parallelBus = sumBusA;
			const int seriesBus = activeC ? sumBusC : sumBusB;
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
		}
		else
		{
			// Only one path active
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 4) // (A|B)->C - A and B parallel, then C in series
	{
		const int numParallel = (activeA ? 1 : 0) + (activeB ? 1 : 0);

		if (numParallel >= 2)
		{
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
				tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			}

			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);

			const bool anyMSBus = (sumBusA != 0) || (sumBusB != 0);
			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);
				const auto* aL = tempBufferA.getReadPointer (0);
				const auto* aR = tempBufferA.getReadPointer (1);
				const auto* bL = tempBufferB.getReadPointer (0);
				const auto* bR = tempBufferB.getReadPointer (1);

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (aL[i], aR[i], sumBusA, stL, stR, midBus, sideBus);
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				buffer.clear();
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				{
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
				}
			}

		}
		else if (activeA)
		{
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		}
		else if (activeB)
		{
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		}

		if (activeC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 5) // A->(B|C) - A in series, then B and C in parallel
	{
		if (activeA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);

		const int numParallel = (activeB ? 1 : 0) + (activeC ? 1 : 0);

		if (numParallel >= 2)
		{
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			}

			processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);

			const bool anyMSBus = (sumBusB != 0) || (sumBusC != 0);
			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);
				const auto* bL = tempBufferB.getReadPointer (0);
				const auto* bR = tempBufferB.getReadPointer (1);
				const auto* cL = tempBufferC.getReadPointer (0);
				const auto* cR = tempBufferC.getReadPointer (1);

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					injectMSBus (cL[i], cR[i], sumBusC, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				buffer.clear();
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				{
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
				}
			}

		}
		else if (activeB)
		{
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		}
		else if (activeC)
		{
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else // route == 0: A->B->C (full series)
	{
		// Full series naturally feeds one stage into the next.
		if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}

	// -- MATCH: Tilt EQ targeting a spectral profile --
	// Target slopes: White=0, Pink=-3, Brown=-6, Bright=+3, Bright+=+6 dB/oct
	{
		const int matchProfile = loadRelaxedInt (pMatch);

		if (matchProfile != 0) // 0 = None
		{
			const float targetSlope = kTargetSlopes[juce::jlimit (0, kNumTargetSlopes - 1, matchProfile)];

			// No extra stage slope to compensate - apply target slope directly
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

	// -- NORM: static peak-normalize wet signal to target level --
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
				// After warmup: peak follower is reliable - allow full range.
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
			// Reset state when off - next enable will re-measure
			normPeakFollower_  = 0.0f;
			normSmoothedGain_  = 1.0f;
			normWarmupSamples_ = 0;
		}
	}

	// -- Invert Polarity / Stereo (WET path: before wet DC/limiter and mix) --
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

	// Loader outputs are AC-coupled before their RAW/Clipper ceiling. The final
	// output bus has its own DC blocker and optional Safe Clip after global OUT.

	// -- User limiter (WET: after wet-only post processing, before global dry/wet mix) --
	if (limMode == 1)
	{
		if (buffer.getNumChannels() >= 2)
			applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples,
			              limThreshLinStart, limThreshLin);
		else if (buffer.getNumChannels() == 1)
			applyLimiterMono (buffer.getWritePointer (0), numSamples,
			                  limThreshLinStart, limThreshLin);
	}


	// Global MIX: blend unprocessed dry with fully processed wet
	// dry = input after gain, wet = after all loader processing (mode + effects)
	if (needsGlobalDry || needsGlobalWetGain)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			if (needsGlobalWetGain)
				buffer.applyGainRamp (ch, 0, numSamples, lastGlobalWetMix_, globalWetEnd);
			if (needsGlobalDry)
				buffer.addFromWithRamp (ch, 0, globalDryBuffer.getReadPointer (ch), numSamples,
				                        lastGlobalDryMix_, globalDryEnd);
		}
	}
	lastGlobalWetMix_ = globalWetEnd;
	lastGlobalDryMix_ = globalDryEnd;

	// -- Flush denormals in global filter states (per-block, near-zero cost) --
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
	const float outputGain = gainFaderDecibelsToGain (loadRelaxed (pOutput));
	for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		buffer.applyGainRamp (ch, 0, numSamples, lastOutputGain_, outputGain);
	lastOutputGain_ = outputGain;

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

	// -- User limiter (GLOBAL: after output gain, before safety clip) --
	if (limMode == 2)
	{
		if (buffer.getNumChannels() >= 2)
			applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples,
			              limThreshLinStart, limThreshLin);
		else if (buffer.getNumChannels() == 1)
			applyLimiterMono (buffer.getWritePointer (0), numSamples,
			                  limThreshLinStart, limThreshLin);
	}

	// -- Invert Polarity / Stereo (GLOBAL mode: after Limiter GLOBAL, before safety clip) --
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

	// Final output AC coupling. Per-loader blockers run before RAW/Safe Clip to
	// avoid post-ceiling overshoot, but global mix/OUT/limiter/routing can still
	// leave residual DC. Keep this last in the audible chain so PluginDoctor and
	// downstream processors see an AC-coupled output.
	{
		const float R = cachedOutputDcBlockR_;
		const int channelsToFilter = juce::jmin (2, buffer.getNumChannels());
		for (int ch = 0; ch < channelsToFilter; ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			float xPrev = dcBlockX_[ch];
			float yPrev = dcBlockY_[ch];
			for (int n = 0; n < numSamples; ++n)
			{
				const float in = data[n];
				const float out = in - xPrev + R * yPrev;
				xPrev = in;
				yPrev = out;
				data[n] = out;
			}
			dcBlockX_[ch] = xPrev;
			dcBlockY_[ch] = yPrev;
		}
	}

	// Optional final sample-safe ceiling. This must live after the final AC
	// coupling stage; otherwise the DC blocker or later global stages can create
	// samples above the requested ceiling.
	if (isSafeClipModeEnabled())
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int n = 0; n < numSamples; ++n)
				data[n] = juce::jlimit (-kSafeClipCeiling, kSafeClipCeiling, data[n]);
		}
	}
	updateMeterPeak (globalOutputMeterPeak_, measureBufferPeak (buffer, numSamples));

	// Safety hard-limiter: prevent catastrophic output only (NaN/Inf runaway).
	// Set very high (+48 dBFS) so it never engages during normal operation.
	{
#if SATTR_DSP_DEBUG_LOG
		// Log output levels before safety limiter (throttled - same ~1s cadence)
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
				LOG_DSP_EVENT (d);
			}
		}
#endif
		constexpr float kSafetyLimit = 251.19f; // +48 dBFS - only catches runaways
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int n = 0; n < numSamples; ++n)
			{
				float s = data[n];
				// NaN/Inf guard
				if (! std::isfinite (s)) { data[n] = 0.0f; continue; }
				data[n] = juce::jlimit (-kSafetyLimit, kSafetyLimit, s);
			}
		}
	}

#if SAT_DSP_DIAG
	// -- Final peak + diagnostics snapshot --
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
		const LoaderState* activeState = diagIdx == 2 ? &stateC : (diagIdx == 1 ? &stateB : &stateA);

		auto diagPick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
			-> std::atomic<float>* { return diagIdx == 0 ? a : (diagIdx == 1 ? b : c); };

		snap.model        = loadRelaxedInt (diagPick (pSatTypeA, pSatTypeB, pSatTypeC));
		snap.seriesCount  = juce::jlimit (1, 4, loadRelaxedInt (diagPick (pSeriesA, pSeriesB, pSeriesC)));
		snap.osOrder      = loadRelaxedInt (pOversample);
		snap.girth        = loadRelaxed (diagPick (pSatCharA, pSatCharB, pSatCharC));
		snap.mod          = loadRelaxed (diagPick (pSatTypeCtrlA,   pSatTypeCtrlB,   pSatTypeCtrlC));
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
		float maxSagEnv = 0.0f;
		for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
			maxSagEnv = std::max (maxSagEnv, ss.sagEnvelope[sp][0]);
		snap.sagEnvelope     = maxSagEnv;
		snap.sagLastPass     = ss.sagEnvelope[dbgPass][0];
		// Max filter state magnitude (check for stuck/denormal filters)
		float mf = 0.0f;
		for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
		{
			mf = std::max (mf, std::abs (ss.emphasis[sp][0].preHP));
			mf = std::max (mf, std::abs (ss.emphasis[sp][0].preSh));
			mf = std::max (mf, std::abs (ss.emphasis[sp][0].postHP));
			mf = std::max (mf, std::abs (ss.emphasis[sp][0].postLP));
			mf = std::max (mf, std::abs (ss.interStageLPF[sp][0]));
			mf = std::max (mf, std::abs (ss.transistorPreHP[sp][0]));
			mf = std::max (mf, std::abs (ss.transistorPreEdge[sp][0]));
			mf = std::max (mf, std::abs (ss.transistorPostLP[sp][0]));
			mf = std::max (mf, std::abs (ss.bumpZ1[sp][0]));
			mf = std::max (mf, std::abs (ss.bumpZ2[sp][0]));
		}
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

// ----------------------------------------------------------------
void SATTRAudioProcessor::processLoader (LoaderState& state,
                                         juce::AudioBuffer<float>& buffer,
                                         int loaderIndex,
                                         bool monoCoreOptimized)
{
#if SAT_DSP_DIAG
	SatDiag::Collector* diagCollectorPtr = &_diagCollector;
#else
	SatDiag::Collector* diagCollectorPtr = nullptr;
#endif
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	

	
	// Get runtime parameters (cached pointers - no hash lookup)
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
	const float instabilityAmt = loadRelaxed (pick (pInstabilityA, pInstabilityB, pInstabilityC));
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
	const float chaosParamSr = juce::jmax (1.0f, static_cast<float> (currentSampleRate));
	const float chaosParamSmoothCoeff = 1.0f - std::exp (-1.0f / (chaosParamSr * 0.010f));

	// Filter / Tilt position
	const int fltPos = loadRelaxedInt (pick (pFilterPosA, pFilterPosB, pFilterPosC));
	// 0=F▼T▼  1=F▲T▲  2=F▲T▼  3=F▼T▲

	// Saturation parameters
	const int   satType  = loadRelaxedInt (pick (pSatTypeA,  pSatTypeB,  pSatTypeC));
	const bool  sidechainEnabled = loadRelaxedBool (pick (pSidechainA, pSidechainB, pSidechainC));
	const float satDrive = loadRelaxed (pick (pSatDriveA, pSatDriveB, pSatDriveC));
	const float satChar = loadRelaxed    (pick (pSatCharA, pSatCharB, pSatCharC));
	const float satTypeCtrl = loadRelaxed    (pick (pSatTypeCtrlA,   pSatTypeCtrlB,   pSatTypeCtrlC));
	const float satBias  = loadRelaxed    (pick (pSatBiasA,  pSatBiasB,  pSatBiasC));
	const float satSag   = loadRelaxed    (pick (pSatSagA,   pSatSagB,   pSatSagC));
	const float satDetail = loadRelaxed   (pick (pDetailA,   pDetailB,   pDetailC));
	const bool  satRaw   = loadRelaxedBool (pick (pSatRawA,   pSatRawB,   pSatRawC));
	const int   osOrder  = loadRelaxedInt (pOversample);
	const float offsetMs = loadRelaxed (pick (pOffsetA, pOffsetB, pOffsetC));

	// Expander parameters
	const bool  expEnabled = loadRelaxedBool (pick (pExpA,       pExpB,       pExpC));
	const bool  expPost    = loadRelaxedBool (pick (pExpOrderA,  pExpOrderB,  pExpOrderC));
	const float expRatio   = loadRelaxed     (pick (pExpRatioA,  pExpRatioB,  pExpRatioC));
	const float expThreshDb = loadRelaxed    (pick (pExpThreshA, pExpThreshB, pExpThreshC));
	const float expKneeDb  = loadRelaxed     (pick (pExpKneeA,   pExpKneeB,   pExpKneeC));
	const float expAtkMs   = loadRelaxed     (pick (pExpAtkA,    pExpAtkB,    pExpAtkC));
	const float expRelMs   = loadRelaxed     (pick (pExpRelA,    pExpRelB,    pExpRelC));
	const float expScHpHz  = loadRelaxed     (pick (pExpScHpA,   pExpScHpB,   pExpScHpC),   kExpScHpDefault);
	const float expScLpHz  = loadRelaxed     (pick (pExpScLpA,   pExpScLpB,   pExpScLpC),   kExpScLpDefault);
	const bool  expScHpOn  = loadRelaxedBool (pick (pExpScHpOnA, pExpScHpOnB, pExpScHpOnC), kExpScHpOnDefault);
	const bool  expScLpOn  = loadRelaxedBool (pick (pExpScLpOnA, pExpScLpOnB, pExpScLpOnC), kExpScLpOnDefault);
	const int   expScHpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                         loadRelaxedInt (pick (pExpScHpSlopeA, pExpScHpSlopeB, pExpScHpSlopeC), kExpScHpSlopeDefault));
	const int   expScLpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                         loadRelaxedInt (pick (pExpScLpSlopeA, pExpScLpSlopeB, pExpScLpSlopeC), kExpScLpSlopeDefault));
	const float expScGainDb = loadRelaxed    (pick (pExpScGainA, pExpScGainB, pExpScGainC), kExpScGainDefault);
	const auto model = static_cast<SatEngine::Model> (
		juce::jlimit (0, (int) SatEngine::Model::NumModels - 1, satType));
	const bool effectiveSatRaw = satRaw && model != SatEngine::Model::Clipper;
	const bool filterPre = (fltPos == 1 || fltPos == 2);
	const bool tiltPre   = (fltPos == 1 || fltPos == 3);
	const bool chaosDriveEnabled = chaosEnabled;
	const bool chaosFilterEnabledForProc = chaosFilterEnabled;
	const bool expanderEnabled = expEnabled;
	
	// (FRED processing happens after saturation + filters)
	
	// 1. INPUT GAIN (IN)
	{
		const float inGain = gainFaderDecibelsToGain (inDb);
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastInGain, inGain);
		state.lastInGain = inGain;
		updateMeterPeak (loaderInputMeterPeak_[loaderIndex], measureBufferPeak (buffer, numSamples));
	}

	// 2. OUTPUT GAIN (OUT)
	// Intentionally NOT applied here.
	// Per-loader OUT must not affect the drive into the distortion black box;
	// only IN should change saturation input level. OUT is applied at the end
	// of the loader after the black-box and post-routing stages.

	// -- Tilt EQ lambda (PRE/POST saturation) --
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
		float b0 = state.tiltB0, b1 = state.tiltB1, a1 = state.tiltA1;
		const float tb0 = state.tiltTargetB0, tb1 = state.tiltTargetB1, ta1 = state.tiltTargetA1;

		auto* leftData = buffer.getWritePointer (0);
		auto* rightData = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;
		float leftState = state.tiltState[0];
		float rightState = state.tiltState[1];

		for (int i = 0; i < numSamples; ++i)
		{
			b0 += (tb0 - b0) * smoothCoeff;
			b1 += (tb1 - b1) * smoothCoeff;
			a1 += (ta1 - a1) * smoothCoeff;

			const float leftX = leftData[i];
			const float leftY = b0 * leftX + leftState;
			leftState = b1 * leftX - a1 * leftY;
			leftData[i] = leftY;

			if (rightData != nullptr)
			{
				const float rightX = rightData[i];
				const float rightY = b0 * rightX + rightState;
				rightState = b1 * rightX - a1 * rightY;
				rightData[i] = rightY;
			}
		}

		state.tiltState[0] = leftState;
		state.tiltState[1] = rightState;
		state.tiltB0 = b0; state.tiltB1 = b1; state.tiltA1 = a1;
	}
	else if (std::abs (state.lastTiltDb) > 0.05f)
	{
		state.lastTiltDb = 0.0f;
		state.tiltB0 = 1.0f; state.tiltB1 = 0.0f; state.tiltA1 = 0.0f;
		state.tiltTargetB0 = 1.0f; state.tiltTargetB1 = 0.0f; state.tiltTargetA1 = 0.0f;
		state.tiltState[0] = state.tiltState[1] = 0.0f;
	}
	}; // applyTilt

	// -- HP + LP Filters lambda (6/12/24 dB/oct) --
	const bool chaosFilterActive = chaosFilterEnabledForProc
		&& (chaosAmtFilter > 0.01f || (state.chaosFilterParamSmoothReady && state.chaosFilterAmtSmoothed > 0.01f));
	if (! chaosFilterActive)
	{
		state.chaosFilterAmtSmoothed = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtFilter);
		state.chaosFilterSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpdFilter);
		state.chaosFilterParamSmoothReady = false;
	}
	auto applyFilters = [&]()
	{
		constexpr float kSmoothCoeff = 0.9955f; // ~5ms @ 44.1kHz
		const float oneMinusCoeff = 1.0f - kSmoothCoeff;
		const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;
		const bool processHp = hpOn || chaosFilterActive;
		const bool processLp = lpOn || chaosFilterActive;

		if (! processHp && ! processLp)
		{
			const float smoothPower = std::pow (kSmoothCoeff, (float) numSamples);
			state.smoothedHpFreq = hpFreq + (state.smoothedHpFreq - hpFreq) * smoothPower;
			state.smoothedLpFreq = lpFreq + (state.smoothedLpFreq - lpFreq) * smoothPower;
			state.filterCoeffCountdown = 0;
			return;
		}

		const float sr = (float) currentSampleRate;
		const float hpBase = hpOn ? hpFreq : kFilterFreqMin;
		const float lpBase = lpOn ? lpFreq : kFilterFreqMax;
		if (chaosFilterActive && ! state.chaosFilterParamSmoothReady)
		{
			if (! hpOn)
				state.smoothedHpFreq = kFilterFreqMin;
			if (! lpOn)
				state.smoothedLpFreq = kFilterFreqMax;
			state.filterCoeffCountdown = 0;
		}

		auto updateFilterCoefficients = [&]()
		{
			// HP: recalc if smoothed frequency or slope changed
			const float clampedHp = juce::jlimit (20.0f, maxFreq, state.smoothedHpFreq);
			if (std::abs (clampedHp - state.lastHpFreq) > 0.01f || hpSlope != state.lastHpSlope)
			{
				if (hpSlope == 0) // 6 dB/oct - first-order
				{
					*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeFirstOrderHighPass (
						currentSampleRate, clampedHp);
				}
				else if (hpSlope == 1) // 12 dB/oct - Butterworth biquad
				{
					*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kSqrt2Over2);
				}
				else // 24 dB/oct - cascaded biquad pair
				{
					*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q1);
					*state.hpFilter2.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q2);
				}
				state.lastHpFreq = clampedHp;
				state.lastHpSlope = hpSlope;
			}

			// LP: recalc if smoothed frequency or slope changed
			const float clampedLp = juce::jlimit (20.0f, maxFreq, state.smoothedLpFreq);
			if (std::abs (clampedLp - state.lastLpFreq) > 0.01f || lpSlope != state.lastLpSlope)
			{
				if (lpSlope == 0) // 6 dB/oct - first-order
				{
					*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeFirstOrderLowPass (
						currentSampleRate, clampedLp);
				}
				else if (lpSlope == 1) // 12 dB/oct - Butterworth biquad
				{
					*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kSqrt2Over2);
				}
				else // 24 dB/oct - cascaded biquad pair
				{
					*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q1);
					*state.lpFilter2.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q2);
				}
				state.lastLpFreq = clampedLp;
				state.lastLpSlope = lpSlope;
			}
		};

		juce::dsp::AudioBlock<float> block (buffer);
		int segmentStart = 0;

		auto processSegment = [&] (int segmentEnd)
		{
			const int segmentLength = segmentEnd - segmentStart;
			if (segmentLength <= 0)
				return;

			auto subBlock = block.getSubBlock ((size_t) segmentStart, (size_t) segmentLength);
			juce::dsp::ProcessContextReplacing<float> context (subBlock);

			// Apply HP filter (1 or 2 stages depending on slope).
			// Also apply when chaos filter is active, even if HP knob is off (full-range sweep).
			if (processHp && (chaosFilterActive || state.smoothedHpFreq >= 21.0f))
			{
				state.hpFilter.process (context);
				if (hpSlope == 2) // 24 dB/oct: second stage
					state.hpFilter2.process (context);
			}

			// Apply LP filter (1 or 2 stages depending on slope).
			// Also apply when chaos filter is active, even if LP knob is off (full-range sweep).
			if (processLp && (chaosFilterActive || state.smoothedLpFreq <= 19900.0f))
			{
				state.lpFilter.process (context);
				if (lpSlope == 2) // 24 dB/oct: second stage
					state.lpFilter2.process (context);
			}

			segmentStart = segmentEnd;
		};

		auto advanceFilterTargets = [&]()
		{
			float hpTarget = hpFreq;
			float lpTarget = lpFreq;

			if (chaosFilterActive)
			{
				const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtFilter);
				const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpdFilter);
				if (! state.chaosFilterParamSmoothReady)
				{
					state.chaosFilterParamSmoothReady = true;
					if (state.chaosFilterSpdSmoothed <= 0.0f)
						state.chaosFilterSpdSmoothed = targetSpd;
				}

				state.chaosFilterAmtSmoothed += (targetAmt - state.chaosFilterAmtSmoothed) * chaosParamSmoothCoeff;
				const float filterSpdLog = std::log (juce::jmax (kChaosSpdMin, state.chaosFilterSpdSmoothed));
				const float filterTargetSpdLog = std::log (targetSpd);
				state.chaosFilterSpdSmoothed = std::exp (filterSpdLog + (filterTargetSpdLog - filterSpdLog) * chaosParamSmoothCoeff);

				const float amountNorm = state.chaosFilterAmtSmoothed * 0.01f;
				const float chaosFilterMaxOct = amountNorm * 2.0f;  // +/-2 octaves at 100%
				const float shPeriodSamples = sr / juce::jmax (kChaosSpdMin, state.chaosFilterSpdSmoothed);

				advanceChaosEngine (state.chaosFPrev, state.chaosFCurr, state.chaosFNext,
				                    state.chaosFPhase, state.chaosFDriftPhase, state.chaosFDriftFreqHz,
				                    state.chaosFOut[0], state.chaosFRng, shPeriodSamples, amountNorm, sr);

				const float octaveShift = state.chaosFOut[0] * chaosFilterMaxOct;
				const float freqMult = std::exp2 (octaveShift);
				hpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * freqMult);
				lpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * freqMult);
			}

			state.smoothedHpFreq += (hpTarget - state.smoothedHpFreq) * oneMinusCoeff;
			state.smoothedLpFreq += (lpTarget - state.smoothedLpFreq) * oneMinusCoeff;
		};

		for (int i = 0; i < numSamples; ++i)
		{
			advanceFilterTargets();

			// Countdown is sample-based: samples before this boundary use the previous coefficients.
			if (--state.filterCoeffCountdown <= 0)
			{
				processSegment (i);
				state.filterCoeffCountdown = LoaderState::kFilterCoeffUpdateInterval;
				updateFilterCoefficients();
			}
		}

		processSegment (numSamples);
	}; // applyFilters

	// -- PRE-saturation: apply tilt/filter if requested --
	if (filterPre) applyFilters();
	if (tiltPre)   applyTilt();

	// -- Expander / Noise Gate lambda --
	auto applyExpander = [&]()
	{
		const float sr = (float) currentSampleRate;
		const float attCoeff = std::exp (-1.0f / (sr * juce::jmax (0.00001f, expAtkMs * 0.001f)));
		const float relCoeff = std::exp (-1.0f / (sr * juce::jmax (0.001f,   expRelMs * 0.001f)));
		const float ratio = juce::jlimit (kExpRatioMin, kExpRatioMax, expRatio);
		if (!expanderEnabled || std::abs (ratio - 1.0f) <= 0.01f)
			return;
		const float kneeDb = juce::jlimit (kExpKneeMin, kExpKneeMax, expKneeDb);
		const float slope = ratio - 1.0f;  // >0 = downward expansion, <0 = upward compression below threshold
		const float maxDetectorFreq = juce::jmin (kExpScFreqMax, sr * 0.45f);
		float detectorHpHz = juce::jlimit (kExpScFreqMin, maxDetectorFreq, expScHpHz);
		const float detectorLpHz = juce::jlimit (kExpScFreqMin, maxDetectorFreq, expScLpHz);
		if (detectorHpHz >= detectorLpHz)
			detectorHpHz = juce::jmax (kExpScFreqMin, detectorLpHz * 0.95f);

		const bool useDetectorHp = expScHpOn;
		const bool useDetectorLp = expScLpOn;
		const BiquadCoefficients detectorHp = useDetectorHp ? makeDetectorHighPassForSlope (detectorHpHz, sr, expScHpSlope, false) : BiquadCoefficients {};
		const BiquadCoefficients detectorHp2 = (useDetectorHp && expScHpSlope >= 2) ? makeDetectorHighPassForSlope (detectorHpHz, sr, expScHpSlope, true) : BiquadCoefficients {};
		const BiquadCoefficients detectorLp = useDetectorLp ? makeDetectorLowPassForSlope (detectorLpHz, sr, expScLpSlope, false) : BiquadCoefficients {};
		const BiquadCoefficients detectorLp2 = (useDetectorLp && expScLpSlope >= 2) ? makeDetectorLowPassForSlope (detectorLpHz, sr, expScLpSlope, true) : BiquadCoefficients {};
		if (!useDetectorHp)
		{
			state.expScHpState.reset();
			state.expScHpState2.reset();
		}
		else if (expScHpSlope < 2)
		{
			state.expScHpState2.reset();
		}
		if (!useDetectorLp)
		{
			state.expScLpState.reset();
			state.expScLpState2.reset();
		}
		else if (expScLpSlope < 2)
		{
			state.expScLpState2.reset();
		}

		const float detectorGainTarget = gainFaderDecibelsToGain (juce::jlimit (kExpScGainMin, kExpScGainMax, expScGainDb));
		float detectorGain = state.expScLastGain;
		const float detectorGainStep = numSamples > 0 ? (detectorGainTarget - detectorGain) / (float) numSamples : 0.0f;

		const int chCount = juce::jmin (numChannels, 2);
		float* channelData[2] = { nullptr, nullptr };
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);

		for (int i = 0; i < numSamples; ++i)
		{
			// Stereo-linked peak detection
			float peak = 0.0f;
			for (int ch = 0; ch < chCount; ++ch)
			{
				float detectorSample = channelData[ch][i];
				if (useDetectorHp)
				{
					detectorSample = processDetectorBiquad (detectorSample, state.expScHpState, detectorHp, ch);
					if (expScHpSlope >= 2)
						detectorSample = processDetectorBiquad (detectorSample, state.expScHpState2, detectorHp2, ch);
				}
				if (useDetectorLp)
				{
					detectorSample = processDetectorBiquad (detectorSample, state.expScLpState, detectorLp, ch);
					if (expScLpSlope >= 2)
						detectorSample = processDetectorBiquad (detectorSample, state.expScLpState2, detectorLp2, ch);
				}
				peak = juce::jmax (peak, std::abs (detectorSample * detectorGain));
			}
			detectorGain += detectorGainStep;

			// Envelope follower (stereo-linked)
			float& env = state.expLinkedEnv;
			if (peak > env)
				env = attCoeff * env + (1.0f - attCoeff) * peak;
			else
				env = relCoeff * env + (1.0f - relCoeff) * peak;

			// Below-threshold expansion gain
			float gr = 1.0f;
			if (env > 1.0e-12f)
			{
				const float envDb = 20.0f * std::log10 (env);
				float gainDeltaDb = 0.0f;

				if (kneeDb <= 1.0e-6f)
				{
					if (envDb < expThreshDb)
						gainDeltaDb = slope * (expThreshDb - envDb);
				}
				else
				{
					const float deltaBelowThreshDb = expThreshDb - envDb;
					const float halfKneeDb = 0.5f * kneeDb;

					if (deltaBelowThreshDb >= halfKneeDb)
					{
						gainDeltaDb = slope * deltaBelowThreshDb;
					}
					else if (deltaBelowThreshDb > -halfKneeDb)
					{
						const float kneePos = deltaBelowThreshDb + halfKneeDb; // 0..kneeDb
						gainDeltaDb = slope * (kneePos * kneePos) / (2.0f * kneeDb);
					}
				}

				gainDeltaDb = juce::jlimit (-120.0f, 120.0f, gainDeltaDb);
				gr = fastDecibelsToGain (-gainDeltaDb);
			}

			for (int ch = 0; ch < chCount; ++ch)
				channelData[ch][i] *= gr;
		}

		state.expScLastGain = detectorGainTarget;
	};

	// -- PRE-saturation expander --
	if (expanderEnabled && !expPost) applyExpander();

	// 2.5. CHAOS D (micro-delay + gain modulation - BEFORE saturation)
	const bool chaosDriveActive = chaosDriveEnabled
		&& (chaosAmt > 0.01f || (state.chaosDriveParamSmoothReady && state.chaosDriveAmtSmoothed > 0.01f));
	if (chaosDriveActive)
	{
		const float maxDelaySec = 0.005f; // -5ms max
		const float sr = (float) currentSampleRate;
		const float chaosDelaySmoothCoeff = 1.0f - std::exp (-1.0f / (sr * 0.002f));
		
		const int chCount = juce::jmin (numChannels, 2);
		const int delayBufLen = LoaderState::kChaosDelayMaxSamples;
		const int mask = delayBufLen - 1;
		
		float* channelData[2] = { nullptr, nullptr };
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		for (int i = 0; i < numSamples; ++i)
		{
			const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmt);
			const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpd);
			if (! state.chaosDriveParamSmoothReady)
			{
				state.chaosDriveParamSmoothReady = true;
				if (state.chaosDriveSpdSmoothed <= 0.0f)
					state.chaosDriveSpdSmoothed = targetSpd;
			}

			state.chaosDriveAmtSmoothed += (targetAmt - state.chaosDriveAmtSmoothed) * chaosParamSmoothCoeff;
			const float driveSpdLog = std::log (juce::jmax (kChaosSpdMin, state.chaosDriveSpdSmoothed));
			const float driveTargetSpdLog = std::log (targetSpd);
			state.chaosDriveSpdSmoothed = std::exp (driveSpdLog + (driveTargetSpdLog - driveSpdLog) * chaosParamSmoothCoeff);

			const float amountNorm = state.chaosDriveAmtSmoothed * 0.01f; // 0..1
			const float maxDelaySamples = amountNorm * maxDelaySec * sr;
			const float shPeriodSamples = sr / juce::jmax (kChaosSpdMin, state.chaosDriveSpdSmoothed);
			const float chaosGainMaxDb = amountNorm * 1.0f; // -1dB at 100%

			// Advance smooth S&H + Drift D+G engines (per-channel)
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
			
			// Write current sample into offset buffer
			const int wp = state.chaosDelayWritePos;
			for (int ch = 0; ch < chCount; ++ch)
				state.chaosDelayBuffer[ch][wp] = channelData[ch][i];
			
			// Read with per-channel offset using Lagrange interpolation
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float targetDelaySamp = juce::jlimit (0.0f, (float) (delayBufLen - 2),
				                                            maxDelaySamples + state.chaosDOut[ch] * maxDelaySamples);
				float& smoothedDelaySamp = state.chaosDelaySmoothedSamples[ch];
				if (! state.chaosDelaySmoothReady[ch])
				{
					smoothedDelaySamp = targetDelaySamp;
					state.chaosDelaySmoothReady[ch] = true;
				}
				else
				{
					smoothedDelaySamp += (targetDelaySamp - smoothedDelaySamp) * chaosDelaySmoothCoeff;
				}

				const float delaySamp = smoothedDelaySamp;
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
				
				// Per-channel gain modulation (-1 dB, fast dB-to-linear)
				const float gainDb  = state.chaosGOut[ch] * chaosGainMaxDb;
				const float ex = gainDb * 0.16609640474f;
				const float exln2 = ex * 0.6931472f;
				const float gainLin = 1.0f + exln2 * (1.0f + exln2 * 0.5f);
				channelData[ch][i] *= gainLin;
			}
			
			state.chaosDelayWritePos = (wp + 1) & mask;
		}
	}
	else
	{
		state.chaosDelaySmoothedSamples[0] = state.chaosDelaySmoothedSamples[1] = 0.0f;
		state.chaosDelaySmoothReady[0] = state.chaosDelaySmoothReady[1] = false;
		state.chaosDriveAmtSmoothed = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmt);
		state.chaosDriveSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpd);
		state.chaosDriveParamSmoothReady = false;
	}

	// 3. SATURATION (with optional oversampling + series chaining)
	if (model == SatEngine::Model::NAM)
	{
		state.lastSidechainPreGain = 1.0f;
		state.satState.deferOverdriveAPostEq = false;
		state.satState.deferFullKlonPostEq = false;
		state.satState.lastModel = SatEngine::Model::Clean;

		auto nam = loadNamModelAtomic (&state.namModel);
		if (nam != nullptr && nam->isLoaded())
		{
			if (monoCoreOptimized)
				nam->processMonoToStereo (buffer, numSamples);
			else
				nam->process (buffer, numSamples);
		}
	}
	else if (model != SatEngine::Model::Clean)
	{
		constexpr float kSidechainPreDriveMaxDb = 24.0f;
		constexpr float kSidechainDriveBoostMax = 0.0f; //0.33f;
		const float sidechainDriveAmount = sidechainEnabled
			? juce::jlimit (0.0f, 1.0f, sidechainDriveAmount_[loaderIndex])
			: 0.0f;
		const float sidechainPreGain = sidechainEnabled
			? fastDecibelsToGain (sidechainDriveAmount * kSidechainPreDriveMaxDb)
			: 1.0f;
		const float effectiveSatDrive = juce::jlimit (kSatDriveMin, kSatDriveMax,
			satDrive + sidechainDriveAmount * kSidechainDriveBoostMax);
		const bool deferFullKlonPostEq = model == SatEngine::Model::OverdriveB
			&& ! effectiveSatRaw;
		const bool deferOverdriveAPostEq = model == SatEngine::Model::OverdriveA
			&& ! effectiveSatRaw;
		state.satState.deferOverdriveAPostEq = deferOverdriveAPostEq;
		state.satState.deferFullKlonPostEq = deferFullKlonPostEq;
		if (! deferOverdriveAPostEq)
			for (int ch = 0; ch < 2; ++ch)
				for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
					state.satState.overdriveANativePost[sp][ch].reset();
		if (! deferFullKlonPostEq)
			for (int ch = 0; ch < 2; ++ch)
				for (int sp = 0; sp < SatEngine::kMaxSeries; ++sp)
					state.satState.overdriveBNativePost[sp][ch].reset();
		if (std::abs (sidechainPreGain - state.lastSidechainPreGain) > 1.0e-5f
		 || std::abs (sidechainPreGain - 1.0f) > 1.0e-5f)
		{
			for (int ch = 0; ch < numChannels; ++ch)
				buffer.applyGainRamp (ch, 0, numSamples, state.lastSidechainPreGain, sidechainPreGain);
			state.lastSidechainPreGain = sidechainPreGain;
		}

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
			jassert (oversamplingBlockCapacity_ > 0);
			jassert (numSamples <= oversamplingBlockCapacity_);
			const int channelsToProcess = (monoCoreOptimized || numChannels < 2) ? 1 : 2;

			if (channelsToProcess == 1)
			{
				auto& os = *oversamplersMono_[loaderIndex][osOrder - 1];
				auto block = juce::dsp::AudioBlock<float> (buffer).getSingleChannelBlock (0);
				auto osBlock = os.processSamplesUp (block);

				float* osL = osBlock.getChannelPointer (0);
				const int osNumSamples = (int) osBlock.getNumSamples();
				const float osSr = (float) currentSampleRate * (float) os.getOversamplingFactor();

				SatEngine::processBlock (state.satState, osL, osL, osNumSamples,
				                         model, effectiveSatDrive, satChar, satTypeCtrl, satBias, satSag, satDetail,
				                         instabilityAmt, osSr, seriesCount, false, effectiveSatRaw, diagCollectorPtr, 1);

				os.processSamplesDown (block);
				if (numChannels > 1)
					buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);
			}
			else
			{
				auto& os = *oversamplers_[loaderIndex][osOrder - 1];
				auto block = juce::dsp::AudioBlock<float> (buffer);
				auto osBlock = os.processSamplesUp (block);

				float* osL = osBlock.getChannelPointer (0);
				float* osR = osBlock.getChannelPointer (1);
				const int osNumSamples = (int) osBlock.getNumSamples();
				const float osSr = (float) currentSampleRate * (float) os.getOversamplingFactor();

				SatEngine::processBlock (state.satState, osL, osR, osNumSamples,
				                         model, effectiveSatDrive, satChar, satTypeCtrl, satBias, satSag, satDetail,
				                         instabilityAmt, osSr, seriesCount, false, effectiveSatRaw, diagCollectorPtr, 2);

				os.processSamplesDown (block);
			}
		}
		else
		{
			float* dataL = buffer.getWritePointer (0);
			float* dataR = numChannels > 1 ? buffer.getWritePointer (1) : dataL;
			const int channelsToProcess = (monoCoreOptimized || numChannels < 2) ? 1 : 2;

			SatEngine::processBlock (state.satState, dataL, dataR, numSamples,
			                         model, effectiveSatDrive, satChar, satTypeCtrl, satBias, satSag, satDetail, instabilityAmt,
			                         (float) currentSampleRate, seriesCount, true, effectiveSatRaw, diagCollectorPtr, channelsToProcess);
			if (monoCoreOptimized && numChannels > 1)
				buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);
		}

		if (deferOverdriveAPostEq)
		{
			float* dataL = buffer.getWritePointer (0);
			float* dataR = (monoCoreOptimized || numChannels < 2) ? dataL : buffer.getWritePointer (1);
			SatEngine::processDeferredOverdriveAPostEq (state.satState, dataL, dataR, numSamples,
			                                                effectiveSatDrive, 0.0f, seriesCount,
			                                                (float) currentSampleRate, effectiveSatRaw);
		}

		if (deferFullKlonPostEq)
		{
			float* dataL = buffer.getWritePointer (0);
			float* dataR = (monoCoreOptimized || numChannels < 2) ? dataL : buffer.getWritePointer (1);
			SatEngine::processDeferredFullKlonPostEq (state.satState, dataL, dataR, numSamples,
			                                          effectiveSatDrive, 1.0f, seriesCount,
			                                          (float) currentSampleRate, effectiveSatRaw);
		}
		if (monoCoreOptimized && numChannels > 1 && (deferOverdriveAPostEq || deferFullKlonPostEq))
			buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

#if SAT_DSP_DIAG
		// Capture post-saturation peak + click detection (L channel)
		{
			const float* pL = buffer.getReadPointer (0);
			for (int n = 0; n < numSamples; ++n)
				_diagCollector.feedOut (pL[n]);
		}
#endif
	}
	else
	{
		state.lastSidechainPreGain = 1.0f;
		state.satState.deferOverdriveAPostEq = false;
		state.satState.deferFullKlonPostEq = false;

		// Clean bypass skips SatEngine::processBlock, so mark the engine as
		// clean here. Re-entering a nonlinear model will then reset model
		// memory instead of resuming stale Tube/Instability state.
		state.satState.lastModel = SatEngine::Model::Clean;
	}

	auto flushTinyTiltStates = [&]() noexcept
	{
		constexpr float kDnr = 1e-20f;
		if (std::abs (state.tiltState[0]) < kDnr) state.tiltState[0] = 0.0f;
		if (std::abs (state.tiltState[1]) < kDnr) state.tiltState[1] = 0.0f;
	};

	auto namPostPathIsNeutral = [&]() noexcept
	{
		if (model != SatEngine::Model::NAM)
			return false;

		const auto near = [] (float a, float b, float eps) noexcept
		{
			return std::abs (a - b) <= eps;
		};

		const bool tiltNeutral = tiltPre
			|| (std::abs (tiltDb) <= 0.05f && std::abs (state.lastTiltDb) <= 0.05f);

		const bool filtersSettled = filterPre
			|| (! hpOn && ! lpOn && ! chaosFilterActive
				&& near (state.smoothedHpFreq, hpFreq, 0.01f)
				&& near (state.smoothedLpFreq, lpFreq, 0.01f));

		const bool expanderNeutral = ! (expanderEnabled && expPost && std::abs (expRatio - 1.0f) > 0.01f);
		const bool posNeutral = pos <= 0.01f && state.lastPosFreq <= 0.0f && near (state.lastPosGain, 1.0f, 1.0e-4f);
		const bool panNeutral = numChannels < 2
			|| (std::abs (pan - 0.5f) <= 0.001f
				&& near (state.lastPanLeft, 1.0f, 1.0e-4f)
				&& near (state.lastPanRight, 1.0f, 1.0e-4f));
		const bool fredNeutral = fred <= 0.001f;
		const bool outNeutral = std::abs (outDb) <= 0.001f && near (state.lastOutGain, 1.0f, 1.0e-4f);

		return tiltNeutral && filtersSettled && expanderNeutral && posNeutral && panNeutral && fredNeutral && outNeutral;
	};

	if (namPostPathIsNeutral())
	{
		// Keep the align offset line fed even when it is effectively dry; this
		// avoids stale-buffer re-entry if DELAY is enabled after the fast path.
		applyOffset (buffer, offsetMs, loaderIndex);
		flushTinyTiltStates();
		return;
	}

	// -- POST-saturation: apply tilt/filter if not already applied --
	if (!tiltPre)   applyTilt();
	if (!filterPre) applyFilters();

	// -- POST-saturation expander --
	if (expanderEnabled && expPost) applyExpander();

	// 4. DISTANCE (exponential LPF + gain attenuation)
	// 0% = close/bright (no change), 100% = far/dark (HF reduction + volume drop)
	if (pos > 0.01f)
	{
		// Exponential cutoff: 12 kHz * exp(-pos * 2.08) -> pos=0 => 12 kHz, pos=1 => 1.5 kHz
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
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastPosGain, distGain);
		state.lastPosGain = distGain;
	}
	else
	{
		if (state.lastPosFreq > 0.0f)
			state.lastPosFreq = -1.0f; // Mark as inactive
		if (std::abs (state.lastPosGain - 1.0f) > 1.0e-4f)
			for (int ch = 0; ch < numChannels; ++ch)
				buffer.applyGainRamp (ch, 0, numSamples, state.lastPosGain, 1.0f);
		state.lastPosGain = 1.0f;
	}
	
	// 5. PAN
	if (numChannels >= 2)
	{
		float targetLeft = 1.0f;
		float targetRight = 1.0f;
		if (std::abs (pan - 0.5f) > 0.001f)
		{
			const float panAngle = pan * 1.5707963f;
			targetLeft = std::cos (panAngle);
			targetRight = std::sin (panAngle);
		}

		buffer.applyGainRamp (0, 0, numSamples, state.lastPanLeft, targetLeft);
		buffer.applyGainRamp (1, 0, numSamples, state.lastPanRight, targetRight);
		state.lastPanLeft = targetLeft;
		state.lastPanRight = targetRight;
		state.lastPan = pan;
	}
	
	// 5b. DELAY (auto-align compensation)
	// Always run the offset stage so the read head can glide cleanly to/from zero
	// without a hard bypass click or stale-buffer re-entry.
	applyOffset (buffer, offsetMs, loaderIndex);
	
	// 6. ANGLE (off-axis mic simulation)
	// Simulates a second mic at an angle on a guitar cab.
	// Fixed offset of ~159us (-5cm path difference), sample-rate independent.
	// First comb null at ~6.3kHz regardless of sample rate.
	// angle=0: pure on-axis (no effect), angle=1: full off-axis blend
	if (fred > 0.001f)
	{
		float* channelData[2] = { nullptr, nullptr };
		const int chCount = juce::jmin (numChannels, 2);
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		// Fractional offset in samples: 159us - sampleRate
		const float delaySamples = LoaderState::kFredDelayMicros * 1e-6f * (float) currentSampleRate;
		const int delayInt = (int) delaySamples;
		const float delayFrac = delaySamples - (float) delayInt;
		const int bufSize = LoaderState::kFredDelayBufSize;
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

	// Loader wet DC protection. This is deliberately before the RAW/Safe Clip
	// ceiling, so AC coupling cannot create post-ceiling overshoots.
	{
		const bool needsLoaderDcBlock = model != SatEngine::Model::Clean
		                              && model != SatEngine::Model::Clipper
		                              && model != SatEngine::Model::NAM;
		if (needsLoaderDcBlock)
		{
			const float R = cachedOutputDcBlockR_;
			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				float xPrev = state.wetDcBlockX[ch];
				float yPrev = state.wetDcBlockY[ch];
				for (int i = 0; i < numSamples; ++i)
				{
					const float in = data[i];
					const float out = in - xPrev + R * yPrev;
					xPrev = in;
					yPrev = out;
					data[i] = out;
				}
				state.wetDcBlockX[ch] = xPrev;
				state.wetDcBlockY[ch] = yPrev;
			}
		}
		else
		{
			state.wetDcBlockX[0] = state.wetDcBlockX[1] = 0.0f;
			state.wetDcBlockY[0] = state.wetDcBlockY[1] = 0.0f;
		}
	}

	// RAW / CLIPPER ceiling after oversampling reconstruction and loader post stages.
	// SAFE CLIP is intentionally not applied here: it is a final output ceiling
	// after global OUT, limiter/routing and the final AC coupling stage.
	if ((effectiveSatRaw || model == SatEngine::Model::Clipper)
		&& model != SatEngine::Model::Clean && model != SatEngine::Model::NAM)
	{
		const float ceiling = SatEngine::kRawCeiling;
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int i = 0; i < numSamples; ++i)
				data[i] = juce::jlimit (-ceiling, ceiling, data[i]);
		}
	}

	// 7. OUTPUT GAIN (OUT) - final per-loader output trim
	{
		const float outGain = gainFaderDecibelsToGain (outDb);
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastOutGain, outGain);
		state.lastOutGain = outGain;
		updateMeterPeak (loaderOutputMeterPeak_[loaderIndex], measureBufferPeak (buffer, numSamples));
	}
	
	// -- Flush denormals in filter/tilt states (per-block, near-zero cost) --
	{
		flushTinyTiltStates();
	}
}

// ----------------------------------------------------------------
void SATTRAudioProcessor::applyDelaySamples (
	juce::AudioBuffer<float>& buffer,
	float targetDelaySamples,
	juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>& delayLine,
	juce::SmoothedValue<float>& smoother)
{
	const int numSamples  = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	auto* const* writeChannels = buffer.getArrayOfWritePointers();

	smoother.setTargetValue (juce::jlimit (0.0f, 191998.0f, targetDelaySamples));

	// Keep the offset line continuously fed even at zero. The output itself must
	// remain a true wet-only offset; blending dry here creates combing at small
	// offset values even when the loader mix is 100% wet.
	constexpr float kDelayDryEpsSamples = 1.0e-4f;

	if (! smoother.isSmoothing())
	{
		const float currentDelay = smoother.getCurrentValue();
		delayLine.setDelay (currentDelay);

		if (currentDelay <= kDelayDryEpsSamples)
		{
			for (int i = 0; i < numSamples; ++i)
			{
				for (int ch = 0; ch < numChannels; ++ch)
				{
					auto* channelData = writeChannels[ch];
					delayLine.pushSample (ch, channelData[i]);
					delayLine.popSample (ch);
				}
			}

			return;
		}

		for (int i = 0; i < numSamples; ++i)
		{
			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* channelData = writeChannels[ch];
				const float input = channelData[i];
				delayLine.pushSample (ch, input);
				const float delayed = delayLine.popSample (ch);
				channelData[i] = delayed;
			}
		}

		return;
	}
	
	for (int i = 0; i < numSamples; ++i)
	{
		const float currentDelay = smoother.getNextValue();
		delayLine.setDelay (currentDelay);
		
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* channelData = writeChannels[ch];
			const float input = channelData[i];
			delayLine.pushSample (ch, input);
			const float delayed = delayLine.popSample (ch);
			if (currentDelay > kDelayDryEpsSamples)
				channelData[i] = delayed;
		}
	}
}

void SATTRAudioProcessor::applyOffset (juce::AudioBuffer<float>& buffer, float offsetMs, int loaderIndex)
{
	const float targetDelaySamples = juce::jmax (0.0f, offsetMs * 0.001f * static_cast<float> (currentSampleRate));
	auto& delayLine = loaderIndex == 0 ? stateA.delayLine : (loaderIndex == 1 ? stateB.delayLine : stateC.delayLine);
	auto& smoother  = loaderIndex == 0 ? stateA.smoothedDelay : (loaderIndex == 1 ? stateB.smoothedDelay : stateC.smoothedDelay);
	applyDelaySamples (buffer, targetDelaySamples, delayLine, smoother);
}

void SATTRAudioProcessor::applyDryAlignDelay (juce::AudioBuffer<float>& buffer, float targetDelaySamples, int loaderIndex)
{
	auto& delayLine = loaderIndex == 0 ? stateA.dryDelayLine : (loaderIndex == 1 ? stateB.dryDelayLine : stateC.dryDelayLine);
	auto& smoother  = loaderIndex == 0 ? stateA.smoothedDryDelay : (loaderIndex == 1 ? stateB.smoothedDryDelay : stateC.smoothedDryDelay);
	applyDelaySamples (buffer, targetDelaySamples, delayLine, smoother);
}

void SATTRAudioProcessor::applyGlobalDryAlignDelay (juce::AudioBuffer<float>& buffer, float targetDelaySamples)
{
	applyDelaySamples (buffer, targetDelaySamples, globalDryDelayLine, smoothedGlobalDryDelay);
}

void SATTRAudioProcessor::applyMidSideOutputMode (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
{
	if (modeVal >= 1 && modeVal <= 3 && buf.getNumChannels() >= 2)
	{
		auto* L = buf.getWritePointer (0);
		auto* R = buf.getWritePointer (1);
		for (int i = 0; i < nSamples; ++i)
		{
			if (modeVal == 1) // M/S decode
			{
				const float mid = L[i];
				const float side = R[i];
				L[i] = mid + side;
				R[i] = mid - side;
			}
			else if (modeVal == 2) // MID output: dual mono mid
			{
				const float mono = (L[i] + R[i]) * 0.5f;
				L[i] = mono;
				R[i] = mono;
			}
			else // SIDE output: stereo-encoded side
			{
				const float mono = (L[i] + R[i]) * 0.5f;
				L[i] = mono;
				R[i] = -mono;
			}
		}
	}
}

// ----------------------------------------------------------------
void SATTRAudioProcessor::calculateAutoAlignment()
{
	const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
	const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
	const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;
	const auto alignTypeA = static_cast<SatEngine::Model> (
		juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
		              static_cast<int> (std::lround (pSatTypeA->load()))));
	const auto alignTypeB = static_cast<SatEngine::Model> (
		juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
		              static_cast<int> (std::lround (pSatTypeB->load()))));
	const auto alignTypeC = static_cast<SatEngine::Model> (
		juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
		              static_cast<int> (std::lround (pSatTypeC->load()))));
	const bool activeA = enabledA && (alignTypeA != SatEngine::Model::NAM || isNamModelLoadedForLoader (0));
	const bool activeB = enabledB && (alignTypeB != SatEngine::Model::NAM || isNamModelLoadedForLoader (1));
	const bool activeC = enabledC && (alignTypeC != SatEngine::Model::NAM || isNamModelLoadedForLoader (2));

	const bool dryAlignRequested = isDryAlignModeEnabled();
	const bool anyEnabled = activeA || activeB || activeC;
	if (! anyEnabled)
	{
		dryAlignRuntimeActive_.store (false, std::memory_order_release);
		return;
	}

	// -- Generate synthetic alignment probes --
	// Use a deterministic broadband probe and run it through the actual model
	// path. This is more reliable than an impulse for NAM/nonlinear pairings.
	const int irLen = 2048;
	const float sr = static_cast<float> (currentSampleRate);
	juce::AudioBuffer<float> alignSource (1, irLen);
	{
		auto* src = alignSource.getWritePointer (0);
		juce::uint32 rng = 0x4d595df4u;
		for (int n = 0; n < irLen; ++n)
		{
			rng = rng * 1664525u + 1013904223u;
			const float white = ((float) ((rng >> 8) & 0x00ffffffu) / 8388608.0f) - 1.0f;
			const float fade = juce::jmin (1.0f, juce::jmin ((float) n / 64.0f,
			                                                 (float) (irLen - 1 - n) / 64.0f));
			src[n] = white * fade * 0.16f;
		}
	}

	auto makeAlignmentProbe = [&] (int satType, float satDrive, float satChar, float satTypeCtrl,
	                               float satBias, float satSag, float satDetail, float instabilityAmt,
	                               int seriesCount, bool satRaw, int loaderIndex,
	                               const juce::AudioBuffer<float>& inputProbe) -> juce::AudioBuffer<float>
	{
		juce::AudioBuffer<float> probe (1, irLen);
		probe.copyFrom (0, 0, inputProbe, 0, 0, irLen);

		const auto model = static_cast<SatEngine::Model> (
			juce::jlimit (0, (int) SatEngine::Model::NumModels - 1, satType));

		if (model == SatEngine::Model::NAM)
		{
			const auto path = getNamModelPathForLoader (loaderIndex);
			if (path.isNotEmpty())
			{
				juce::String error;
				auto probeModel = std::make_unique<SatNamModel>();
				if (probeModel->loadFromFile (juce::File (path), currentSampleRate,
				                              juce::jmax (currentBlockSize, irLen), error))
					probeModel->process (probe, irLen);
			}
			return probe;
		}

		if (model == SatEngine::Model::Clean)
			return probe;

		auto probeState = std::make_unique<SatEngine::State>();
		probeState->reset();
		probeState->lastModel = model;
		probeState->sDrive = SatEngine::applyDriveCurve (satDrive, model);
		probeState->sDetailDrive = juce::jlimit (0.0f, 1.0f, satDrive);
		probeState->sGirth = satChar;
		probeState->sMod = satTypeCtrl;
		probeState->sBias = satBias;
		probeState->sReact = satSag;
		probeState->sDetail = juce::jlimit (0.0f, 1.0f, satDetail);
		probeState->sInstability = instabilityAmt;

		const int osOrder = loadRelaxedInt (pOversample);
		const bool effectiveSatRaw = satRaw && model != SatEngine::Model::Clipper;
		const bool deferFullKlonPostEq = model == SatEngine::Model::OverdriveB
			&& ! effectiveSatRaw;
		const bool deferOverdriveAPostEq = model == SatEngine::Model::OverdriveA
			&& ! effectiveSatRaw;
		probeState->deferFullKlonPostEq = deferFullKlonPostEq;
		probeState->deferOverdriveAPostEq = deferOverdriveAPostEq;

		auto* data = probe.getWritePointer (0);
		if (osOrder > 0 && osOrder <= 4)
		{
			// Align must measure the same oversampled core path as audio, but never
			// touch live oversampler states. A local mono oversampler makes the probe
			// include both nonlinear SR effects and OS filter phase/latency.
			juce::dsp::Oversampling<float> probeOs (
				1, (size_t) osOrder,
				juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
			probeOs.initProcessing ((size_t) irLen);
			probeOs.reset();

			auto probeBlock = juce::dsp::AudioBlock<float> (probe);
			auto osBlock = probeOs.processSamplesUp (probeBlock);
			float* osData = osBlock.getChannelPointer (0);
			const int osNumSamples = (int) osBlock.getNumSamples();
			const float osSr = sr * (float) probeOs.getOversamplingFactor();

			SatEngine::processBlock (*probeState, osData, osData, osNumSamples,
			                         model, satDrive, satChar, satTypeCtrl, satBias, satSag, satDetail,
			                         instabilityAmt, osSr, seriesCount, false, effectiveSatRaw, nullptr, 1);
			probeOs.processSamplesDown (probeBlock);
			data = probe.getWritePointer (0);
		}
		else
		{
			SatEngine::processBlock (*probeState, data, data, irLen,
			                         model, satDrive, satChar, satTypeCtrl, satBias, satSag, satDetail,
			                         instabilityAmt, sr, seriesCount, true, effectiveSatRaw, nullptr, 1);
		}

		if (deferOverdriveAPostEq)
			SatEngine::processDeferredOverdriveAPostEq (*probeState, data, data, irLen,
			                                                satDrive, 0.0f, seriesCount, sr, effectiveSatRaw);
		if (deferFullKlonPostEq)
			SatEngine::processDeferredFullKlonPostEq (*probeState, data, data, irLen,
			                                          satDrive, 1.0f, seriesCount, sr, effectiveSatRaw);

		return probe;
	};

	// Estimate effective offset by correlating the processed broadband probe
	// against the shared source. This handles NAM/nonlinear pairings better
	// than an impulse centroid and still returns a single offset value.
	auto calcCentroid = [&] (const juce::AudioBuffer<float>& probe) -> float
	{
		const float* source = alignSource.getReadPointer (0);
		const float* data = probe.getReadPointer (0);
		const int len = probe.getNumSamples();
		const int maxLag = juce::jmin (128, len / 4);

		auto scoreAtLag = [&] (int lag, float* signedScoreOut = nullptr) -> float
		{
			const int srcStart = lag >= 0 ? 0 : -lag;
			const int dstStart = lag >= 0 ? lag : 0;
			const int count = len - std::abs (lag);
			if (count <= 32)
			{
				if (signedScoreOut != nullptr)
					*signedScoreOut = 0.0f;
				return 0.0f;
			}

			double xy = 0.0;
			double xx = 0.0;
			double yy = 0.0;
			for (int i = 0; i < count; ++i)
			{
				const float x = source[srcStart + i];
				const float y = data[dstStart + i];
				xy += (double) x * (double) y;
				xx += (double) x * (double) x;
				yy += (double) y * (double) y;
			}

			const double denom = std::sqrt (xx * yy) + 1.0e-30;
			const float signedScore = (float) (xy / denom);
			if (signedScoreOut != nullptr)
				*signedScoreOut = signedScore;
			return std::abs (signedScore);
		};

		int bestLag = 0;
		float bestScore = -1.0f;
		for (int lag = -maxLag; lag <= maxLag; ++lag)
		{
			const float score = scoreAtLag (lag);
			if (score > bestScore + 1.0e-7f
				|| (std::abs (score - bestScore) <= 1.0e-7f && std::abs (lag) < std::abs (bestLag)))
			{
				bestScore = score;
				bestLag = lag;
			}
		}

		float frac = 0.0f;
		if (bestLag > -maxLag && bestLag < maxLag)
		{
			const float leftScore = scoreAtLag (bestLag - 1);
			const float midScore = scoreAtLag (bestLag);
			const float rightScore = scoreAtLag (bestLag + 1);
			const float denom = leftScore - 2.0f * midScore + rightScore;
			if (std::abs (denom) > 1.0e-9f)
				frac = juce::jlimit (-0.5f, 0.5f, 0.5f * (leftScore - rightScore) / denom);
		}

		return (float) bestLag + frac;
	};

	// Cross-correlation for polarity detection only (sign of correlation)
	auto xcorrSign = [&] (const float* dataA, const juce::AudioBuffer<float>& probeX) -> float
	{
		const float* dataX = probeX.getReadPointer (0);
		const int len = juce::jmin (irLen, probeX.getNumSamples());
		float sum = 0.0f;
		for (int n = 0; n < len; ++n)
			sum += dataA[n] * dataX[n];
		return sum;
	};

	auto applyStaticToneToProbe = [&] (juce::AudioBuffer<float>& probe, int loaderIndex, bool preStage)
	{
		if (probe.getNumSamples() <= 0 || currentSampleRate <= 0.0)
			return;

		auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
			-> std::atomic<float>*
		{
			return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
		};

		const float hpFreq = pick (pHpFreqA, pHpFreqB, pHpFreqC)->load();
		const float lpFreq = pick (pLpFreqA, pLpFreqB, pLpFreqC)->load();
		const bool hpOn = pick (pHpOnA, pHpOnB, pHpOnC)->load() > 0.5f;
		const bool lpOn = pick (pLpOnA, pLpOnB, pLpOnC)->load() > 0.5f;
		const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
			static_cast<int> (std::lround (pick (pHpSlopeA, pHpSlopeB, pHpSlopeC)->load())));
		const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
			static_cast<int> (std::lround (pick (pLpSlopeA, pLpSlopeB, pLpSlopeC)->load())));
		const float tiltDb = pick (pTiltA, pTiltB, pTiltC)->load();
		const int filterPos = static_cast<int> (pick (pFilterPosA, pFilterPosB, pFilterPosC)->load());
		const bool filterPre = (filterPos == 1 || filterPos == 2);
		const bool tiltPre = (filterPos == 1 || filterPos == 3);
		const float sr = static_cast<float> (currentSampleRate);
		const float maxFreq = sr * 0.49f;

		auto applyTilt = [&]()
		{
			if (std::abs (tiltDb) <= 0.05f)
				return;

			float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;
			computeTiltShelfCoeffs (currentSampleRate, tiltDb, b0, b1, a1);
			for (int ch = 0; ch < probe.getNumChannels(); ++ch)
			{
				float state = 0.0f;
				auto* data = probe.getWritePointer (ch);
				for (int n = 0; n < probe.getNumSamples(); ++n)
				{
					const float x = data[n];
					const float y = b0 * x + state;
					state = b1 * x - a1 * y;
					data[n] = y;
				}
			}
		};

		auto applyBiquad = [&] (const BiquadCoefficients& coeffs)
		{
			for (int ch = 0; ch < probe.getNumChannels(); ++ch)
			{
				float z1 = 0.0f;
				float z2 = 0.0f;
				auto* data = probe.getWritePointer (ch);
				for (int n = 0; n < probe.getNumSamples(); ++n)
				{
					const float x = data[n];
					const float y = coeffs.b0 * x + z1;
					z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
					z2 = coeffs.b2 * x - coeffs.a2 * y;
					data[n] = y;
				}
			}
		};

		auto applyFilters = [&]()
		{
			if (hpOn && hpFreq >= 21.0f)
			{
				const float clampedHp = juce::jlimit (20.0f, maxFreq, hpFreq);
				applyBiquad (makeDetectorHighPassForSlope (clampedHp, sr, hpSlope, false));
				if (hpSlope == 2)
					applyBiquad (makeDetectorHighPassForSlope (clampedHp, sr, hpSlope, true));
			}

			if (lpOn && lpFreq <= 19900.0f)
			{
				const float clampedLp = juce::jlimit (20.0f, maxFreq, lpFreq);
				applyBiquad (makeDetectorLowPassForSlope (clampedLp, sr, lpSlope, false));
				if (lpSlope == 2)
					applyBiquad (makeDetectorLowPassForSlope (clampedLp, sr, lpSlope, true));
			}
		};

		if (preStage)
		{
			if (filterPre) applyFilters();
			if (tiltPre) applyTilt();
		}
		else
		{
			if (! tiltPre) applyTilt();
			if (! filterPre) applyFilters();
		}
	};

	auto makeLoaderProbe = [&] (int loaderIndex)
	{
		auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
			-> std::atomic<float>*
		{
			return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
		};

		const int type = static_cast<int> (pick (pSatTypeA, pSatTypeB, pSatTypeC)->load());
		const float drive = pick (pSatDriveA, pSatDriveB, pSatDriveC)->load();
		const float character = pick (pSatCharA, pSatCharB, pSatCharC)->load();
		const float typeCtrl = pick (pSatTypeCtrlA, pSatTypeCtrlB, pSatTypeCtrlC)->load();
		const float bias = pick (pSatBiasA, pSatBiasB, pSatBiasC)->load();
		const float sag = pick (pSatSagA, pSatSagB, pSatSagC)->load();
		const float detail = pick (pDetailA, pDetailB, pDetailC)->load();
		const float instability = pick (pInstabilityA, pInstabilityB, pInstabilityC)->load();
		const float inDb = pick (pInA, pInB, pInC)->load();
		const int series = juce::jlimit (1, 4, static_cast<int> (std::lround (pick (pSeriesA, pSeriesB, pSeriesC)->load())));
		const bool raw = pick (pSatRawA, pSatRawB, pSatRawC)->load() > 0.5f;

		juce::AudioBuffer<float> probeInput (1, irLen);
		probeInput.copyFrom (0, 0, alignSource, 0, 0, irLen);
		probeInput.applyGain (gainFaderDecibelsToGain (inDb));
		applyStaticToneToProbe (probeInput, loaderIndex, true);

		auto probe = makeAlignmentProbe (type, drive, character, typeCtrl, bias, sag, detail,
		                                 instability, series, raw, loaderIndex, probeInput);
		applyStaticToneToProbe (probe, loaderIndex, false);
		return probe;
	};

	juce::AudioBuffer<float> probeA;
	juce::AudioBuffer<float> probeB;
	juce::AudioBuffer<float> probeC;
	float centroidA = 0.0f;
	float centroidB = 0.0f;
	float centroidC = 0.0f;


	if (activeA)
	{
		probeA = makeLoaderProbe (0);
		centroidA = calcCentroid (probeA);
		stateA.dryAnchorSamples.store (centroidA);
	}
	else
		stateA.dryAnchorSamples.store (0.0f);

	if (activeB)
	{
		probeB = makeLoaderProbe (1);
		centroidB = calcCentroid (probeB);
		stateB.dryAnchorSamples.store (centroidB);
	}
	else
		stateB.dryAnchorSamples.store (0.0f);

	if (activeC)
	{
		probeC = makeLoaderProbe (2);
		centroidC = calcCentroid (probeC);
		stateC.dryAnchorSamples.store (centroidC);
	}
	else
		stateC.dryAnchorSamples.store (0.0f);

	if (! activeA || (! activeB && ! activeC))
	{
		dryAlignRuntimeActive_.store (dryAlignRequested && anyEnabled, std::memory_order_release);
		return;
	}

	dryAlignRuntimeActive_.store (dryAlignRequested, std::memory_order_release);
	const float* dataA = probeA.getReadPointer (0);

	const bool currentInvA = parameters.getRawParameterValue (kParamInvA)->load() > 0.5f;

	// Reset all delays first - ALIGN finds timing from scratch.
	if (auto* p = parameters.getParameter (kParamOffsetA))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamOffsetB))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamOffsetC))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamInvA))
		p->setValueNotifyingHost (0.0f);

	float corrSignB = 1.0f;
	float corrSignC = 1.0f;

	if (activeB)
		corrSignB = xcorrSign (dataA, probeB);

	if (activeC)
		corrSignC = xcorrSign (dataA, probeC);

	// Find maximum centroid - loaders with smaller centroids need offset compensation
	const float maxCentroid = juce::jmax (centroidA,
	                                      activeB ? centroidB : centroidA,
	                                      activeC ? centroidC : centroidA);

	// Apply delays: convert centroid difference from samples to ms
	auto setCompDelay = [&] (const char* offsetId, float centroid)
	{
		const float delaySamples = maxCentroid - centroid;
		const float offsetMs = delaySamples / sr * 1000.0f;
		const float clampedMs = juce::jlimit (0.0f, kOffsetMax, offsetMs);
		if (auto* p = parameters.getParameter (offsetId))
			p->setValueNotifyingHost (p->convertTo0to1 (clampedMs));
	};

	setCompDelay (kParamOffsetA, centroidA);
	if (activeB) setCompDelay (kParamOffsetB, centroidB);
	if (activeC) setCompDelay (kParamOffsetC, centroidC);

	auto resolvePolarity = [&] (const char* invId, float corrSign, bool currentInv)
	{
		float rawCorr = corrSign;
		if (currentInvA) rawCorr = -rawCorr;
		if (currentInv)  rawCorr = -rawCorr;

		if (auto* p = parameters.getParameter (invId))
			p->setValueNotifyingHost (rawCorr < 0.0f ? 1.0f : 0.0f);
	};

	// Apply inversion using the audible polarity state before ALIGN, matching CAB-TR.
	if (activeB)
	{
		const bool currentInvB = parameters.getRawParameterValue (kParamInvB)->load() > 0.5f;
		resolvePolarity (kParamInvB, corrSignB, currentInvB);
	}
	if (activeC)
	{
		const bool currentInvC = parameters.getRawParameterValue (kParamInvC)->load() > 0.5f;
		resolvePolarity (kParamInvC, corrSignC, currentInvC);
	}
}

// ----------------------------------------------------------------
bool SATTRAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* SATTRAudioProcessor::createEditor()
{
    return new SATTRAudioProcessorEditor (*this);
}

float SATTRAudioProcessor::getNamSlimForLoader (int loaderIndex) const noexcept
{
	std::atomic<float>* params[] = { pNamSlimA, pNamSlimB, pNamSlimC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return kNamSlimDefault;

	if (params[loaderIndex] != nullptr)
		return juce::jlimit (kNamSlimMin, kNamSlimMax,
		                     params[loaderIndex]->load (std::memory_order_relaxed));

	const char* ids[] = { kParamNamSlimA, kParamNamSlimB, kParamNamSlimC };
	if (auto* p = parameters.getRawParameterValue (ids[loaderIndex]))
		return juce::jlimit (kNamSlimMin, kNamSlimMax,
		                     p->load (std::memory_order_relaxed));

	return kNamSlimDefault;
}

void SATTRAudioProcessor::applyNamSlimForLoader (int loaderIndex, float amount01)
{
	LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return;

	const float slim = juce::jlimit (kNamSlimMin, kNamSlimMax, amount01);
	if (! hasPreparedToPlay)
	{
		lastAppliedNamSlim_[loaderIndex] = slim;
		return;
	}

	juce::String currentPath;
	if (auto nam = loadNamModelAtomic (&states[loaderIndex]->namModel))
		currentPath = nam->getCurrentFilePath();
	else
		currentPath = states[loaderIndex]->pendingNamPath;

	if (currentPath.isEmpty())
	{
		lastAppliedNamSlim_[loaderIndex] = slim;
		return;
	}

	auto replacement = std::make_shared<SatNamModel>();
	replacement->setSlimAmount (slim);

	juce::String error;
	if (! replacement->loadFromFile (juce::File (currentPath), currentSampleRate, currentBlockSize, error))
		return;

	{
		const auto activeNam = loadNamModelAtomic (&states[loaderIndex]->namModel);
		const auto activePath = activeNam != nullptr ? activeNam->getCurrentFilePath()
		                                      : states[loaderIndex]->pendingNamPath;

		if (activePath != currentPath)
			return;

		storeNamModelAtomic (&states[loaderIndex]->namModel, replacement);
		states[loaderIndex]->pendingNamPath = currentPath;
	}

	lastAppliedNamSlim_[loaderIndex] = slim;
}

// ----------------------------------------------------------------
// TIMER CALLBACK: poll lightweight UI actions and maintenance tasks
// ----------------------------------------------------------------
void SATTRAudioProcessor::timerCallback()
{
#if SAT_DSP_DIAG
	SatDiag::getDiagWriter().drain (SatDiag::getDiagRing());
#endif

	const juce::int64 nowMs = juce::Time::currentTimeMillis();
	constexpr juce::int64 namSlimDebounceMs = 140;
	for (int loader = 0; loader < 3; ++loader)
	{
		const float slim = getNamSlimForLoader (loader);
		if (std::abs (slim - pendingNamSlim_[loader]) > 0.0005f)
		{
			pendingNamSlim_[loader] = slim;
			pendingNamSlimChangeTimeMs_[loader] = nowMs;
		}

		if (std::abs (pendingNamSlim_[loader] - lastAppliedNamSlim_[loader]) > 0.0005f
			&& nowMs - pendingNamSlimChangeTimeMs_[loader] >= namSlimDebounceMs)
		{
			applyNamSlimForLoader (loader, pendingNamSlim_[loader]);
		}
	}

	// ALIGN: momentary action - calculate cross-correlation + set delay/inv, then auto-reset
	{
		const float alignVal = parameters.getRawParameterValue (kParamAlign)->load();
		const juce::int64 now = juce::Time::currentTimeMillis();
		constexpr juce::int64 alignCooldownMs = 500;
		if (alignVal > 0.5f && (now - lastAlignTime_ >= alignCooldownMs))
		{
			const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
			const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
			const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;
			const auto alignTypeA = static_cast<SatEngine::Model> (
				juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
				              static_cast<int> (std::lround (pSatTypeA->load()))));
			const auto alignTypeB = static_cast<SatEngine::Model> (
				juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
				              static_cast<int> (std::lround (pSatTypeB->load()))));
			const auto alignTypeC = static_cast<SatEngine::Model> (
				juce::jlimit (0, (int) SatEngine::Model::NumModels - 1,
				              static_cast<int> (std::lround (pSatTypeC->load()))));
			const bool activeA = enabledA && (alignTypeA != SatEngine::Model::NAM || isNamModelLoadedForLoader (0));
			const bool activeB = enabledB && (alignTypeB != SatEngine::Model::NAM || isNamModelLoadedForLoader (1));
			const bool activeC = enabledC && (alignTypeC != SatEngine::Model::NAM || isNamModelLoadedForLoader (2));
			const bool dryAlignRequested = isDryAlignModeEnabled();

			if ((activeA && (activeB || activeC)) || (dryAlignRequested && (activeA || activeB || activeC)))
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

// ----------------------------------------------------------------
// UI state persistence (non-automatable collapse state)
// ----------------------------------------------------------------
void SATTRAudioProcessor::setUiIoExpanded (int loaderIndex, bool expanded)
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
		parameters.state.setProperty (keys[loaderIndex], expanded, nullptr);
}

bool SATTRAudioProcessor::getUiIoExpanded (int loaderIndex) const noexcept
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
	{
		const auto fromState = parameters.state.getProperty (keys[loaderIndex]);
		if (! fromState.isVoid()) return (bool) fromState;
	}
	return false;
}

void SATTRAudioProcessor::setUiFirstVisibleLoaderIndex (int loaderIndex)
{
	parameters.state.setProperty (UiStateKeys::firstVisibleLoader,
	                              juce::jlimit (0, 2, loaderIndex), nullptr);
}

int SATTRAudioProcessor::getUiFirstVisibleLoaderIndex() const noexcept
{
	return juce::jlimit (0, 2, static_cast<int> (parameters.state.getProperty (UiStateKeys::firstVisibleLoader, 0)));
}

void SATTRAudioProcessor::setDryAlignModeEnabled (bool enabled)
{
	dryAlignModeEnabled_.store (enabled, std::memory_order_release);
	if (! enabled)
		dryAlignRuntimeActive_.store (false, std::memory_order_release);
	parameters.state.setProperty (UiStateKeys::dryAlignMode, enabled, nullptr);
}

bool SATTRAudioProcessor::isDryAlignModeEnabled() const noexcept
{
	return dryAlignModeEnabled_.load (std::memory_order_acquire);
}

void SATTRAudioProcessor::setSafeClipModeEnabled (bool enabled)
{
	safeClipModeEnabled_.store (enabled, std::memory_order_release);
	parameters.state.setProperty (UiStateKeys::safeClipMode, enabled, nullptr);
}

bool SATTRAudioProcessor::isSafeClipModeEnabled() const noexcept
{
	return safeClipModeEnabled_.load (std::memory_order_acquire);
}

void SATTRAudioProcessor::toggleSafeClipMode()
{
	setSafeClipModeEnabled (! isSafeClipModeEnabled());
}

bool SATTRAudioProcessor::loadNamModelForLoader (int loaderIndex, const juce::String& filePath, juce::String& error)
{
	LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
	{
		error = "Invalid NAM loader index";
		return false;
	}

	if (! hasPreparedToPlay)
	{
		states[loaderIndex]->pendingNamPath = filePath;
		parameters.state.setProperty (getNamPathPropertyKey (loaderIndex), filePath, nullptr);
		return true;
	}

	auto replacement = std::make_shared<SatNamModel>();
	replacement->setSlimAmount (getNamSlimForLoader (loaderIndex));
	if (! replacement->loadFromFile (juce::File (filePath), currentSampleRate, currentBlockSize, error))
	{
		if (loadNamModelAtomic (&states[loaderIndex]->namModel) == nullptr
			&& states[loaderIndex]->pendingNamPath == filePath)
			states[loaderIndex]->pendingNamPath.clear();
		return false;
	}

	storeNamModelAtomic (&states[loaderIndex]->namModel, replacement);
	states[loaderIndex]->pendingNamPath = filePath;
	parameters.state.setProperty (getNamPathPropertyKey (loaderIndex), filePath, nullptr);
	lastAppliedNamSlim_[loaderIndex] = getNamSlimForLoader (loaderIndex);
	return true;
}

void SATTRAudioProcessor::clearNamModelForLoader (int loaderIndex)
{
	LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return;

	storeNamModelAtomic (&states[loaderIndex]->namModel, {});
	states[loaderIndex]->pendingNamPath.clear();
	parameters.state.setProperty (getNamPathPropertyKey (loaderIndex), juce::String(), nullptr);
}

juce::String SATTRAudioProcessor::getNamModelPathForLoader (int loaderIndex) const
{
	const LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return {};

	const auto pendingPath = states[loaderIndex]->pendingNamPath;
	if (pendingPath.isNotEmpty())
		return pendingPath;

	if (auto nam = loadNamModelAtomic (&states[loaderIndex]->namModel))
	{
		const auto modelPath = nam->getCurrentFilePath();
		if (modelPath.isNotEmpty())
			return modelPath;
	}

	return parameters.state.getProperty (getNamPathPropertyKey (loaderIndex), "").toString();
}

juce::String SATTRAudioProcessor::getNamDisplayNameForLoader (int loaderIndex) const
{
	const LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return {};

	if (auto nam = loadNamModelAtomic (&states[loaderIndex]->namModel))
		return nam->getDisplayName();
	return juce::File (states[loaderIndex]->pendingNamPath).getFileNameWithoutExtension();
}

bool SATTRAudioProcessor::isNamModelLoadedForLoader (int loaderIndex) const
{
	const LoaderState* states[] = { &stateA, &stateB, &stateC };
	if (loaderIndex < 0 || loaderIndex >= 3)
		return false;

	const auto nam = loadNamModelAtomic (&states[loaderIndex]->namModel);
	return nam != nullptr && nam->isLoaded();
}

// ----------------------------------------------------------------
void SATTRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	auto state = parameters.copyState();
	state.setProperty (UiStateKeys::dryAlignAnchorA, stateA.dryAnchorSamples.load(), nullptr);
	state.setProperty (UiStateKeys::dryAlignAnchorB, stateB.dryAnchorSamples.load(), nullptr);
	state.setProperty (UiStateKeys::dryAlignAnchorC, stateC.dryAnchorSamples.load(), nullptr);
	state.setProperty (UiStateKeys::safeClipMode, isSafeClipModeEnabled(), nullptr);
	const juce::String namPaths[] = { getNamModelPathForLoader (0), getNamModelPathForLoader (1), getNamModelPathForLoader (2) };
	state.setProperty (kNamPathA, namPaths[0], nullptr);
	state.setProperty (kNamPathB, namPaths[1], nullptr);
	state.setProperty (kNamPathC, namPaths[2], nullptr);
	auto namState = state.getOrCreateChildWithName (kNamStateNode, nullptr);
	namState.setProperty (kNamPathA, namPaths[0], nullptr);
	namState.setProperty (kNamPathB, namPaths[1], nullptr);
	namState.setProperty (kNamPathC, namPaths[2], nullptr);
	auto xml = state.createXml();
	if (xml != nullptr)
		copyXmlToBinary (*xml, destData);
}

void SATTRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	auto xml = getXmlFromBinary (data, sizeInBytes);
	if (xml != nullptr)
	{
		auto state = juce::ValueTree::fromXml (*xml);
		if (state.isValid())
		{
			parameters.replaceState (state);

			const bool restoredDryAlignMode = (bool) parameters.state.getProperty (UiStateKeys::dryAlignMode, false);
			dryAlignModeEnabled_.store (restoredDryAlignMode, std::memory_order_release);
			dryAlignRuntimeActive_.store (restoredDryAlignMode, std::memory_order_release);
			const bool restoredSafeClipMode = (bool) parameters.state.getProperty (
				UiStateKeys::safeClipMode,
				parameters.state.getProperty (UiStateKeys::legacyTruePeakMode, false));
			safeClipModeEnabled_.store (restoredSafeClipMode, std::memory_order_release);
			currentReportedLatencySamples_ = -1;
			stateA.dryAnchorSamples.store (static_cast<float> ((double) parameters.state.getProperty (UiStateKeys::dryAlignAnchorA, 0.0)));
			stateB.dryAnchorSamples.store (static_cast<float> ((double) parameters.state.getProperty (UiStateKeys::dryAlignAnchorB, 0.0)));
			stateC.dryAnchorSamples.store (static_cast<float> ((double) parameters.state.getProperty (UiStateKeys::dryAlignAnchorC, 0.0)));
			stateA.dryDelayLine.reset();
			stateB.dryDelayLine.reset();
			stateC.dryDelayLine.reset();
			globalDryDelayLine.reset();
			stateA.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
			stateB.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
			stateC.smoothedDryDelay.setCurrentAndTargetValue (0.0f);
			smoothedGlobalDryDelay.setCurrentAndTargetValue (0.0f);

			for (int loader = 0; loader < 3; ++loader)
			{
				sidechainEnv_[loader] = 0.0f;
				sidechainDriveAmount_[loader] = 0.0f;
				sidechainLastGain_[loader] = 1.0f;
				sidechainHpFilters_[loader].reset();
				sidechainHpFilters2_[loader].reset();
				sidechainLpFilters_[loader].reset();
				sidechainLpFilters2_[loader].reset();
				for (int ch = 0; ch < 2; ++ch)
				{
					sidechainDcPrevIn_[loader][ch] = 0.0f;
					sidechainDcPrevOut_[loader][ch] = 0.0f;
				}
			}

			stateA.lastSidechainPreGain = 1.0f;
			stateB.lastSidechainPreGain = 1.0f;
			stateC.lastSidechainPreGain = 1.0f;

			juce::String error;
			const juce::String pathA = readNamPathFromState (state, 0);
			const juce::String pathB = readNamPathFromState (state, 1);
			const juce::String pathC = readNamPathFromState (state, 2);
			if (pathA.isNotEmpty()) loadNamModelForLoader (0, pathA, error); else clearNamModelForLoader (0);
			if (pathB.isNotEmpty()) loadNamModelForLoader (1, pathB, error); else clearNamModelForLoader (1);
			if (pathC.isNotEmpty()) loadNamModelForLoader (2, pathC, error); else clearNamModelForLoader (2);
		}
	}
}

// ----------------------------------------------------------------
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SATTRAudioProcessor();
}

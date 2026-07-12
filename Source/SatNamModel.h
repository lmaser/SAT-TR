#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>

namespace nam
{
class DSP;
}

#if defined (NAM_SAMPLE_FLOAT)
using SatNamSample = float;
#else
using SatNamSample = double;
#endif

class SatNamModel
{
public:
	SatNamModel();
	~SatNamModel();

	bool loadFromFile (const juce::File& file,
	                   double hostSampleRate,
	                   int maxBlockSize,
	                   juce::String& error);
	void clear();
	void reset (double hostSampleRate, int maxBlockSize);
	void setSlimAmount (float amount01);
	void process (juce::AudioBuffer<float>& buffer, int numSamples);
	void processMonoToStereo (juce::AudioBuffer<float>& buffer, int numSamples);

	bool isLoaded() const noexcept { return loaded_; }
	float getSlimAmount() const noexcept { return slimAmount_; }
	const juce::String& getCurrentFilePath() const noexcept { return currentFilePath_; }
	const juce::String& getDisplayName() const noexcept { return displayName_; }
	double getExpectedSampleRate() const noexcept { return expectedSampleRate_; }

private:
	std::array<std::unique_ptr<nam::DSP>, 2> channelModels_;
	juce::AudioBuffer<SatNamSample> inputScratch_;
	juce::AudioBuffer<SatNamSample> outputScratch_;
	std::array<SatNamSample*, 2> inputPtrs_ {};
	std::array<SatNamSample*, 2> outputPtrs_ {};
	juce::String currentFilePath_;
	juce::String displayName_;
	double hostSampleRate_ = 44100.0;
	double expectedSampleRate_ = -1.0;
	int maxBlockSize_ = 512;
	float slimAmount_ = 0.0f;
	bool loaded_ = false;
};

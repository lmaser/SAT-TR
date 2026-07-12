#include "SatNamModel.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <mutex>

#if defined (_MSC_VER)
 #pragma warning (push)
 #pragma warning (disable: 4100 4189 4244 4267 4305 4458)
#endif
#include "NAM/container.h"
#include "NAM/convnet.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/lstm.h"
#include "NAM/model_config.h"
#include "NAM/slimmable.h"
#include "NAM/wavenet/model.h"
#if defined (_MSC_VER)
 #pragma warning (pop)
#endif

namespace
{
	void registerNamParserIfMissing (const char* name, nam::ConfigParserFunction parser)
	{
		auto& registry = nam::ConfigParserRegistry::instance();
		if (! registry.has (name))
			registry.registerParser (name, std::move (parser));
	}

	void ensureNamCoreParsersRegistered()
	{
		static std::once_flag flag;
		std::call_once (flag, []()
		{
			registerNamParserIfMissing ("Linear", nam::linear::create_config);
			registerNamParserIfMissing ("ConvNet", nam::convnet::create_config);
			registerNamParserIfMissing ("LSTM", nam::lstm::create_config);
			registerNamParserIfMissing ("WaveNet", nam::wavenet::create_config);
			registerNamParserIfMissing ("SlimmableContainer", nam::container::create_config);

			// Some exported models use legacy/variant architecture spellings for
			// the same container schema. Keep them routed to the v0.5.x container.
			registerNamParserIfMissing ("SlimableContainer", nam::container::create_config);
			registerNamParserIfMissing ("Slimmable container", nam::container::create_config);
			registerNamParserIfMissing ("Slimable container", nam::container::create_config);
		});
	}

	void applyNamSlimAmount (nam::DSP* model, float amount01)
	{
		if (auto* slimmable = dynamic_cast<nam::SlimmableModel*> (model))
		{
			const float size = juce::jlimit (0.0f, 1.0f, amount01);
			slimmable->SetSlimmableSize (static_cast<double> (size));
		}
	}
}

SatNamModel::SatNamModel() = default;
SatNamModel::~SatNamModel() = default;

bool SatNamModel::loadFromFile (const juce::File& file,
                                double hostSampleRate,
                                int maxBlockSize,
                                juce::String& error)
{
	clear();

	if (! file.existsAsFile())
	{
		error = "NAM file not found";
		return false;
	}

	try
	{
		ensureNamCoreParsersRegistered();

		const auto path = std::filesystem::path (file.getFullPathName().toWideCharPointer());
		auto left = nam::get_dsp (path);
		auto right = nam::get_dsp (path);

		if (left == nullptr || right == nullptr)
		{
			error = "NAM model could not be created";
			return false;
		}

		applyNamSlimAmount (left.get(), slimAmount_);
		applyNamSlimAmount (right.get(), slimAmount_);

		if (left->NumInputChannels() != 1 || left->NumOutputChannels() != 1)
		{
			error = "Only mono NAM models are supported in this SAT-TR build";
			clear();
			return false;
		}

		expectedSampleRate_ = left->GetExpectedSampleRate();
		if (expectedSampleRate_ > 0.0 && hostSampleRate > 0.0
			&& std::abs (expectedSampleRate_ - hostSampleRate) > 1.0)
		{
			error = "NAM sample-rate mismatch: model "
			      + juce::String (expectedSampleRate_, 0)
			      + " Hz, host " + juce::String (hostSampleRate, 0) + " Hz";
			clear();
			return false;
		}

		channelModels_[0] = std::move (left);
		channelModels_[1] = std::move (right);
		reset (hostSampleRate, maxBlockSize);

		currentFilePath_ = file.getFullPathName();
		displayName_ = file.getFileNameWithoutExtension();
		loaded_ = true;
		error.clear();
		return true;
	}
	catch (const std::exception& e)
	{
		error = e.what();
		clear();
		return false;
	}
	catch (...)
	{
		error = "Unknown NAM load error";
		clear();
		return false;
	}
}

void SatNamModel::clear()
{
	channelModels_[0].reset();
	channelModels_[1].reset();
	inputScratch_.setSize (0, 0);
	currentFilePath_.clear();
	displayName_.clear();
	expectedSampleRate_ = -1.0;
	loaded_ = false;
}

void SatNamModel::reset (double hostSampleRate, int maxBlockSize)
{
	hostSampleRate_ = hostSampleRate > 0.0 ? hostSampleRate : hostSampleRate_;
	const int safeBlock = juce::jmax (1, maxBlockSize);
	maxBlockSize_ = safeBlock;
	for (auto& model : channelModels_)
		if (model != nullptr)
			model->Reset (hostSampleRate_, safeBlock);

	inputScratch_.setSize (2, safeBlock, false, false, true);
	outputScratch_.setSize (2, safeBlock, false, false, true);
}

void SatNamModel::setSlimAmount (float amount01)
{
	slimAmount_ = juce::jlimit (0.0f, 1.0f, amount01);
	for (auto& model : channelModels_)
		if (model != nullptr)
			applyNamSlimAmount (model.get(), slimAmount_);
}

void SatNamModel::process (juce::AudioBuffer<float>& buffer, int numSamples)
{
	if (! loaded_ || channelModels_[0] == nullptr || numSamples <= 0)
		return;

	const int chunkSamples = juce::jmin (inputScratch_.getNumSamples(), maxBlockSize_);
	if (chunkSamples <= 0)
		return;

	const int channels = juce::jmin (2, buffer.getNumChannels());
	for (int ch = 0; ch < channels; ++ch)
	{
		auto& model = channelModels_[ch == 0 ? 0 : 1];
		if (model == nullptr)
			continue;

		for (int offset = 0; offset < numSamples; offset += chunkSamples)
		{
			const int chunk = juce::jmin (chunkSamples, numSamples - offset);
			const auto* in = buffer.getReadPointer (ch, offset);
			auto* scratchIn = inputScratch_.getWritePointer (ch);
			auto* scratchOut = outputScratch_.getWritePointer (ch);
			std::copy (in, in + chunk, scratchIn);

			inputPtrs_[0] = inputScratch_.getWritePointer (ch);
			outputPtrs_[0] = scratchOut;
			model->process (inputPtrs_.data(), outputPtrs_.data(), chunk);

			auto* out = buffer.getWritePointer (ch, offset);
			std::copy (scratchOut, scratchOut + chunk, out);
		}
	}

	if (buffer.getNumChannels() > 1 && channels == 1)
		buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);
}

void SatNamModel::processMonoToStereo (juce::AudioBuffer<float>& buffer, int numSamples)
{
	if (! loaded_ || channelModels_[0] == nullptr || numSamples <= 0 || buffer.getNumChannels() <= 0)
		return;

	const int chunkSamples = juce::jmin (inputScratch_.getNumSamples(), maxBlockSize_);
	if (chunkSamples <= 0)
		return;

	for (int offset = 0; offset < numSamples; offset += chunkSamples)
	{
		const int chunk = juce::jmin (chunkSamples, numSamples - offset);
		const auto* in = buffer.getReadPointer (0, offset);
		auto* scratchIn = inputScratch_.getWritePointer (0);
		auto* scratchOut = outputScratch_.getWritePointer (0);
		std::copy (in, in + chunk, scratchIn);

		inputPtrs_[0] = inputScratch_.getWritePointer (0);
		outputPtrs_[0] = scratchOut;
		channelModels_[0]->process (inputPtrs_.data(), outputPtrs_.data(), chunk);

		auto* out = buffer.getWritePointer (0, offset);
		std::copy (scratchOut, scratchOut + chunk, out);
	}

	if (buffer.getNumChannels() > 1)
		buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);
}

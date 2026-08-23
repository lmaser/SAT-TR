#include "../Source/SatNamModel.h"

#include <JuceHeader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require (bool condition, const std::string& message)
{
    if (! condition)
        throw std::runtime_error (message);
}

struct TimingResult
{
    int blockSize = 0;
    double elapsedMs = 0.0;
    double audioMs = 0.0;
    double realtimePercent = 0.0;
    bool finite = true;
};

TimingResult measure (const juce::File& modelFile, int blockSize, double sampleRate)
{
    SatNamModel model;
    juce::String error;
    require (model.loadFromFile (modelFile, sampleRate, blockSize, error), error.toStdString());

    juce::AudioBuffer<float> audio (1, blockSize);
    constexpr int warmupBlocks = 32;
    constexpr int measuredBlocks = 2000;

    for (int block = 0; block < warmupBlocks; ++block)
    {
        auto* data = audio.getWritePointer (0);
        for (int sample = 0; sample < blockSize; ++sample)
            data[sample] = 0.1f * std::sin (0.013f * static_cast<float> (block * blockSize + sample));
        model.process (audio, blockSize);
    }

    const auto start = std::chrono::steady_clock::now();
    bool finite = true;
    for (int block = 0; block < measuredBlocks; ++block)
    {
        auto* data = audio.getWritePointer (0);
        for (int sample = 0; sample < blockSize; ++sample)
            data[sample] = 0.1f * std::sin (0.013f * static_cast<float> (block * blockSize + sample));
        model.process (audio, blockSize);
        for (int sample = 0; sample < blockSize; ++sample)
            finite = finite && std::isfinite (audio.getSample (0, sample));
    }
    const auto elapsed = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    const double audioMs = 1000.0 * measuredBlocks * blockSize / sampleRate;
    return { blockSize, elapsed, audioMs, 100.0 * elapsed / audioMs, finite };
}

int measureImpulseLatency (const juce::File& modelFile, double sampleRate)
{
    SatNamModel model;
    juce::String error;
    constexpr int blockSize = 128;
    require (model.loadFromFile (modelFile, sampleRate, blockSize, error), error.toStdString());

    juce::AudioBuffer<float> audio (1, blockSize);
    int firstNonZero = -1;
    for (int block = 0; block < 64; ++block)
    {
        audio.clear();
        if (block == 0)
            audio.setSample (0, 0, 1.0f);
        model.process (audio, blockSize);
        for (int sample = 0; sample < blockSize; ++sample)
            if (std::abs (audio.getSample (0, sample)) > 1.0e-7f)
            {
                firstNonZero = block * blockSize + sample;
                return firstNonZero;
            }
    }
    return firstNonZero;
}
}

int main (int argc, char** argv)
{
    try
    {
        require (argc >= 2, "Usage: SatNamRealtimeProbe <model.nam>");
        const juce::File modelFile (argv[1]);
        require (modelFile.existsAsFile(), "NAM model file does not exist");

        constexpr double sampleRate = 48000.0;
        std::cout << "model=" << modelFile.getFullPathName() << "\n";
        for (const int blockSize : { 32, 64, 128, 256, 512 })
        {
            const auto result = measure (modelFile, blockSize, sampleRate);
            std::cout << "block=" << result.blockSize
                      << " elapsed_ms=" << result.elapsedMs
                      << " audio_ms=" << result.audioMs
                      << " realtime_percent=" << result.realtimePercent
                      << " finite=" << (result.finite ? "true" : "false") << "\n";
            require (result.finite, "NAM produced non-finite audio");
        }

        SatNamModel metadataModel;
        juce::String metadataError;
        require (metadataModel.loadFromFile (modelFile, sampleRate, 128, metadataError), metadataError.toStdString());
        std::cout << "expected_sample_rate=" << metadataModel.getExpectedSampleRate() << "\n";
        std::cout << "impulse_first_nonzero_sample=" << measureImpulseLatency (modelFile, sampleRate) << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

// ==================================================================================
// ThreadedConvolver.h — Stereo convolver with background tail processing
// Uses FFTConvolver (HiFi-LoFi, MIT) with tail partitions on a worker thread.
// ==================================================================================

#pragma once

#include "../ThirdParty/FFTConvolver/TwoStageFFTConvolver.h"
#include "DspDebugLog.h"
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

// ==================================================================================
// MonoThreadedConvolver — single-channel TwoStageFFTConvolver with a background
// thread for the big tail partitions.
// ==================================================================================
class MonoThreadedConvolver : public fftconvolver::TwoStageFFTConvolver
{
public:
    MonoThreadedConvolver()
    {
        _workerThread = std::thread([this] { workerLoop(); });
    }

    ~MonoThreadedConvolver() override
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _shutdown = true;
        }
        _cv.notify_one();
        if (_workerThread.joinable())
            _workerThread.join();
    }

protected:
    void startBackgroundProcessing() override
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _workReady = true;
        }
        _cv.notify_one();
    }

    void waitForBackgroundProcessing() override
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this] { return !_workReady; });
    }

private:
    void workerLoop()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return _workReady || _shutdown; });

            if (_shutdown)
                return;

            // Do the heavy tail convolution on this background thread
            doBackgroundProcessing();

            _workReady = false;
            lock.unlock();
            _cv.notify_one();
        }
    }

    std::thread _workerThread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _workReady = false;
    bool _shutdown = false;
};


// ==================================================================================
// StereoThreadedConvolver — wraps two MonoThreadedConvolvers (L+R).
// Drop-in replacement for juce::dsp::Convolution in our IR loader pipeline.
//
// Usage:
//   convolver.prepare (sampleRate, blockSize);
//   convolver.loadIR (irBuffer, irSampleRate);
//   convolver.process (audioBuffer);  // in processBlock
//   convolver.reset();
// ==================================================================================
class StereoThreadedConvolver
{
public:
    StereoThreadedConvolver() = default;

    void prepare (double sampleRate, int blockSize)
    {
        _blockSize = blockSize;
        _sampleRate = sampleRate;
        // Generous pre-allocation for hosts with variable buffer sizes (e.g. FL Studio)
        const int allocSize = std::max (blockSize, 8192);
        _scratch.setSize(2, allocSize);
        _crossScratch.setSize(2, allocSize);
        _prepared = true;
    }

    // Load an impulse response (may be mono or stereo).
    // Called from the message thread. The swap is protected by _swapMutex;
    // old engines are kept alive for crossfading, then destroyed when done.
    void loadIR (const juce::AudioBuffer<float>& ir, double /*irSampleRate*/)
    {
        if (!_prepared || ir.getNumSamples() == 0)
            return;

        const int numSamples = ir.getNumSamples();
        const int numChannels = ir.getNumChannels();

        // Head block = host block size (low latency). Tail block = 4096 (efficiency).
        const size_t headBlock = static_cast<size_t>(_blockSize);
        const size_t tailBlock = 4096;

        auto newL = std::make_unique<MonoThreadedConvolver>();
        auto newR = std::make_unique<MonoThreadedConvolver>();

        const float* irL = ir.getReadPointer(0);
        const float* irR = (numChannels >= 2) ? ir.getReadPointer(1) : irL;

        newL->init(headBlock, tailBlock, irL, static_cast<size_t>(numSamples));
        newR->init(headBlock, tailBlock, irR, static_cast<size_t>(numSamples));

#if CABTR_DSP_DEBUG_LOG
        // ── Convolver verification: unit-impulse test ──
        // Create a throwaway convolver, feed it a unit impulse, and check
        // that the first output sample matches ir[0].  Any FFT scaling bug
        // (e.g. missing 1/N from FFTW) will show up here immediately.
        {
            MonoThreadedConvolver testConv;
            testConv.init(headBlock, tailBlock, irL, static_cast<size_t>(numSamples));

            // Feed one block of zeros first (to prime the convolver)
            const size_t testLen = static_cast<size_t>(_blockSize);
            std::vector<float> testIn(testLen, 0.0f);
            std::vector<float> testOut(testLen, 0.0f);
            testConv.process(testIn.data(), testOut.data(), testLen);

            // Now feed a unit impulse at sample 0
            testIn[0] = 1.0f;
            std::fill(testIn.begin() + 1, testIn.end(), 0.0f);
            std::fill(testOut.begin(), testOut.end(), 0.0f);
            testConv.process(testIn.data(), testOut.data(), testLen);

            // The output of conv(impulse, ir) should be ir[0], ir[1], ...
            // Compare first few samples
            float maxErr = 0.0f;
            float outPeak = 0.0f;
            juce::String detail;
            const int checkLen = std::min(8, numSamples);
            for (int i = 0; i < checkLen; ++i)
            {
                float expected = irL[i];
                float got = testOut[i];
                float err = std::abs(got - expected);
                maxErr = std::max(maxErr, err);
                outPeak = std::max(outPeak, std::abs(got));
                detail << "  [" << i << "] expected=" << juce::String(expected, 6)
                       << " got=" << juce::String(got, 6)
                       << " err=" << juce::String(err, 6) << "\n";
            }
            // Also check peak of entire output block
            float blockPeak = 0.0f;
            for (size_t i = 0; i < testLen; ++i)
                blockPeak = std::max(blockPeak, std::abs(testOut[i]));

            juce::String msg;
            msg << "CONVOLVER VERIFY: headBlock=" << (int)headBlock
                << " tailBlock=" << (int)tailBlock
                << " irLen=" << numSamples
                << " ir[0]=" << juce::String(irL[0], 6)
                << " out[0]=" << juce::String(testOut[0], 6)
                << " maxErr(0.." << checkLen << ")=" << juce::String(maxErr, 6)
                << " blockPeak=" << juce::String(blockPeak, 6)
                << " ratio=" << juce::String(blockPeak / std::max(0.00001f, std::abs(irL[0])), 2)
                << "\n" << detail;
            LOG_IR_EVENT(msg);
        }
#endif

        // Swap under lock — old convolvers kept for crossfade
        std::unique_ptr<MonoThreadedConvolver> discardL, discardR;
        {
            std::lock_guard<std::mutex> lock(_swapMutex);
            // If a previous crossfade is still in progress, discard those old convolvers
            discardL = std::move(_oldConvolverL);
            discardR = std::move(_oldConvolverR);
            // Move current → old for crossfade
            _oldConvolverL = std::move(_convolverL);
            _oldConvolverR = std::move(_convolverR);
            _convolverL = std::move(newL);
            _convolverR = std::move(newR);
            _irLoaded.store(true);
            // ~50ms crossfade
            _crossfadeTotal = static_cast<int>(_sampleRate * 0.05);
            _crossfadeRemaining = _crossfadeTotal;
        }
        // discardL / discardR destroyed here — outside lock
    }

    // Process a stereo buffer in-place. Audio-thread safe.
    // Lock is held for the entire duration so the convolvers cannot be
    // destroyed from under us (eliminates use-after-free).
    void process (juce::AudioBuffer<float>& buffer)
    {
        if (!_irLoaded.load())
            return;

        std::lock_guard<std::mutex> lock(_swapMutex);

        if (!_convolverL || !_convolverR)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        // Guard: some hosts (e.g. FL Studio) may send blocks larger
        // than the blockSize passed to prepare(). Resize scratch buffers.
        if (numSamples > _scratch.getNumSamples())
        {
            _scratch.setSize (2, numSamples);
            _crossScratch.setSize (2, numSamples);
        }

        // Copy raw input to scratch buffer BEFORE convolution.
        for (int ch = 0; ch < numChannels; ++ch)
            _scratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Process through new (current) convolvers
        _convolverL->process(_scratch.getReadPointer(0),
                             buffer.getWritePointer(0),
                             static_cast<size_t>(numSamples));

        if (numChannels >= 2)
        {
            _convolverR->process(_scratch.getReadPointer(1),
                                 buffer.getWritePointer(1),
                                 static_cast<size_t>(numSamples));
        }

        // Crossfade from old → new convolver output
        if (_crossfadeRemaining > 0 && _oldConvolverL && _oldConvolverR)
        {
            // Process same input through OLD convolvers
            _oldConvolverL->process(_scratch.getReadPointer(0),
                                    _crossScratch.getWritePointer(0),
                                    static_cast<size_t>(numSamples));
            if (numChannels >= 2)
            {
                _oldConvolverR->process(_scratch.getReadPointer(1),
                                        _crossScratch.getWritePointer(1),
                                        static_cast<size_t>(numSamples));
            }

            // Blend: old * (1-t) + new * t, with S-curve
            // Counter decremented once per sample (not per channel)
            for (int i = 0; i < numSamples && _crossfadeRemaining > 0; ++i)
            {
                const float t = 1.0f - static_cast<float>(_crossfadeRemaining)
                                        / static_cast<float>(_crossfadeTotal);
                const float wet = t * t * (3.0f - 2.0f * t); // S-curve
                --_crossfadeRemaining;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* newOut = buffer.getWritePointer(ch);
                    const auto* oldOut = _crossScratch.getReadPointer(ch);
                    newOut[i] = oldOut[i] + (newOut[i] - oldOut[i]) * wet;
                }
            }

            // Crossfade done — release old convolvers
            if (_crossfadeRemaining <= 0)
            {
                _oldConvolverL.reset();
                _oldConvolverR.reset();
            }
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(_swapMutex);
        _convolverL.reset();
        _convolverR.reset();
        _oldConvolverL.reset();
        _oldConvolverR.reset();
        _irLoaded.store(false);
        _crossfadeRemaining = 0;
    }

private:
    int _blockSize = 64;
    double _sampleRate = 48000.0;
    bool _prepared = false;
    std::atomic<bool> _irLoaded { false };
    std::mutex _swapMutex;
    std::unique_ptr<MonoThreadedConvolver> _convolverL;
    std::unique_ptr<MonoThreadedConvolver> _convolverR;
    std::unique_ptr<MonoThreadedConvolver> _oldConvolverL;
    std::unique_ptr<MonoThreadedConvolver> _oldConvolverR;
    juce::AudioBuffer<float> _scratch;       // raw-input copy to avoid in-place feedback
    juce::AudioBuffer<float> _crossScratch;  // old convolver output for crossfade
    int _crossfadeTotal = 0;
    int _crossfadeRemaining = 0;
};

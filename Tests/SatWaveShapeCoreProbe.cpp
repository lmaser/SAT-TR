#include <JuceHeader.h>

#include "../Source/WaveShape/SatWaveShapeCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

bool near(float actual, float expected, float tolerance = 2.0e-5f) noexcept
{
    return std::abs(actual - expected) <= tolerance;
}

TR::Curves::Curve makeCurve(float exponent, float curvature = 0.0f)
{
    TR::Curves::Curve curve;
    constexpr int points = 33;
    curve.points.reserve(points);
    for (int index = 0; index < points; ++index)
    {
        const auto x = static_cast<float>(index) / static_cast<float>(points - 1);
        curve.points.push_back({ x, std::pow(x, exponent),
                                 index + 1 < points ? curvature : 0.0f });
    }
    return curve;
}

void processMono(SATTR::WaveShape::Core& core, std::vector<float>& samples)
{
    float* channels[] = { samples.data() };
    core.process(channels, 1, static_cast<int>(samples.size()));
}

void settleCurveCrossfade(SATTR::WaveShape::Core& core)
{
    std::vector<float> silence(static_cast<std::size_t>(core.curveCrossfadeSamples() + 8), 0.0f);
    processMono(core, silence);
    require(! core.isCurveCrossfading(), "curve publication did not finish its crossfade");
}

void verifyCompiler()
{
    SATTR::WaveShape::CompiledCurve identity;
    SATTR::WaveShape::CompileReport report;
    require(SATTR::WaveShape::compileCurve(
                TR::Curves::makeIdentity(TR::Curves::Domain::unipolar),
                TR::Curves::Domain::unipolar, identity, &report, "identity")
                && report.ok(),
            "identity curve did not compile");
    require(identity.identity, "identity curve flag was not detected");
    for (int sample = 0; sample <= 1000; ++sample)
    {
        const auto x = -1.0f + 2.0f * static_cast<float>(sample) / 1000.0f;
        require(near(identity.evaluate(x), x, 1.0e-6f), "identity LUT is inaccurate");
        require(near(identity.evaluateAntiderivative(x), 0.5f * x * x, 2.0e-6f),
                "identity antiderivative is inaccurate");
    }
    require(near(identity.evaluate(-4.0f), -1.0f)
                && near(identity.evaluate(4.0f), 1.0f),
            "curve overflow is not HOLD/clamp");
    require(near(identity.evaluateAntiderivative(2.0f), 1.5f)
                && near(identity.evaluateAntiderivative(-2.0f), 1.5f),
            "antiderivative overflow does not extend HOLD linearly");

    SATTR::WaveShape::CompiledCurve shaped;
    const auto source = makeCurve(2.4f, 0.45f);
    require(SATTR::WaveShape::compileCurve(source, TR::Curves::Domain::unipolar,
                                           shaped, &report, "shaped"),
            "shaped curve did not compile");
    for (int sample = 0; sample <= 1000; ++sample)
    {
        const auto x = static_cast<float>(sample) / 1000.0f;
        require(near(shaped.evaluate(-x), -shaped.evaluate(x), 3.0e-6f),
                "compiled unipolar curve is not an exact X/Y mirror");
    }

    auto invalid = source;
    invalid.points[2].x = invalid.points[1].x;
    require(! SATTR::WaveShape::compileCurve(invalid, TR::Curves::Domain::unipolar,
                                              shaped, &report, "invalid")
                && ! report.ok(),
            "invalid source curve compiled successfully");
}

void verifyAdaaAndControls()
{
    SATTR::WaveShape::Core core;
    core.prepare(48000.0, 512, 1);
    core.reset(0.0f, 0.0f);
    require(near(SATTR::WaveShape::Core::intrinsicDelaySamples(), 0.5f, 0.0f),
            "ADAA group delay contract changed");

    std::vector<float> identityInput { 0.2f, 0.6f, -0.4f, -0.4f };
    const auto original = identityInput;
    processMono(core, identityInput);
    float previous = 0.0f;
    for (std::size_t index = 0; index < identityInput.size(); ++index)
    {
        require(near(identityInput[index], 0.5f * (original[index] + previous), 2.0e-6f),
                "ADAA identity response is not the documented half-sample average");
        previous = original[index];
    }

    SATTR::WaveShape::LoaderState loader;
    loader.slots[0].unipolar.points = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    loader.slots[0].bipolar = TR::Curves::mirrorXY(loader.slots[0].unipolar);
    loader.slots[1].unipolar = TR::Curves::makeIdentity(TR::Curves::Domain::unipolar);
    loader.slots[1].bipolar = TR::Curves::mirrorXY(loader.slots[1].unipolar);
    const auto published = core.compileAndPublish(loader);
    require(published.published && published.report.ok(), "A/B curve bank was not published");
    settleCurveCrossfade(core);

    for (const auto morph : { 0.0f, 0.25f, 1.0f })
    {
        core.reset(morph, 0.0f);
        std::vector<float> samples { 0.8f, 0.8f };
        processMono(core, samples);
        require(near(samples[1], morph * 0.8f, 3.0e-5f),
                "A/B morph is not a direct output-domain interpolation");
    }

    loader.slots[0].unipolar = makeCurve(2.0f);
    loader.slots[0].bipolar = TR::Curves::mirrorXY(loader.slots[0].unipolar);
    loader.slots[1] = loader.slots[0];
    require(core.compileAndPublish(loader).published, "biased curve bank was not published");
    settleCurveCrossfade(core);
    core.reset(0.0f, 1.0f);
    std::vector<float> silence(64, 0.0f);
    processMono(core, silence);
    for (const auto sample : silence)
        require(near(sample, 0.0f, 2.0e-6f), "Bias does not preserve digital silence");
    std::vector<float> steady { 0.25f, 0.25f };
    processMono(core, steady);
    require(near(steady[1], 0.75f * 0.75f - 0.5f * 0.5f, 1.0e-3f),
            "Bias does not implement f(x+b)-f(b)");

    core.reset(0.0f, 0.0f);
    core.setControlTargets(1.0f, -1.0f);
    std::vector<float> modulation(2048, 0.3f);
    processMono(core, modulation);
    for (const auto sample : modulation)
        require(std::isfinite(sample), "smoothed control automation produced non-finite audio");
}

void verifyIndependentSeriesStages()
{
    SATTR::WaveShape::Core core;
    core.prepare(48000.0, 16, 1);
    for (const int stages : { 1, 2, 4 })
    {
        core.reset();
        std::vector<float> impulse(8, 0.0f);
        impulse[0] = 1.0f;
        float* channels[] = { impulse.data() };
        core.process(channels, 1, static_cast<int>(impulse.size()), stages);
        const int coefficients[][5] = {
            { 1, 1, 0, 0, 0 }, { 1, 2, 1, 0, 0 }, { 1, 4, 6, 4, 1 }
        };
        const int row = stages == 1 ? 0 : (stages == 2 ? 1 : 2);
        const float denominator = static_cast<float>(1 << stages);
        for (int sample = 0; sample <= stages; ++sample)
            require(near(impulse[static_cast<std::size_t>(sample)],
                         static_cast<float>(coefficients[row][sample]) / denominator, 2.0e-6f),
                    "SERIES stages share or corrupt ADAA history");
    }

    std::array<float, 17> snapshot {};
    require(core.makeTransferSnapshot(0.0f, 0.0f, snapshot.data(),
                                      static_cast<int>(snapshot.size()), -1.0f, 1.0f),
            "transfer snapshot was unavailable");
    for (int index = 0; index < static_cast<int>(snapshot.size()); ++index)
        require(near(snapshot[static_cast<std::size_t>(index)],
                     -1.0f + 2.0f * static_cast<float>(index)
                             / static_cast<float>(snapshot.size() - 1), 2.0e-6f),
                "identity transfer snapshot is inaccurate");
}

void verifyPublicationAndCrossfade()
{
    SATTR::WaveShape::Core core;
    core.prepare(48000.0, 512, 1);
    core.reset();
    std::vector<float> warmup { 0.5f, 0.5f };
    processMono(core, warmup);

    auto zero = SATTR::WaveShape::makeDefaultState().loaders[0];
    for (auto& slot : zero.slots)
    {
        slot.unipolar.points = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
        slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
    }
    require(core.compileAndPublish(zero).published, "zero curve was not published");
    std::vector<float> transition(static_cast<std::size_t>(core.curveCrossfadeSamples()), 0.5f);
    processMono(core, transition);
    require(transition.front() < 0.5f && transition.front() > 0.49f,
            "curve crossfade did not begin continuously from the old bank");
    require(near(transition.back(), 0.0f, 2.0e-5f),
            "curve crossfade did not reach the new bank");
    for (std::size_t index = 1; index < transition.size(); ++index)
        require(transition[index] <= transition[index - 1] + 2.0e-6f,
                "curve crossfade is not monotonic for a monotonic endpoint change");

    auto identity = SATTR::WaveShape::makeDefaultState().loaders[0];
    require(core.compileAndPublish(identity).published, "identity update was not published");
    std::vector<float> oneSample { 0.5f };
    processMono(core, oneSample);
    auto shaped = identity;
    shaped.slots[0].unipolar = makeCurve(2.0f);
    shaped.slots[0].bipolar = TR::Curves::mirrorXY(shaped.slots[0].unipolar);
    require(core.compileAndPublish(shaped).published, "pending update was not queued");
    const auto busy = core.compileAndPublish(zero);
    require(! busy.published && busy.busy, "publisher overwrote a bank held by audio");
    settleCurveCrossfade(core);
    std::vector<float> nextBlock { 0.0f };
    processMono(core, nextBlock);
    require(core.isCurveCrossfading(), "queued curve update was not adopted after crossfade");
    settleCurveCrossfade(core);
}

void verifyConcurrentPublication()
{
    SATTR::WaveShape::Core core;
    core.prepare(48000.0, 64, 2);
    core.reset();
    std::atomic<bool> finished { false };
    std::atomic<bool> failed { false };
    std::thread producer([&]
    {
        for (int iteration = 0; iteration < 120; ++iteration)
        {
            auto loader = SATTR::WaveShape::makeDefaultState().loaders[0];
            const auto exponent = 0.55f + 0.015f * static_cast<float>(iteration % 60);
            for (auto& slot : loader.slots)
            {
                slot.unipolar = makeCurve(exponent, (iteration & 1) != 0 ? 0.4f : -0.35f);
                slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
            }
            const auto result = core.compileAndPublish(loader);
            if (! result.report.ok()) failed.store(true, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });

    std::array<float, 64> left {};
    std::array<float, 64> right {};
    float* channels[] = { left.data(), right.data() };
    int block = 0;
    while (! finished.load(std::memory_order_acquire) || block < 200)
    {
        for (int sample = 0; sample < 64; ++sample)
        {
            left[static_cast<std::size_t>(sample)] = 0.9f * std::sin(0.013f * static_cast<float>(block * 64 + sample));
            right[static_cast<std::size_t>(sample)] = 0.8f * std::cos(0.017f * static_cast<float>(block * 64 + sample));
        }
        core.process(channels, 2, 64);
        for (int sample = 0; sample < 64; ++sample)
            if (! std::isfinite(left[static_cast<std::size_t>(sample)])
                || ! std::isfinite(right[static_cast<std::size_t>(sample)]))
                failed.store(true, std::memory_order_relaxed);
        ++block;
    }
    producer.join();
    require(! failed.load(std::memory_order_relaxed),
            "concurrent curve publication corrupted audio or compilation");
}

void verifyRateAndBlockMatrix()
{
    for (const auto sampleRate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (const auto blockSize : { 1, 17, 64, 511, 2048 })
        {
            SATTR::WaveShape::Core core;
            core.prepare(sampleRate, blockSize, 2);
            core.reset(0.37f, -0.82f);
            auto loader = SATTR::WaveShape::makeDefaultState().loaders[0];
            for (auto& slot : loader.slots)
            {
                slot.unipolar = makeCurve(0.62f, 0.75f);
                slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
            }
            require(core.compileAndPublish(loader).published, "matrix curve was not published");
            std::vector<float> left(static_cast<std::size_t>(blockSize));
            std::vector<float> right(static_cast<std::size_t>(blockSize));
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto phase = static_cast<float>(sample + 1);
                left[static_cast<std::size_t>(sample)] = 251.18864f * std::sin(0.031f * phase);
                right[static_cast<std::size_t>(sample)] = -251.18864f * std::cos(0.023f * phase);
            }
            float* channels[] = { left.data(), right.data() };
            core.process(channels, 2, blockSize);
            require(core.curveCrossfadeSamples() == std::max(1, static_cast<int>(std::round(sampleRate * 0.015))),
                    "crossfade duration is hardcoded to a sample rate");
            for (int sample = 0; sample < blockSize; ++sample)
                require(std::isfinite(left[static_cast<std::size_t>(sample)])
                            && std::isfinite(right[static_cast<std::size_t>(sample)]),
                        "+48 dB matrix input produced non-finite audio");
        }
}

double aliasRatioDb(const std::vector<float>& signal, int fundamentalBin)
{
    int order = 0;
    while ((1 << order) < static_cast<int>(signal.size())) ++order;
    require((1 << order) == static_cast<int>(signal.size()), "FFT signal size is not a power of two");
    juce::dsp::FFT fft(order);
    std::vector<float> spectrum(signal.size() * 2, 0.0f);
    std::copy(signal.begin(), signal.end(), spectrum.begin());
    fft.performFrequencyOnlyForwardTransform(spectrum.data());
    std::vector<bool> harmonic(spectrum.size() / 4 + 1, false);
    harmonic[0] = true;
    for (int multiple = 1; multiple * fundamentalBin < static_cast<int>(harmonic.size()); multiple += 2)
        for (int offset = -1; offset <= 1; ++offset)
        {
            const auto bin = multiple * fundamentalBin + offset;
            if (bin >= 0 && bin < static_cast<int>(harmonic.size()))
                harmonic[static_cast<std::size_t>(bin)] = true;
        }
    double aliasPower = 0.0;
    double totalPower = 1.0e-30;
    for (std::size_t bin = 0; bin < harmonic.size(); ++bin)
    {
        const auto magnitude = static_cast<double>(spectrum[bin]);
        const auto power = magnitude * magnitude;
        totalPower += power;
        if (! harmonic[bin]) aliasPower += power;
    }
    return 10.0 * std::log10(std::max(aliasPower / totalPower, 1.0e-30));
}

void verifyActualCoreAliasing()
{
    constexpr int fftOrder = 16;
    constexpr int sampleCount = 1 << fftOrder;
    constexpr int fundamentalBin = 997;
    constexpr double sampleRate = 48000.0;
    SATTR::WaveShape::Core core;
    core.prepare(sampleRate, 512, 1);
    auto loader = SATTR::WaveShape::makeDefaultState().loaders[0];
    TR::Curves::Curve hard;
    constexpr int pointCount = 64;
    hard.points.reserve(pointCount);
    const auto normaliser = std::tanh(12.0f);
    for (int index = 0; index < pointCount; ++index)
    {
        const auto x = static_cast<float>(index) / static_cast<float>(pointCount - 1);
        hard.points.push_back({ x, std::tanh(12.0f * x) / normaliser, 0.0f });
    }
    for (auto& slot : loader.slots)
    {
        slot.unipolar = hard;
        slot.bipolar = TR::Curves::mirrorXY(hard);
    }
    require(core.compileAndPublish(loader).published, "alias probe curve was not published");
    settleCurveCrossfade(core);
    core.reset();

    SATTR::WaveShape::CompiledCurve compiled;
    require(SATTR::WaveShape::compileCurve(hard, TR::Curves::Domain::unipolar, compiled),
            "alias probe reference curve did not compile");
    std::vector<float> direct(sampleCount);
    std::vector<float> adaa(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi
                         * static_cast<double>(fundamentalBin * sample)
                         / static_cast<double>(sampleCount);
        const auto input = 1.15f * static_cast<float>(std::sin(phase));
        direct[static_cast<std::size_t>(sample)] = compiled.evaluate(input);
        adaa[static_cast<std::size_t>(sample)] = input;
    }
    processMono(core, adaa);
    const auto directAlias = aliasRatioDb(direct, fundamentalBin);
    const auto adaaAlias = aliasRatioDb(adaa, fundamentalBin);
    require(adaaAlias < directAlias - 2.0,
            "actual ADAA core did not materially reduce aliasing");
    std::cout << "waveshape_alias_db_direct_x1=" << directAlias
              << " waveshape_alias_db_adaa_x1=" << adaaAlias << '\n';
}

void reportPerformance()
{
    SATTR::WaveShape::Core core;
    core.prepare(48000.0, 64, 2);
    auto loader = SATTR::WaveShape::makeDefaultState().loaders[0];
    for (auto& slot : loader.slots)
    {
        slot.unipolar = makeCurve(0.7f, 0.5f);
        slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
    }
    require(core.compileAndPublish(loader).published, "benchmark curve was not published");
    settleCurveCrossfade(core);
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    float* channels[] = { left.data(), right.data() };
    constexpr int blocks = 12000;
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < blocks; ++block)
    {
        for (int sample = 0; sample < 64; ++sample)
            left[static_cast<std::size_t>(sample)] = right[static_cast<std::size_t>(sample)]
                = 0.8f * std::sin(0.01f * static_cast<float>(block * 64 + sample));
        core.process(channels, 2, 64);
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto audioSeconds = static_cast<double>(blocks * 64) / 48000.0;
    std::cout << "waveshape_core_rt_percent_48k_b64=" << 100.0 * elapsed / audioSeconds << '\n';
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        verifyCompiler();
        verifyAdaaAndControls();
        verifyIndependentSeriesStages();
        verifyPublicationAndCrossfade();
        verifyConcurrentPublication();
        verifyRateAndBlockMatrix();
        verifyActualCoreAliasing();
        reportPerformance();
        std::cout << "SAT WaveShape phase 2 core probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SAT WaveShape phase 2 core probe failed: " << error.what() << '\n';
        return 1;
    }
}

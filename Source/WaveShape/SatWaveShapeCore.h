#pragma once

#include "SatWaveShapeState.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace SATTR::WaveShape
{
inline constexpr int compiledCurveSize = 4097;
inline constexpr int maximumAudioChannels = 2;
inline constexpr int maximumSeriesStages = 4;
inline constexpr float adaaIntrinsicDelaySamples = 0.5f;

struct CompileIssue
{
    juce::String path;
    juce::String message;
};

struct CompileReport
{
    std::vector<CompileIssue> issues;

    bool ok() const noexcept { return issues.empty(); }
    explicit operator bool() const noexcept { return ok(); }
    void add(juce::String path, juce::String message);
};

struct CompiledCurve
{
    std::array<float, compiledCurveSize> values {};
    std::array<float, compiledCurveSize> integrals {};
    bool identity = true;

    float evaluate(float input) const noexcept;
    float evaluateAntiderivative(float input) const noexcept;
};

struct CompiledBank
{
    std::array<CompiledCurve, slotCount> curves;
    std::uint64_t generation = 0;
};

bool compileCurve(const TR::Curves::Curve&, TR::Curves::Domain,
                  CompiledCurve&, CompileReport* = nullptr,
                  juce::String path = {});

class Core
{
public:
    struct PublishResult
    {
        bool published = false;
        bool busy = false;
        std::uint64_t generation = 0;
        CompileReport report;
    };

    Core();
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    PublishResult compileAndPublish(const LoaderState&);

    void prepare(double processingSampleRate, int maximumBlockSize, int channelCount);
    void reset(float morph = 0.0f, float bias = 0.0f) noexcept;
    void setControlTargets(float morph, float bias) noexcept;
    void process(float* const* channels, int channelCount, int sampleCount,
                 int seriesStages = 1) noexcept;
    bool makeTransferSnapshot(float morph, float bias, float* output,
                              int pointCount, float inputMinimum = -1.15f,
                              float inputMaximum = 1.15f) const noexcept;

    std::uint64_t activeGeneration() const noexcept { return activeGeneration_; }
    bool isCurveCrossfading() const noexcept { return crossfadeRemaining_ > 0; }
    int curveCrossfadeSamples() const noexcept { return crossfadeSamples_; }
    static constexpr float intrinsicDelaySamples() noexcept { return adaaIntrinsicDelaySamples; }

private:
    struct Publisher;

    struct LinearSmoother
    {
        void prepare(double sampleRate, double seconds) noexcept;
        void reset(float value) noexcept;
        void setTarget(float value) noexcept;
        float next() noexcept;

        float current = 0.0f;
        float target = 0.0f;
        float step = 0.0f;
        int remaining = 0;
        int rampLength = 1;
    };

    float evaluate(const CompiledBank&, float input, float previousInput,
                   float morph, float bias) const noexcept;
    void adoptPublishedBank() noexcept;
    void finishCrossfade() noexcept;

    std::unique_ptr<Publisher> publisher_;
    int currentSlot_ = 0;
    int previousSlot_ = -1;
    std::uint64_t activeGeneration_ = 1;
    std::array<std::array<float, maximumAudioChannels>, maximumSeriesStages> previousInput_ {};
    LinearSmoother morphSmoother_;
    LinearSmoother biasSmoother_;
    int preparedChannels_ = maximumAudioChannels;
    int crossfadeSamples_ = 1;
    int crossfadeRemaining_ = 0;
};
}

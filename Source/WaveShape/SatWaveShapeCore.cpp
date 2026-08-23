#include "SatWaveShapeCore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace SATTR::WaveShape
{
namespace
{
constexpr float curveMinimum = -1.0f;
constexpr float curveMaximum = 1.0f;
constexpr float curveStep = (curveMaximum - curveMinimum)
                          / static_cast<float>(compiledCurveSize - 1);
constexpr float adaaDifferenceThreshold = 1.0e-5f;
constexpr double controlSmoothingSeconds = 0.015;
constexpr double curveCrossfadeSeconds = 0.015;

float clamp01(float value) noexcept
{
    return std::max(0.0f, std::min(1.0f, value));
}

float boundedBezier(float t, float curvature) noexcept
{
    t = clamp01(t);
    curvature = std::max(-1.0f, std::min(1.0f, curvature));
    float control1 = 1.0f / 3.0f;
    float control2 = 2.0f / 3.0f;
    if (curvature >= 0.0f)
    {
        control1 += curvature * (2.0f / 3.0f);
        control2 += curvature * (1.0f / 3.0f);
    }
    else
    {
        const auto magnitude = -curvature;
        control1 *= 1.0f - magnitude;
        control2 *= 1.0f - magnitude;
    }
    const auto inverse = 1.0f - t;
    return 3.0f * inverse * inverse * t * control1
         + 3.0f * inverse * t * t * control2 + t * t * t;
}

float evaluateSourceCurve(const TR::Curves::Curve& curve, float x) noexcept
{
    std::size_t segment = 0;
    while (segment + 2 < curve.points.size() && x > curve.points[segment + 1].x)
        ++segment;
    const auto& left = curve.points[segment];
    const auto& right = curve.points[segment + 1];
    const auto span = right.x - left.x;
    const auto local = span > 0.0f ? (x - left.x) / span : 0.0f;
    return left.y + (right.y - left.y) * boundedBezier(local, left.curvature);
}

bool isIdentityCurve(const TR::Curves::Curve& curve) noexcept
{
    for (const auto& point : curve.points)
        if (std::abs(point.x - point.y) > 1.0e-7f
            || std::abs(point.curvature) > 1.0e-7f)
            return false;
    return true;
}
}

void CompileReport::add(juce::String path, juce::String message)
{
    issues.push_back({ std::move(path), std::move(message) });
}

float CompiledCurve::evaluate(float input) const noexcept
{
    if (input <= curveMinimum) return values.front();
    if (input >= curveMaximum) return values.back();
    const auto position = (input - curveMinimum) / curveStep;
    const auto lower = static_cast<int>(position);
    const auto upper = std::min(compiledCurveSize - 1, lower + 1);
    const auto fraction = position - static_cast<float>(lower);
    const auto lowerValue = values[static_cast<std::size_t>(lower)];
    return lowerValue + fraction
         * (values[static_cast<std::size_t>(upper)] - lowerValue);
}

float CompiledCurve::evaluateAntiderivative(float input) const noexcept
{
    if (input <= curveMinimum)
        return integrals.front() + values.front() * (input - curveMinimum);
    if (input >= curveMaximum)
        return integrals.back() + values.back() * (input - curveMaximum);

    const auto position = (input - curveMinimum) / curveStep;
    const auto lower = static_cast<int>(position);
    const auto upper = std::min(compiledCurveSize - 1, lower + 1);
    const auto fraction = position - static_cast<float>(lower);
    const auto lowerValue = values[static_cast<std::size_t>(lower)];
    const auto delta = values[static_cast<std::size_t>(upper)] - lowerValue;
    return integrals[static_cast<std::size_t>(lower)]
         + curveStep * (lowerValue * fraction + 0.5f * delta * fraction * fraction);
}

bool compileCurve(const TR::Curves::Curve& source, TR::Curves::Domain domain,
                  CompiledCurve& destination, CompileReport* outputReport,
                  juce::String path)
{
    CompileReport report;
    const auto maximumPoints = domain == TR::Curves::Domain::unipolar
                             ? maximumUnipolarPointCount : maximumBipolarPointCount;
    const auto validation = TR::Curves::validate(source, domain, maximumPoints);
    for (const auto& issue : validation.issues)
        report.add(path + "/" + issue.path, issue.message);
    if (! report.ok())
    {
        if (outputReport != nullptr) *outputReport = report;
        return false;
    }

    const auto effective = domain == TR::Curves::Domain::unipolar
                         ? TR::Curves::mirrorXY(source) : source;
    CompiledCurve compiled;
    compiled.identity = isIdentityCurve(effective);
    for (int index = 0; index < compiledCurveSize; ++index)
    {
        const auto x = curveMinimum + curveStep * static_cast<float>(index);
        compiled.values[static_cast<std::size_t>(index)] = evaluateSourceCurve(effective, x);
    }

    compiled.integrals.front() = 0.0f;
    for (int index = 1; index < compiledCurveSize; ++index)
        compiled.integrals[static_cast<std::size_t>(index)] =
            compiled.integrals[static_cast<std::size_t>(index - 1)]
            + 0.5f * curveStep
            * (compiled.values[static_cast<std::size_t>(index - 1)]
               + compiled.values[static_cast<std::size_t>(index)]);
    const auto integralAtZero = compiled.integrals[static_cast<std::size_t>(compiledCurveSize / 2)];
    for (auto& value : compiled.integrals) value -= integralAtZero;

    destination = std::move(compiled);
    if (outputReport != nullptr) *outputReport = report;
    return true;
}

struct Core::Publisher
{
    struct Slot
    {
        CompiledBank bank;
        std::atomic<int> readers { 0 };
    };

    Publisher()
    {
        CompiledCurve identity;
        CompileReport report;
        const auto source = TR::Curves::makeIdentity(TR::Curves::Domain::unipolar);
        const auto compiled = compileCurve(source, TR::Curves::Domain::unipolar,
                                           identity, &report, "identity");
        jassert(compiled && report.ok());
        juce::ignoreUnused(compiled);
        for (auto& slot : slots)
        {
            slot.bank.curves[0] = identity;
            slot.bank.curves[1] = identity;
            slot.bank.generation = 1;
        }
        slots[0].readers.store(1, std::memory_order_relaxed);
    }

    bool publish(CompiledBank bank, std::uint64_t& outputGeneration) noexcept
    {
        const auto published = publishedIndex.load(std::memory_order_acquire);
        for (int index = 0; index < static_cast<int>(slots.size()); ++index)
        {
            if (index == published) continue;
            int expected = 0;
            if (! slots[static_cast<std::size_t>(index)].readers.compare_exchange_strong(
                    expected, -1, std::memory_order_acq_rel))
                continue;
            if (index == publishedIndex.load(std::memory_order_acquire))
            {
                slots[static_cast<std::size_t>(index)].readers.store(0, std::memory_order_release);
                continue;
            }

            bank.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
            slots[static_cast<std::size_t>(index)].bank = std::move(bank);
            slots[static_cast<std::size_t>(index)].readers.store(0, std::memory_order_release);
            publishedIndex.store(index, std::memory_order_release);
            outputGeneration = slots[static_cast<std::size_t>(index)].bank.generation;
            return true;
        }
        return false;
    }

    int acquirePublished() noexcept
    {
        for (;;)
        {
            const auto index = publishedIndex.load(std::memory_order_acquire);
            auto& readers = slots[static_cast<std::size_t>(index)].readers;
            int count = readers.load(std::memory_order_acquire);
            if (count >= 0 && readers.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel))
                return index;
        }
    }

    void release(int index) noexcept
    {
        if (index >= 0)
            slots[static_cast<std::size_t>(index)].readers.fetch_sub(1, std::memory_order_acq_rel);
    }

    const CompiledBank& bank(int index) const noexcept
    {
        return slots[static_cast<std::size_t>(index)].bank;
    }

    std::array<Slot, 3> slots;
    std::atomic<int> publishedIndex { 0 };
    std::atomic<std::uint64_t> nextGeneration { 1 };
};

Core::Core() : publisher_(std::make_unique<Publisher>()) {}

Core::~Core()
{
    if (publisher_ != nullptr)
    {
        publisher_->release(previousSlot_);
        publisher_->release(currentSlot_);
    }
}

Core::PublishResult Core::compileAndPublish(const LoaderState& state)
{
    PublishResult result;
    CompiledBank bank;
    const auto domain = state.polarity == PolarityMode::bipolar
                      ? TR::Curves::Domain::bipolar : TR::Curves::Domain::unipolar;
    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        const auto& slot = state.slots[static_cast<std::size_t>(slotIndex)];
        if (domain == TR::Curves::Domain::bipolar && ! slot.bipolarInitialised)
        {
            result.report.add("slot[" + juce::String(slotIndex) + "]/bipolarInitialised",
                              "active bipolar curve has not been initialised");
            continue;
        }
        const auto& curve = domain == TR::Curves::Domain::bipolar ? slot.bipolar : slot.unipolar;
        CompileReport curveReport;
        if (! compileCurve(curve, domain, bank.curves[static_cast<std::size_t>(slotIndex)],
                           &curveReport, "slot[" + juce::String(slotIndex) + "]"))
        {
            result.report.issues.insert(result.report.issues.end(),
                                        curveReport.issues.begin(), curveReport.issues.end());
        }
    }
    if (! result.report.ok()) return result;
    result.published = publisher_->publish(std::move(bank), result.generation);
    result.busy = ! result.published;
    return result;
}

void Core::LinearSmoother::prepare(double sampleRate, double seconds) noexcept
{
    rampLength = std::max(1, static_cast<int>(std::round(sampleRate * seconds)));
}

void Core::LinearSmoother::reset(float value) noexcept
{
    current = target = value;
    step = 0.0f;
    remaining = 0;
}

void Core::LinearSmoother::setTarget(float value) noexcept
{
    if (value == target) return;
    target = value;
    remaining = rampLength;
    step = (target - current) / static_cast<float>(remaining);
}

float Core::LinearSmoother::next() noexcept
{
    if (remaining > 0)
    {
        current += step;
        if (--remaining == 0) current = target;
    }
    return current;
}

void Core::prepare(double processingSampleRate, int maximumBlockSize, int channelCount)
{
    juce::ignoreUnused(maximumBlockSize);
    const auto sampleRate = std::max(1.0, processingSampleRate);
    preparedChannels_ = juce::jlimit(1, maximumAudioChannels, channelCount);
    morphSmoother_.prepare(sampleRate, controlSmoothingSeconds);
    biasSmoother_.prepare(sampleRate, controlSmoothingSeconds);
    crossfadeSamples_ = std::max(1, static_cast<int>(std::round(sampleRate * curveCrossfadeSeconds)));
    reset(morphSmoother_.current, biasSmoother_.current);
}

void Core::reset(float morph, float bias) noexcept
{
    for (auto& stage : previousInput_) stage.fill(0.0f);
    morphSmoother_.reset(juce::jlimit(0.0f, 1.0f, morph));
    biasSmoother_.reset(juce::jlimit(-1.0f, 1.0f, bias));
    finishCrossfade();
}

void Core::setControlTargets(float morph, float bias) noexcept
{
    morphSmoother_.setTarget(juce::jlimit(0.0f, 1.0f, morph));
    biasSmoother_.setTarget(juce::jlimit(-1.0f, 1.0f, bias));
}

float Core::evaluate(const CompiledBank& bank, float input, float previousInput,
                     float morph, float bias) const noexcept
{
    const auto biasOffset = 0.5f * bias;
    const auto delta = input - previousInput;
    std::array<float, slotCount> outputs {};
    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
    {
        const auto& curve = bank.curves[static_cast<std::size_t>(slotIndex)];
        const auto baseline = curve.evaluate(biasOffset);
        if (std::abs(delta) > adaaDifferenceThreshold)
            outputs[static_cast<std::size_t>(slotIndex)] =
                (curve.evaluateAntiderivative(input + biasOffset)
                 - curve.evaluateAntiderivative(previousInput + biasOffset)) / delta
                - baseline;
        else
            outputs[static_cast<std::size_t>(slotIndex)] =
                curve.evaluate(0.5f * (input + previousInput) + biasOffset) - baseline;
    }
    return outputs[0] + morph * (outputs[1] - outputs[0]);
}

void Core::adoptPublishedBank() noexcept
{
    if (crossfadeRemaining_ > 0) return;
    const auto next = publisher_->acquirePublished();
    const auto generation = publisher_->bank(next).generation;
    if (generation <= activeGeneration_ || next == currentSlot_)
    {
        publisher_->release(next);
        activeGeneration_ = std::max(activeGeneration_, generation);
        return;
    }
    previousSlot_ = currentSlot_;
    currentSlot_ = next;
    activeGeneration_ = generation;
    crossfadeRemaining_ = crossfadeSamples_;
}

void Core::finishCrossfade() noexcept
{
    crossfadeRemaining_ = 0;
    if (previousSlot_ >= 0)
    {
        publisher_->release(previousSlot_);
        previousSlot_ = -1;
    }
}

void Core::process(float* const* channels, int channelCount, int sampleCount,
                   int seriesStages) noexcept
{
    if (channels == nullptr || sampleCount <= 0) return;
    const auto channelsToProcess = juce::jlimit(0, preparedChannels_, channelCount);
    if (channelsToProcess <= 0) return;
    const auto stagesToProcess = juce::jlimit(1, maximumSeriesStages, seriesStages);
    adoptPublishedBank();

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto morph = morphSmoother_.next();
        const auto bias = biasSmoother_.next();
        const auto fade = crossfadeRemaining_ > 0
                        ? static_cast<float>(crossfadeSamples_ - crossfadeRemaining_ + 1)
                          / static_cast<float>(crossfadeSamples_)
                        : 1.0f;
        for (int channel = 0; channel < channelsToProcess; ++channel)
        {
            auto* data = channels[channel];
            if (data == nullptr) continue;
            auto stageInput = data[sample];
            for (int stage = 0; stage < stagesToProcess; ++stage)
            {
                auto& previousInput = previousInput_[static_cast<std::size_t>(stage)]
                                                    [static_cast<std::size_t>(channel)];
                const auto current = evaluate(publisher_->bank(currentSlot_), stageInput,
                                              previousInput, morph, bias);
                auto stageOutput = current;
                if (previousSlot_ >= 0)
                {
                    const auto previous = evaluate(publisher_->bank(previousSlot_), stageInput,
                                                   previousInput, morph, bias);
                    stageOutput = previous + fade * (current - previous);
                }
                previousInput = stageInput;
                stageInput = stageOutput;
            }
            data[sample] = stageInput;
        }
        if (crossfadeRemaining_ > 0 && --crossfadeRemaining_ == 0)
            finishCrossfade();
    }
}

bool Core::makeTransferSnapshot(float morph, float bias, float* output,
                                int pointCount, float inputMinimum,
                                float inputMaximum) const noexcept
{
    if (publisher_ == nullptr || output == nullptr || pointCount < 2
        || ! std::isfinite(inputMinimum) || ! std::isfinite(inputMaximum)
        || inputMaximum <= inputMinimum)
        return false;

    const auto slot = publisher_->acquirePublished();
    const auto& bank = publisher_->bank(slot);
    const auto amount = juce::jlimit(0.0f, 1.0f, morph);
    const auto offset = 0.5f * juce::jlimit(-1.0f, 1.0f, bias);
    for (int index = 0; index < pointCount; ++index)
    {
        const auto proportion = static_cast<float>(index) / static_cast<float>(pointCount - 1);
        const auto input = inputMinimum + proportion * (inputMaximum - inputMinimum);
        const auto a = bank.curves[0].evaluate(input + offset)
                     - bank.curves[0].evaluate(offset);
        const auto b = bank.curves[1].evaluate(input + offset)
                     - bank.curves[1].evaluate(offset);
        output[index] = a + amount * (b - a);
    }
    publisher_->release(slot);
    return true;
}
}

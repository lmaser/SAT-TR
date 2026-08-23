#pragma once

#include "../../../TR-Shared/Curves/State/TRCurveState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace SATTR::WaveShape
{
inline constexpr int schemaVersion = 1;
inline constexpr int loaderCount = 3;
inline constexpr int slotCount = 2;
inline constexpr std::size_t maximumUnipolarPointCount = 64;
inline constexpr std::size_t maximumBipolarPointCount = maximumUnipolarPointCount * 2 - 1;

enum class PolarityMode
{
    unipolar,
    bipolar
};

struct SlotState
{
    TR::Curves::Curve unipolar;
    TR::Curves::Curve bipolar;
    bool bipolarInitialised = false;

    bool operator==(const SlotState& other) const noexcept;
};

struct LoaderState
{
    bool enabled = false;
    PolarityMode polarity = PolarityMode::unipolar;
    std::array<SlotState, slotCount> slots;

    bool operator==(const LoaderState& other) const noexcept;
};

struct State
{
    std::array<LoaderState, loaderCount> loaders;

    bool operator==(const State& other) const noexcept;
};

struct ValidationIssue
{
    juce::String path;
    juce::String message;
};

struct ValidationReport
{
    std::vector<ValidationIssue> issues;

    bool ok() const noexcept { return issues.empty(); }
    explicit operator bool() const noexcept { return ok(); }
    void add(juce::String path, juce::String message);
};

struct DecodeResult
{
    bool ok = false;
    bool migratedFromMissingState = false;
    State state;
    ValidationReport report;
};

State makeDefaultState();
bool setPolarityMode(LoaderState&, PolarityMode);
ValidationReport validate(const State&);

std::optional<juce::ValueTree> encodeState(const State&, ValidationReport* = nullptr);
DecodeResult decodeState(const juce::ValueTree&);
DecodeResult readStateFromParent(const juce::ValueTree&);
bool replaceStateInParent(juce::ValueTree&, const State&, ValidationReport* = nullptr);

const juce::Identifier& stateTreeType();
}

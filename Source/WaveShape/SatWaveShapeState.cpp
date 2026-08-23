#include "SatWaveShapeState.h"

#include <cmath>
#include <utility>

namespace SATTR::WaveShape
{
namespace
{
const juce::Identifier waveShapeType { "WAVESHAPER_STATE" };
const juce::Identifier loaderType { "LOADER" };
const juce::Identifier slotType { "SLOT" };
const juce::Identifier unipolarType { "UNIPOLAR" };
const juce::Identifier bipolarType { "BIPOLAR" };
const juce::Identifier pointType { "POINT" };

juce::String polarityText(PolarityMode polarity)
{
    return polarity == PolarityMode::bipolar ? "bipolar" : "unipolar";
}

std::optional<PolarityMode> parsePolarity(const juce::var& value)
{
    if (! value.isString()) return std::nullopt;
    const auto text = value.toString().trim().toLowerCase();
    if (text == "unipolar") return PolarityMode::unipolar;
    if (text == "bipolar") return PolarityMode::bipolar;
    return std::nullopt;
}

void appendCurveIssues(ValidationReport& destination, const TR::Curves::ValidationReport& source,
                       const juce::String& path)
{
    for (const auto& issue : source.issues)
        destination.add(path + "/" + issue.path, issue.message);
}

juce::ValueTree encodeCurve(const TR::Curves::Curve& curve, const juce::Identifier& type)
{
    juce::ValueTree tree(type);
    for (const auto& point : curve.points)
    {
        juce::ValueTree child(pointType);
        child.setProperty("x", point.x, nullptr);
        child.setProperty("y", point.y, nullptr);
        child.setProperty("curvature", point.curvature, nullptr);
        tree.addChild(child, -1, nullptr);
    }
    return tree;
}

bool decodeCurve(const juce::ValueTree& tree, TR::Curves::Curve& curve,
                 TR::Curves::Domain domain, const juce::String& path,
                 ValidationReport& report)
{
    if (! tree.isValid())
    {
        report.add(path, "curve node is missing");
        return false;
    }

    TR::Curves::Curve decoded;
    decoded.points.reserve(static_cast<std::size_t>(tree.getNumChildren()));
    for (int index = 0; index < tree.getNumChildren(); ++index)
    {
        const auto child = tree.getChild(index);
        if (! child.hasType(pointType))
        {
            report.add(path + "/" + juce::String(index), "unexpected curve child");
            continue;
        }
        decoded.points.push_back({ static_cast<float>((double) child.getProperty("x")),
                                   static_cast<float>((double) child.getProperty("y")),
                                   static_cast<float>((double) child.getProperty("curvature")) });
    }

    const auto maximumPointCount = domain == TR::Curves::Domain::bipolar
                                 ? maximumBipolarPointCount
                                 : maximumUnipolarPointCount;
    const auto curveReport = TR::Curves::validate(decoded, domain, maximumPointCount);
    appendCurveIssues(report, curveReport, path);
    if (! curveReport.ok()) return false;
    curve = std::move(decoded);
    return true;
}
}

bool SlotState::operator==(const SlotState& other) const noexcept
{
    return unipolar == other.unipolar && bipolar == other.bipolar
        && bipolarInitialised == other.bipolarInitialised;
}

bool LoaderState::operator==(const LoaderState& other) const noexcept
{
    return enabled == other.enabled && polarity == other.polarity
        && slots == other.slots;
}

bool State::operator==(const State& other) const noexcept { return loaders == other.loaders; }

void ValidationReport::add(juce::String path, juce::String message)
{
    issues.push_back({ std::move(path), std::move(message) });
}

State makeDefaultState()
{
    State result;
    for (auto& loader : result.loaders)
        for (auto& slot : loader.slots)
        {
            slot.unipolar = TR::Curves::makeIdentity(TR::Curves::Domain::unipolar);
            slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
        }
    return result;
}

bool setPolarityMode(LoaderState& loader, PolarityMode polarity)
{
    if (polarity == PolarityMode::bipolar)
    {
        for (const auto& slot : loader.slots)
            if (! slot.bipolarInitialised
                && ! TR::Curves::validate(slot.unipolar, TR::Curves::Domain::unipolar,
                                          maximumUnipolarPointCount).ok())
                return false;
        for (auto& slot : loader.slots)
            if (! slot.bipolarInitialised)
            {
                slot.bipolar = TR::Curves::mirrorXY(slot.unipolar);
                slot.bipolarInitialised = true;
            }
    }
    loader.polarity = polarity;
    return true;
}

ValidationReport validate(const State& state)
{
    ValidationReport report;
    for (int loaderIndex = 0; loaderIndex < loaderCount; ++loaderIndex)
    {
        const auto& loader = state.loaders[static_cast<std::size_t>(loaderIndex)];
        const auto loaderPath = "loader[" + juce::String(loaderIndex) + "]";
        for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
        {
            const auto& slot = loader.slots[static_cast<std::size_t>(slotIndex)];
            const auto slotPath = loaderPath + "/slot[" + juce::String(slotIndex) + "]";
            if (loader.polarity == PolarityMode::bipolar && ! slot.bipolarInitialised)
                report.add(slotPath + "/bipolarInitialised",
                           "active bipolar mode requires an initialised bipolar curve");
            appendCurveIssues(report,
                              TR::Curves::validate(slot.unipolar, TR::Curves::Domain::unipolar,
                                                   maximumUnipolarPointCount),
                              slotPath + "/unipolar");
            appendCurveIssues(report,
                              TR::Curves::validate(slot.bipolar, TR::Curves::Domain::bipolar,
                                                   maximumBipolarPointCount),
                              slotPath + "/bipolar");
        }
    }
    return report;
}

std::optional<juce::ValueTree> encodeState(const State& state, ValidationReport* outputReport)
{
    auto report = validate(state);
    if (outputReport != nullptr) *outputReport = report;
    if (! report.ok()) return std::nullopt;

    juce::ValueTree root(waveShapeType);
    root.setProperty("schema", schemaVersion, nullptr);
    for (int loaderIndex = 0; loaderIndex < loaderCount; ++loaderIndex)
    {
        const auto& loader = state.loaders[static_cast<std::size_t>(loaderIndex)];
        juce::ValueTree loaderTree(loaderType);
        loaderTree.setProperty("index", loaderIndex, nullptr);
        loaderTree.setProperty("enabled", loader.enabled, nullptr);
        loaderTree.setProperty("polarity", polarityText(loader.polarity), nullptr);
        for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
        {
            const auto& slot = loader.slots[static_cast<std::size_t>(slotIndex)];
            juce::ValueTree slotTree(slotType);
            slotTree.setProperty("index", slotIndex, nullptr);
            slotTree.setProperty("bipolarInitialised", slot.bipolarInitialised, nullptr);
            slotTree.addChild(encodeCurve(slot.unipolar, unipolarType), -1, nullptr);
            slotTree.addChild(encodeCurve(slot.bipolar, bipolarType), -1, nullptr);
            loaderTree.addChild(slotTree, -1, nullptr);
        }
        root.addChild(loaderTree, -1, nullptr);
    }
    return root;
}

DecodeResult decodeState(const juce::ValueTree& tree)
{
    DecodeResult result;
    result.state = makeDefaultState();
    if (! tree.isValid() || ! tree.hasType(waveShapeType))
    {
        result.report.add("WAVESHAPER_STATE", "state node is missing or has the wrong type");
        return result;
    }
    if ((int) tree.getProperty("schema", 0) != schemaVersion)
    {
        result.report.add("WAVESHAPER_STATE/schema", "unsupported schema version");
        return result;
    }

    std::array<bool, loaderCount> seenLoaders {};
    for (int childIndex = 0; childIndex < tree.getNumChildren(); ++childIndex)
    {
        const auto loaderTree = tree.getChild(childIndex);
        if (! loaderTree.hasType(loaderType))
        {
            result.report.add("WAVESHAPER_STATE", "unexpected child node");
            continue;
        }
        const int loaderIndex = (int) loaderTree.getProperty("index", -1);
        if (loaderIndex < 0 || loaderIndex >= loaderCount || seenLoaders[static_cast<std::size_t>(loaderIndex)])
        {
            result.report.add("WAVESHAPER_STATE/LOADER", "loader index is invalid or duplicated");
            continue;
        }
        seenLoaders[static_cast<std::size_t>(loaderIndex)] = true;
        auto& loader = result.state.loaders[static_cast<std::size_t>(loaderIndex)];
        loader.enabled = (bool) loaderTree.getProperty("enabled", false);
        const auto polarity = parsePolarity(loaderTree.getProperty("polarity", "unipolar"));
        if (! polarity.has_value())
            result.report.add("WAVESHAPER_STATE/LOADER/polarity", "polarity is invalid");
        else
            loader.polarity = *polarity;
        std::array<bool, slotCount> seenSlots {};
        for (int slotChildIndex = 0; slotChildIndex < loaderTree.getNumChildren(); ++slotChildIndex)
        {
            const auto slotTree = loaderTree.getChild(slotChildIndex);
            if (! slotTree.hasType(slotType))
            {
                result.report.add("WAVESHAPER_STATE/LOADER", "unexpected child node");
                continue;
            }
            const int slotIndex = (int) slotTree.getProperty("index", -1);
            if (slotIndex < 0 || slotIndex >= slotCount || seenSlots[static_cast<std::size_t>(slotIndex)])
            {
                result.report.add("WAVESHAPER_STATE/LOADER/SLOT", "slot index is invalid or duplicated");
                continue;
            }
            seenSlots[static_cast<std::size_t>(slotIndex)] = true;
            auto& slot = loader.slots[static_cast<std::size_t>(slotIndex)];
            slot.bipolarInitialised = (bool) slotTree.getProperty("bipolarInitialised", false);
            if (slotTree.getNumChildren() != 2)
                result.report.add("WAVESHAPER_STATE/LOADER/SLOT", "slot must contain exactly two curves");
            decodeCurve(slotTree.getChildWithName(unipolarType), slot.unipolar,
                        TR::Curves::Domain::unipolar, "UNIPOLAR", result.report);
            decodeCurve(slotTree.getChildWithName(bipolarType), slot.bipolar,
                        TR::Curves::Domain::bipolar, "BIPOLAR", result.report);
        }
        for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
            if (! seenSlots[static_cast<std::size_t>(slotIndex)])
                result.report.add("WAVESHAPER_STATE/LOADER/SLOT", "required slot is missing");
    }
    for (int loaderIndex = 0; loaderIndex < loaderCount; ++loaderIndex)
        if (! seenLoaders[static_cast<std::size_t>(loaderIndex)])
            result.report.add("WAVESHAPER_STATE/LOADER", "required loader is missing");

    const auto validation = validate(result.state);
    for (const auto& issue : validation.issues)
        result.report.add(issue.path, issue.message);
    result.ok = result.report.ok();
    return result;
}

DecodeResult readStateFromParent(const juce::ValueTree& parent)
{
    const auto tree = parent.getChildWithName(waveShapeType);
    if (tree.isValid()) return decodeState(tree);

    DecodeResult result;
    result.ok = true;
    result.migratedFromMissingState = true;
    result.state = makeDefaultState();
    return result;
}

bool replaceStateInParent(juce::ValueTree& parent, const State& state,
                          ValidationReport* report)
{
    auto encoded = encodeState(state, report);
    if (! encoded.has_value()) return false;
    if (const auto existing = parent.getChildWithName(waveShapeType); existing.isValid())
        parent.removeChild(existing, nullptr);
    parent.addChild(*encoded, -1, nullptr);
    return true;
}

const juce::Identifier& stateTreeType() { return waveShapeType; }
}

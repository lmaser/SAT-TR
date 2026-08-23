#include "../Source/UIV2/SatUiDefinition.h"
#include "../../TR-Shared/SimpleUIV2/Runtime/SimpleUiRuntime.h"

#include <iostream>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace L = TR::LoaderUIV2;
namespace S = TR::SimpleUIV2;

namespace
{
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

std::set<std::string> parameterIds(const L::ScopeSpec& scope)
{
    std::set<std::string> ids;
    for (const auto& item : scope.macros) if (!item.parameterId.empty()) ids.insert(item.parameterId);
    for (const auto& item : scope.headerAccessories) if (!item.parameterId.empty()) ids.insert(item.parameterId);
    for (const auto& page : scope.pages)
    {
        for (const auto& item : page.fixedActions) if (!item.parameterId.empty()) ids.insert(item.parameterId);
        for (const auto& group : page.groups)
            for (const auto& item : group.controls)
                if (!item.parameterId.empty()) ids.insert(item.parameterId);
    }
    return ids;
}

const S::SimpleControlSpec& control(const L::ScopeSpec& scope, const std::string& id)
{
    for (const auto& page : scope.pages)
        for (const auto& group : page.groups)
            for (const auto& item : group.controls)
                if (item.controlId == id) return item;
    throw std::runtime_error("Missing control: " + id);
}

std::size_t visibleMacroCount(const L::ScopeSpec& scope, int model)
{
    const auto loader = static_cast<char>(std::tolower(scope.label.front()));
    S::ParameterSnapshot values {
        { scope.typeParameterId, static_cast<double>(model) },
        { "waveshape_enabled_" + std::string(1, loader), 0.0 }
    };
    return static_cast<std::size_t>(std::count_if(scope.macros.begin(), scope.macros.end(),
        [&values](const auto& macro)
        {
            return std::all_of(macro.visibleWhen.begin(), macro.visibleWhen.end(),
                [&values](const auto& condition) { return S::evaluateCondition(condition, values); });
        }));
}

std::size_t visibleWaveShapeMacroCount(const L::ScopeSpec& scope)
{
    const auto loader = static_cast<char>(std::tolower(scope.label.front()));
    S::ParameterSnapshot values {
        { scope.typeParameterId, 0.0 },
        { "waveshape_enabled_" + std::string(1, loader), 1.0 }
    };
    return static_cast<std::size_t>(std::count_if(scope.macros.begin(), scope.macros.end(),
        [&values](const auto& macro)
        {
            return std::all_of(macro.visibleWhen.begin(), macro.visibleWhen.end(),
                [&values](const auto& condition) { return S::evaluateCondition(condition, values); });
        }));
}
}

int main()
{
    try
    {
        const auto& definition = SATTR::UIV2::definition();
        const auto issues = L::validateDefinition(definition);
        for (const auto& issue : issues)
            std::cerr << issue.code << " " << issue.path << ": " << issue.message << '\n';
        require(!L::hasValidationErrors(issues), "SAT definition rejected");
        require(definition.parameters.size() == 231, "SAT classified-state count changed");
        require(definition.preset.parameterWhitelist.size() == 210, "SAT APVTS preset coverage changed");
        require(definition.preset.musicalStateWhitelist.size() == 9, "SAT external musical state coverage changed");
        for (const auto& parameter : definition.parameters)
            if (parameter.access == S::ParameterAccess::prompt
                || parameter.access == S::ParameterAccess::inspector)
                require(!parameter.accessTargetId.empty(), "Nested parameter lost its access target");
        require(definition.scopes.size() == 4, "SAT requires A/B/C/GLOBAL");
        for (std::size_t index = 0; index < 3; ++index)
        {
            const auto& scope = definition.scopes[index];
            require(scope.macros.size() == 8, "Loader macro substitution set changed");
            require(scope.pages.size() == 4, "Loader task count changed");
            require(scope.asset.kind == L::AssetKind::namModel, "SAT loader lost NAM asset contract");
            require(scope.signatureKind == L::SignatureKind::dynamicTransfer, "SAT loader lost transfer signature contract");
            require(scope.headerAccessories.size() == 1
                        && scope.headerAccessories.front().label == "RAW"
                        && scope.headerAccessories.front().parameterId.find("sat_raw_") == 0,
                    "SAT RAW is not the selected-loader signature-header accessory");
            for (const auto& page : scope.pages)
                for (const auto& group : page.groups)
                    for (const auto& item : group.controls)
                        require(item.parameterId != scope.headerAccessories.front().parameterId,
                                "SAT RAW is duplicated in the scrollable surface");
            const auto& modelControl = scope.pages.front().groups.front().controls.front();
            const auto loader = std::string(1, static_cast<char>('a' + index));
            require(modelControl.choiceLabels == std::vector<std::string> {
                        "CLEAN", "TAPE", "TUBE", "TRANSISTOR", "DIODE",
                        "OVERDRIVE A", "OVERDRIVE B", "CLIPPER", "WAVE SHAPE", "NAM" }
                        && modelControl.choiceValues.empty()
                        && modelControl.choiceActionIds == std::vector<std::string> {
                            "select-legacy-model-" + loader + "-0",
                            "select-legacy-model-" + loader + "-1",
                            "select-legacy-model-" + loader + "-2",
                            "select-legacy-model-" + loader + "-3",
                            "select-legacy-model-" + loader + "-4",
                            "select-legacy-model-" + loader + "-5",
                            "select-legacy-model-" + loader + "-8",
                            "select-legacy-model-" + loader + "-6",
                            "select-waveshape-" + loader,
                            "select-legacy-model-" + loader + "-7" },
                    "SAT composed model order or contextual actions changed");
            const auto& controlRows = scope.pages[2].groups.front().controls;
            require(controlRows.size() >= 2
                        && controlRows[controlRows.size() - 2].label == "CHAOS FILTER"
                        && controlRows.back().label == "CHAOS DELAY",
                    "SAT chaos controls must remain FILTER then DELAY");
            require(visibleMacroCount(scope, 0) == 1, "CLEAN must expose only MIX");
            require(visibleMacroCount(scope, 7) == 2, "NAM must expose NAM SIZE and MIX");
            for (const int model : { 1, 2, 3, 4, 5, 6, 8 })
                require(visibleMacroCount(scope, model) == 4, "Saturation model macro deck changed");
            require(visibleWaveShapeMacroCount(scope) == 4,
                    "WAVE SHAPE must expose MORPH, BIAS, SERIES and MIX");
        }
        auto a = parameterIds(definition.scopes[0]);
        auto b = parameterIds(definition.scopes[1]);
        auto c = parameterIds(definition.scopes[2]);
        require(a.size() == b.size() && b.size() == c.size(), "A/B/C direct surfaces diverged");
        require(definition.routing.topologyLabels.size() == 6, "SAT topology count changed");
        const auto& globalControl = definition.scopes[3].pages[2];
        require(globalControl.fixedActions.size() == 2, "ALIGN controls are not fixed");
        require(globalControl.fixedActions[0].label == "LOADER AUTO ALIGN"
                    && globalControl.fixedActions[0].contextualActionId == "trigger-align"
                    && globalControl.fixedActions[0].parameterId == "align",
                "LOADER AUTO ALIGN lost its visible backend route");
        require(globalControl.fixedActions[1].label == "ALIGN + DI"
                    && globalControl.fixedActions[1].parameterId == "uiDryAlignMode",
                "ALIGN + DI lost its visible state control");
        const auto& globalIo = definition.scopes[3].pages[3];
        bool foundSafeClip = false;
        for (const auto& group : globalIo.groups)
            for (const auto& item : group.controls)
                foundSafeClip = foundSafeClip || item.parameterId == "uiSafeClipMode";
        require(foundSafeClip, "SAFE CLIP lost its visible route");
        require(parameterIds(definition.scopes[3]).count("lim_quality") == 1,
                "LIMIT QUALITY lost its GLOBAL I/O route");
        const auto& quality = control(definition.scopes[3], "limit-quality");
        const auto& threshold = control(definition.scopes[3], "limit-threshold");
        const auto& oversampling = control(definition.scopes[3], "oversampling");
        require(quality.enabledWhen.size() == 1
                    && quality.enabledWhen.front().comparison == S::Comparison::equal
                    && quality.enabledWhen.front().value == 2.0,
                "SAT LIMIT QUALITY must only be editable in GLOBAL mode");
        require(threshold.enabledWhen.size() == 1
                    && threshold.enabledWhen.front().comparison == S::Comparison::greater
                    && threshold.enabledWhen.front().value == 0.0,
                "SAT LIMIT THRESHOLD must remain editable in WET and GLOBAL modes");
        require(oversampling.choiceLabels == std::vector<std::string> {
                    "x1", "x2", "x4", "x8", "x16 EXP." }
                    && oversampling.tooltip.find("x8") != std::string::npos,
                "SAT x16 must retain raw index 4 and be labelled experimental");
        std::cout << "SAT UI V2 definition probe passed: 231 states, 210 APVTS preset parameters, "
                     "9 external musical states and symmetric loader scopes.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SAT UI V2 definition probe failed: " << error.what() << '\n';
        return 1;
    }
}

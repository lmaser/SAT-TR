#include "SatUiDefinition.h"
#include "../../../TR-Shared/LoaderUIV2/Runtime/LoaderUiRuntime.h"

#include <array>
#include <cctype>
#include <iterator>

namespace SATTR::UIV2
{
namespace L = TR::LoaderUIV2;
namespace S = TR::SimpleUIV2;

namespace
{
S::SimpleControlSpec control(std::string id, std::string parameter, std::string label,
                             S::ControlRole role = S::ControlRole::fader)
{
    S::SimpleControlSpec value;
    value.controlId = std::move(id);
    value.parameterId = std::move(parameter);
    value.label = std::move(label);
    value.role = role;
    return value;
}

S::SimpleControlSpec action(std::string id, std::string label, std::string actionId)
{
    auto value = control(std::move(id), {}, std::move(label), S::ControlRole::action);
    value.domain = S::StateDomain::uiInstance;
    value.contextualActionId = std::move(actionId);
    value.visibleAlternativeControlId = value.controlId;
    return value;
}

S::SimpleGroupSpec group(std::string id, std::initializer_list<S::SimpleControlSpec> controls)
{
    S::SimpleGroupSpec value;
    value.groupId = std::move(id);
    value.labelVisibility = S::GroupLabelVisibility::hidden;
    value.controls = controls;
    return value;
}

S::SimplePageSpec page(S::TaskId task,
                       std::initializer_list<S::SimpleGroupSpec> groups,
                       std::initializer_list<S::SimpleControlSpec> fixed = {})
{
    S::SimplePageSpec value;
    value.taskId = task;
    value.label = S::taskName(task);
    value.groups = groups;
    value.fixedActions = fixed;
    return value;
}

S::Condition equal(const std::string& parameter, double value)
{
    return { parameter, S::Comparison::equal, value };
}

S::Condition notEqual(const std::string& parameter, double value)
{
    return { parameter, S::Comparison::notEqual, value };
}

std::string scoped(const char* root, char suffix)
{
    return std::string(root) + "_" + static_cast<char>(std::tolower(suffix));
}

void addParameter(L::LoaderPluginDefinition& result, const std::string& id,
                  S::StateDomain domain = S::StateDomain::musicalParameter,
                  S::ParameterAccess access = S::ParameterAccess::direct,
                  std::string target = {}, std::string backendJustification = {})
{
    result.parameters.push_back({ id, domain, access, std::move(target),
                                  std::move(backendJustification) });
    if (domain == S::StateDomain::musicalParameter)
        result.preset.parameterWhitelist.push_back(id);
    else if (domain == S::StateDomain::musicalState)
        result.preset.musicalStateWhitelist.push_back(id);
}

S::SimplePromptSpec prompt(std::string id, std::string title,
                            std::initializer_list<std::string> parameters)
{
    S::SimplePromptSpec value;
    value.promptId = std::move(id);
    value.title = std::move(title);
    value.parameterIds = parameters;
    return value;
}

S::SimplePromptSpec promptControls(std::string id, std::string title,
                                   std::vector<S::SimpleControlSpec> controls)
{
    S::SimplePromptSpec value;
    value.promptId = std::move(id);
    value.title = std::move(title);
    value.controls = std::move(controls);
    return value;
}

S::SimpleInspectorSpec inspector(std::string id, std::string title,
                                  std::initializer_list<S::SimpleControlSpec> controls)
{
    S::SimpleInspectorSpec value;
    value.inspectorId = std::move(id);
    value.title = std::move(title);
    value.groups.push_back(group(value.inspectorId + "-parameters", controls));
    return value;
}

S::SimpleInspectorSpec inspectorControls(std::string id, std::string title,
                                         std::vector<S::SimpleControlSpec> controls)
{
    S::SimpleInspectorSpec value;
    value.inspectorId = std::move(id);
    value.title = std::move(title);
    S::SimpleGroupSpec parameters;
    parameters.groupId = value.inspectorId + "-parameters";
    parameters.labelVisibility = S::GroupLabelVisibility::hidden;
    parameters.controls = std::move(controls);
    value.groups.push_back(std::move(parameters));
    return value;
}

L::ScopeSpec loaderScope(L::ScopeId scopeId, char suffix)
{
    const auto s = [suffix](const char* root) { return scoped(root, suffix); };
    const auto tag = std::string(1, static_cast<char>(std::toupper(suffix)));
    const auto enabled = s("enable");
    const auto type = s("sat_type");
    const auto waveShape = s("waveshape_enabled");
    const std::vector<S::Condition> isNam { equal(type, 7.0), equal(waveShape, 0.0) };
    const std::vector<S::Condition> isWaveShape { equal(waveShape, 1.0) };
    const std::vector<S::Condition> legacyOnly { equal(waveShape, 0.0) };
    const std::vector<S::Condition> nonlinear {
        notEqual(type, 0.0), notEqual(type, 7.0), equal(waveShape, 0.0)
    };

    L::ScopeSpec scope;
    scope.scopeId = scopeId;
    scope.kind = L::ScopeKind::loader;
    scope.label = tag;
    scope.enableParameterId = enabled;
    scope.typeParameterId = type;
    scope.asset = { L::AssetKind::namModel, "namPath" + tag, ".nam",
                    "browse-nam-" + std::string(1, suffix),
                    "clear-nam-" + std::string(1, suffix), true };
    scope.signatureKind = L::SignatureKind::dynamicTransfer;

    auto driveMacro = control("macro-drive-" + tag, s("sat_drive"), "DRIVE", S::ControlRole::macro);
    driveMacro.visibleWhen = nonlinear;
    auto characterMacro = control("macro-character-" + tag, s("sat_girth"), "CHARACTER", S::ControlRole::macro);
    characterMacro.visibleWhen = nonlinear;
    auto typeMacro = control("macro-type-" + tag, s("sat_mod"), "TYPE", S::ControlRole::macro);
    typeMacro.visibleWhen = nonlinear;
    auto namSizeMacro = control("macro-nam-size-" + tag, s("nam_slim"), "NAM SIZE", S::ControlRole::macro);
    namSizeMacro.visibleWhen = isNam;
    auto morphMacro = control("macro-waveshape-morph-" + tag, s("waveshape_morph"),
                              "MORPH", S::ControlRole::macro);
    morphMacro.visibleWhen = isWaveShape;
    auto waveShapeBiasMacro = control("macro-waveshape-bias-" + tag, s("waveshape_bias"),
                                      "BIAS", S::ControlRole::macro);
    waveShapeBiasMacro.visibleWhen = isWaveShape;
    auto seriesMacro = control("macro-waveshape-series-" + tag, s("series"),
                               "SERIES", S::ControlRole::macro);
    seriesMacro.visibleWhen = isWaveShape;
    scope.macros = { driveMacro, characterMacro, typeMacro, namSizeMacro,
                     morphMacro, waveShapeBiasMacro, seriesMacro,
                     control("macro-mix-" + tag, s("mix"), "MIX", S::ControlRole::macro) };

    auto browse = action("browse-" + tag, "LOAD NAM", "browse-nam-" + std::string(1, suffix));
    browse.visibleWhen = isNam;
    auto clear = action("clear-" + tag, "CLEAR", "clear-nam-" + std::string(1, suffix));
    clear.visibleWhen = isNam;
    auto raw = control("raw-" + tag, s("sat_raw"), "RAW", S::ControlRole::toggle);
    raw.visibleWhen = nonlinear;
    scope.headerAccessories = { raw };
    auto expander = control("expander-" + tag, s("exp"), "EXPANDER", S::ControlRole::toggle);
    expander.inspectorId = "expander-" + tag;
    auto chaosDelay = control("chaos-delay-" + tag, s("chaos"), "CHAOS DELAY", S::ControlRole::toggle);
    chaosDelay.inspectorId = "chaos-delay-" + tag;
    auto chaosFilter = control("chaos-filter-" + tag, s("chaos_filter"), "CHAOS FILTER", S::ControlRole::toggle);
    chaosFilter.inspectorId = "chaos-filter-" + tag;

    auto filter = action("filter-options-" + tag, "FILTER OPTIONS", "open-filter-" + tag);
    filter.contextualActionId.clear();
    filter.promptId = "filter-" + tag;
    auto routing = action("routing-" + tag, "ROUTING", "open-routing-" + tag);
    routing.contextualActionId.clear();
    routing.promptId = "routing-" + tag;

    auto input = control("input-" + tag, s("in"), "INPUT");
    input.meterSource = S::MeterSource::input;
    auto output = control("output-" + tag, s("out"), "OUTPUT");
    output.meterSource = S::MeterSource::output;
    auto model = control("model-" + tag, type, "MODEL", S::ControlRole::choice);
    model.choiceLabels = { "CLEAN", "TAPE", "TUBE", "TRANSISTOR", "DIODE",
                           "OVERDRIVE A", "OVERDRIVE B", "CLIPPER", "WAVE SHAPE", "NAM" };
    model.choiceActionIds = {
        "select-legacy-model-" + std::string(1, suffix) + "-0",
        "select-legacy-model-" + std::string(1, suffix) + "-1",
        "select-legacy-model-" + std::string(1, suffix) + "-2",
        "select-legacy-model-" + std::string(1, suffix) + "-3",
        "select-legacy-model-" + std::string(1, suffix) + "-4",
        "select-legacy-model-" + std::string(1, suffix) + "-5",
        "select-legacy-model-" + std::string(1, suffix) + "-8",
        "select-legacy-model-" + std::string(1, suffix) + "-6",
        "select-waveshape-" + std::string(1, suffix),
        "select-legacy-model-" + std::string(1, suffix) + "-7"
    };

    auto bias = control("bias-" + tag, s("sat_bias"), "BIAS");
    bias.visibleWhen = legacyOnly;
    auto dynamics = control("dynamics-" + tag, s("sat_sag"), "DYNAMICS");
    dynamics.visibleWhen = legacyOnly;
    auto detail = control("detail-" + tag, s("detail"), "DETAIL");
    detail.visibleWhen = legacyOnly;
    auto series = control("series-" + tag, s("series"), "SERIES");
    series.visibleWhen = legacyOnly;
    scope.pages = {
        page(S::TaskId::core,
             { group("source-" + tag,
                     { model, browse, clear }) }),
        page(S::TaskId::shape,
             { group("shape-" + tag,
                     { bias, dynamics, detail, series,
                       control("frequency-" + tag, s("fred"), "FREQUENCY"),
                       control("position-" + tag, s("pos"), "POSITION"),
                       control("offset-" + tag, s("offset"), "OFFSET") }) }),
        page(S::TaskId::control,
             { group("control-" + tag,
                     { control("invert-" + tag, s("inv"), "INVERT", S::ControlRole::toggle),
                       expander, chaosFilter, chaosDelay }) }),
        page(S::TaskId::io,
             { group("io-" + tag,
                     { input, output, control("pan-" + tag, s("pan"), "PAN") }) },
             { filter, routing })
    };
    return scope;
}

L::LoaderPluginDefinition buildDefinition()
{
    L::LoaderPluginDefinition result;
    for (int macro = 1; macro <= 8; ++macro)
    {
        const auto id = "mod_macro_" + std::to_string(macro);
        addParameter(result, id, S::StateDomain::musicalParameter,
                     S::ParameterAccess::backendOnly, {},
                     "Automatable Macro value exposed by the shared MACROS workspace.");
        result.preset.missingParameterDefaults.push_back({ id, 0.0 });
    }
    addParameter(result, "modulation_v1", S::StateDomain::musicalState,
                 S::ParameterAccess::backendOnly, {},
                 "Macro names, routes, source settings and transfer curves.");
    result.preset.missingMusicalStateDefaults.push_back({ "modulation_v1", 0.0 });
    result.product = { "sat-tr", "SAT-TR", "1.4.0",
                       "https://github.com/nmstr/tr-series/issues" };
    result.capabilities = { true, true, false, true, true, true, true };

    constexpr std::array<const char*, 60> loaderRoots {
        "enable", "hp_freq", "lp_freq", "hp_on", "lp_on", "hp_slope", "lp_slope",
        "in", "out", "tilt", "detail", "series", "instability", "pan", "fred",
        "pos", "inv", "offset", "sidechain", "sidechain_gain",
        "sidechain_smooth", "sidechain_hp", "sidechain_lp", "sidechain_hp_on",
        "sidechain_lp_on", "sidechain_hp_slope", "sidechain_lp_slope", "chaos",
        "chaos_filter", "chaos_amt", "chaos_spd", "chaos_amt_filter",
        "chaos_spd_filter", "mode_in", "mode_out", "sum_bus", "filter_pos", "mix",
        "sat_type", "sat_drive", "nam_slim", "sat_girth", "sat_mod", "sat_bias",
        "sat_sag", "sat_raw", "exp", "exp_order", "exp_ratio", "exp_thresh",
        "exp_knee", "exp_atk", "exp_rel", "exp_sc_hp", "exp_sc_lp", "exp_sc_hp_on",
        "exp_sc_lp_on", "exp_sc_hp_slope", "exp_sc_lp_slope", "exp_sc_gain"
    };
    for (const char suffix : { 'a', 'b', 'c' })
    {
        for (const auto* root : loaderRoots)
        {
            const std::string rootName(root);
            const auto tag = std::string(1, static_cast<char>(std::toupper(suffix)));
            S::ParameterAccess access = S::ParameterAccess::direct;
            std::string target;
            std::string backendJustification;
            if (rootName == "instability")
            {
                access = S::ParameterAccess::backendOnly;
                backendJustification = "Legacy Instability parameter retained for presets and host automation; new editing uses a MACROS motion recipe.";
            }
            else if (rootName == "hp_freq" || rootName == "lp_freq" || rootName == "hp_on"
                || rootName == "lp_on" || rootName == "hp_slope" || rootName == "lp_slope"
                || rootName == "tilt")
            {
                access = S::ParameterAccess::prompt;
                target = "filter-" + tag;
            }
            else if (rootName == "mode_in" || rootName == "mode_out"
                     || rootName == "sum_bus" || rootName == "filter_pos")
            {
                access = S::ParameterAccess::prompt;
                target = "routing-" + tag;
            }
            else if (rootName.rfind("sidechain_", 0) == 0)
            {
                access = S::ParameterAccess::prompt;
                target = "sidechain-" + tag;
            }
            else if (rootName.rfind("exp_", 0) == 0)
            {
                access = S::ParameterAccess::inspector;
                target = "expander-" + tag;
            }
            else if (rootName == "chaos_amt" || rootName == "chaos_spd")
            {
                access = S::ParameterAccess::inspector;
                target = "chaos-delay-" + tag;
            }
            else if (rootName == "chaos_amt_filter" || rootName == "chaos_spd_filter")
            {
                access = S::ParameterAccess::inspector;
                target = "chaos-filter-" + tag;
            }
            addParameter(result, scoped(root, suffix), S::StateDomain::musicalParameter,
                         access, std::move(target), std::move(backendJustification));
        }
        addParameter(result, scoped("waveshape_morph", suffix));
        addParameter(result, scoped("waveshape_bias", suffix));
        addParameter(result, scoped("waveshape_enabled", suffix), S::StateDomain::uiInstance,
                     S::ParameterAccess::backendOnly, {},
                     "Derived effective-model flag used only by UI visibility conditions");
    }

    constexpr std::array<const char*, 16> globalParameters {
        "input", "output", "route", "mix", "mix_mode", "dry_level", "wet_level",
        "match", "trim", "lim_threshold", "lim_mode", "lim_quality", "inv_pol", "inv_str",
        "oversample", "align"
    };
    for (const auto* id : globalParameters)
        addParameter(result, id, S::StateDomain::musicalParameter,
                     std::string(id) == "route" ? S::ParameterAccess::prompt
                                                 : S::ParameterAccess::direct,
                     std::string(id) == "route" ? "global-routing" : "");

    for (const auto* id : { "ui_width", "ui_height", "ui_palette", "ui_fx_tail",
                            "ui_io_fx", "ui_color0", "ui_color1", "ui_color2", "ui_color3" })
        addParameter(result, id, S::StateDomain::uiInstance, S::ParameterAccess::notPresented);
    for (const auto* id : { "namPathA", "namPathB", "namPathC", "uiDryAlignMode",
                            "uiSafeClipMode" })
        addParameter(result, id, S::StateDomain::musicalState, S::ParameterAccess::direct);
    for (const auto* id : { "uiDryAlignAnchorA", "uiDryAlignAnchorB", "uiDryAlignAnchorC" })
        addParameter(result, id, S::StateDomain::musicalState, S::ParameterAccess::backendOnly, {},
                     "Processor-owned per-loader alignment anchor consumed by SAT dry alignment");
    for (const auto* id : { "instabilitySeedA", "instabilitySeedB", "instabilitySeedC" })
        addParameter(result, id, S::StateDomain::musicalState, S::ParameterAccess::backendOnly, {},
                     "Processor-owned deterministic seed consumed by SAT component instability");

    result.scopes = { loaderScope(L::ScopeId::loaderA, 'a'),
                      loaderScope(L::ScopeId::loaderB, 'b'),
                      loaderScope(L::ScopeId::loaderC, 'c') };

    L::ScopeSpec global;
    global.scopeId = L::ScopeId::global;
    global.kind = L::ScopeKind::global;
    global.label = "GLOBAL";
    global.signatureKind = L::SignatureKind::routingTopology;
    auto globalMix = control("global-mix", "mix", "MIX", S::ControlRole::macro);
    globalMix.parameterAlternatives = { "wet_level" };
    global.macros = {
        control("global-input", "input", "INPUT", S::ControlRole::macro),
        std::move(globalMix),
        control("global-output", "output", "OUTPUT", S::ControlRole::macro)
    };
    auto globalRoute = action("global-routing", "GLOBAL ROUTING", "open-global-routing");
    globalRoute.contextualActionId.clear();
    globalRoute.promptId = "global-routing";
    auto dryAlign = control("dry-align", "uiDryAlignMode", "ALIGN + DI", S::ControlRole::toggle);
    dryAlign.domain = S::StateDomain::musicalState;
    dryAlign.tooltip = "Include global and per-loader dry paths in alignment";
    auto alignAction = action("align-action", "LOADER AUTO ALIGN", "trigger-align");
    alignAction.parameterId = "align";
    alignAction.tooltip = "Calculate timing and polarity alignment for active loaders; this is not host compensation";
    auto safeClip = control("safe-clip", "uiSafeClipMode", "SAFE CLIP", S::ControlRole::toggle);
    safeClip.domain = S::StateDomain::musicalState;
    safeClip.tooltip = "Clamp final output to the safe ceiling";
    auto mixMode = control("mix-mode", "mix_mode", "MIX MODE", S::ControlRole::choice);
    auto dry = control("dry-level", "dry_level", "DRY");
    dry.visibleWhen = { equal("mix_mode", 1.0) };
    auto limitMode = control("limit-mode", "lim_mode", "LIMIT MODE", S::ControlRole::choice);
    limitMode.choiceLabels = { "NONE", "WET", "GLOBAL" };
    auto limitQuality = control("limit-quality", "lim_quality", "LIMIT QUALITY", S::ControlRole::choice);
    limitQuality.choiceLabels = { "FAST", "CLEAN", "TRUE PEAK" };
    limitQuality.enabledWhen = { S::Condition { "lim_mode", S::Comparison::equal, 2.0 } };
    limitQuality.unavailableReason = "Wet always uses Fast; select Global for Clean or True Peak";
    auto threshold = control("limit-threshold", "lim_threshold", "THRESHOLD");
    threshold.enabledWhen = { S::Condition { "lim_mode", S::Comparison::greater, 0.0 } };
    threshold.unavailableReason = "Choose WET or GLOBAL limiter mode first";
    auto oversampling = control("oversampling", "oversample", "OVERSAMPLING", S::ControlRole::choice);
    oversampling.choiceLabels = { "x1", "x2", "x4", "x8", "x16 EXP." };
    oversampling.tooltip = "x16 is experimental/offline; x8 is the highest release-qualified mode";
    global.pages = {
        page(S::TaskId::core,
             { group("global-core", { std::move(oversampling) }) }),
        page(S::TaskId::shape,
             { group("global-shape", { control("spectral-match", "match", "SPECTRAL MATCH", S::ControlRole::choice),
                                        control("normalization", "trim", "NORMALIZATION", S::ControlRole::choice),
                                        control("invert-polarity", "inv_pol", "INVERT POLARITY", S::ControlRole::choice),
                                        control("invert-stereo", "inv_str", "INVERT STEREO", S::ControlRole::choice) }) }),
        page(S::TaskId::control, {}, { alignAction, dryAlign }),
        page(S::TaskId::io,
             { group("global-io", { mixMode, dry, limitMode, limitQuality, threshold, safeClip }) },
             { globalRoute })
    };
    result.scopes.push_back(std::move(global));

    for (const char suffix : { 'a', 'b', 'c' })
    {
        const auto tag = std::string(1, static_cast<char>(std::toupper(suffix)));
        const auto s = [suffix](const char* root) { return scoped(root, suffix); };
        result.prompts.push_back(promptControls("filter-" + tag, "FILTER OPTIONS",
            L::makeFilterStageControls("filter-" + tag,
                { s("hp_on"), s("hp_freq"), s("hp_slope"), s("lp_on"),
                  s("lp_freq"), s("lp_slope"), s("tilt") })));
        auto inputMode = control("routing-input-" + tag, s("mode_in"),
                                 "INPUT MODE", S::ControlRole::choice);
        inputMode.choiceLabels = { "L+R", "M/S", "MID", "SIDE" };
        auto outputMode = control("routing-output-" + tag, s("mode_out"),
                                  "OUTPUT MODE", S::ControlRole::choice);
        outputMode.choiceLabels = inputMode.choiceLabels;
        auto sumBus = control("routing-sum-" + tag, s("sum_bus"),
                              "SUM BUS", S::ControlRole::choice);
        sumBus.choiceLabels = { "ST", "->M", "->S" };
        auto filterPosition = control("routing-filter-position-" + tag, s("filter_pos"),
                                      "F / T POSITION", S::ControlRole::choice);
        filterPosition.choiceLabels = { "POST/POST", "PRE/PRE", "PRE/POST", "POST/PRE" };
        result.prompts.push_back(promptControls("routing-" + tag, "ROUTING",
            { std::move(inputMode), std::move(outputMode), std::move(sumBus),
              std::move(filterPosition) }));
        std::vector<S::SimpleControlSpec> sidechainControls {
            control("sidechain-gain-" + tag, s("sidechain_gain"), "GAIN"),
            control("sidechain-smooth-" + tag, s("sidechain_smooth"), "SMOOTH")
        };
        auto sidechainFilters = L::makeFilterStageControls("sidechain-filter-" + tag,
            { s("sidechain_hp_on"), s("sidechain_hp"), s("sidechain_hp_slope"),
              s("sidechain_lp_on"), s("sidechain_lp"), s("sidechain_lp_slope"), {} });
        sidechainControls.insert(sidechainControls.end(),
                                 std::make_move_iterator(sidechainFilters.begin()),
                                 std::make_move_iterator(sidechainFilters.end()));
        result.prompts.push_back(promptControls("sidechain-" + tag, "SIDECHAIN " + tag,
                                                std::move(sidechainControls)));
        std::vector<S::SimpleControlSpec> expanderControls {
            L::makePrePostPositionControl("exp-position-" + tag, s("exp_order")),
            control("exp-ratio-" + tag, s("exp_ratio"), "RATIO"),
            control("exp-threshold-" + tag, s("exp_thresh"), "THRESHOLD"),
            control("exp-knee-" + tag, s("exp_knee"), "KNEE"),
            control("exp-attack-" + tag, s("exp_atk"), "ATTACK"),
            control("exp-release-" + tag, s("exp_rel"), "RELEASE")
        };
        auto expanderFilters = L::makeFilterStageControls("expander-filter-" + tag,
            { s("exp_sc_hp_on"), s("exp_sc_hp"), s("exp_sc_hp_slope"),
              s("exp_sc_lp_on"), s("exp_sc_lp"), s("exp_sc_lp_slope"), {} }, "SC ");
        expanderControls.insert(expanderControls.end(),
                                std::make_move_iterator(expanderFilters.begin()),
                                std::make_move_iterator(expanderFilters.end()));
        expanderControls.push_back(control("exp-sc-gain-" + tag, s("exp_sc_gain"), "SC GAIN"));
        result.inspectors.push_back(inspectorControls("expander-" + tag, "EXPANDER",
                                                     std::move(expanderControls)));
        result.inspectors.push_back(inspector("chaos-delay-" + tag, "CHAOS DELAY",
            { control("chaos-amount-" + tag, s("chaos_amt"), "AMOUNT"),
              control("chaos-speed-" + tag, s("chaos_spd"), "SPEED") }));
        result.inspectors.push_back(inspector("chaos-filter-" + tag, "CHAOS FILTER",
            { control("chaos-filter-amount-" + tag, s("chaos_amt_filter"), "AMOUNT"),
              control("chaos-filter-speed-" + tag, s("chaos_spd_filter"), "SPEED") }));
    }
    result.prompts.push_back(prompt("global-routing", "GLOBAL ROUTING", { "route" }));
    for (const char suffix : { 'A', 'B', 'C' })
    {
        const auto tag = std::string(1, suffix);
        auto sidechainRoute = action("macros-sidechain-" + tag, "SIDECHAIN " + tag, {});
        sidechainRoute.promptId = "sidechain-" + tag;
        result.auxiliaryControls.push_back(std::move(sidechainRoute));
    }
    result.routing = { "route", { "A>B>C", "A|B|C", "A>B|C", "A|B>C", "(A|B)>C", "A>(B|C)" },
                       { "mode_in_a", "mode_out_a", "sum_bus_a", "filter_pos_a",
                         "mode_in_b", "mode_out_b", "sum_bus_b", "filter_pos_b",
                         "mode_in_c", "mode_out_c", "sum_bus_c", "filter_pos_c" },
                       "global-routing", "routing" };
    // The enable roots are driven by the shared MACROS workspace rather than
    // a loader page; keep them out of the page contract while preserving the
    // parameter IDs for automation and preset compatibility.
    result.hiddenCompatibilityInputs = { "sidechain_a", "sidechain_b", "sidechain_c" };
    return result;
}
}

const L::LoaderPluginDefinition& definition()
{
    static const auto value = buildDefinition();
    return value;
}
}

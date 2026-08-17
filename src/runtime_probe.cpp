#include "runtime_probe.h"

#include "internal_probe.h"
#include "layout_probe.h"
#include "registry.h"
#include "stl_probe.h"
#include "vtable_probe.h"

#include <endstone/boss/bar_color.h>
#include <endstone/boss/bar_style.h>
#include <endstone/event/event_priority.h>
#include <endstone/logger.h>
#include <endstone/permissions/permission_default.h>
#include <endstone/plugin/plugin_load_order.h>
#include <endstone/version.h>

#include <algorithm>
#include <array>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace abi_probe {
namespace {

std::vector<std::string> split_dependencies(std::string_view text)
{
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto separator = text.find(',');
        const auto token = text.substr(0, separator);
        if (!token.empty()) {
            result.emplace_back(token);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    return result;
}

bool is_derived(std::string_view name)
{
    return name == "ES_BLOCK_SLOT_DELETE" || name == "ES_ITEM_META_SLOT_DELETE" ||
           name == "ES_BOSSBAR_SLOT_DTOR" ||
           name.find("_SLOT_DTOR") != std::string_view::npos;
}

bool is_runtime_object(std::string_view name)
{
    return name == "ES_PLUGIN_OFF_SERVER" || name == "ES_PLUGIN_OFF_LOGGER" ||
           name == "ES_PLUGIN_OFF_DESCRIPTION";
}

bool is_enum(std::string_view name)
{
    return name == "ES_LOAD_POST_WORLD" || name == "ES_PERM_OPERATOR" || name == "ES_LOG_INFO" ||
           name == "ES_PRIORITY_NORMAL" || name == "ES_BAR_COLOR_GREEN" || name == "ES_BAR_STYLE_SOLID";
}

Fact enum_fact(std::string_view name)
{
    if (name == "ES_LOAD_POST_WORLD") {
        return {static_cast<std::int64_t>(endstone::PluginLoadOrder::PostWorld), "compile-time enum expression",
                "endstone::PluginLoadOrder::PostWorld", Provenance::CompileMeasured};
    }
    if (name == "ES_PERM_OPERATOR") {
        return {static_cast<std::int64_t>(endstone::PermissionDefault::Operator), "compile-time enum expression",
                "endstone::PermissionDefault::Operator", Provenance::CompileMeasured};
    }
    if (name == "ES_LOG_INFO") {
        return {static_cast<std::int64_t>(endstone::Logger::Info), "compile-time enum expression",
                "endstone::Logger::Info", Provenance::CompileMeasured};
    }
    if (name == "ES_PRIORITY_NORMAL") {
        return {static_cast<std::int64_t>(endstone::EventPriority::Normal), "compile-time enum expression",
                "endstone::EventPriority::Normal", Provenance::CompileMeasured};
    }
    if (name == "ES_BAR_COLOR_GREEN") {
        return {static_cast<std::int64_t>(endstone::BarColor::Green), "compile-time enum expression",
                "endstone::BarColor::Green", Provenance::CompileMeasured};
    }
    return {static_cast<std::int64_t>(endstone::BarStyle::Solid), "compile-time enum expression",
            "endstone::BarStyle::Solid", Provenance::CompileMeasured};
}

Fact unresolved(std::string_view method, std::string_view evidence)
{
    return {std::nullopt, method, evidence, Provenance::Unresolved};
}

Fact measure_fact(std::string_view name, const endstone::Plugin *plugin, bool live_context)
{
    if (is_enum(name)) {
        return enum_fact(name);
    }
    // ES_API_VERSION is the only string-valued compile fact and is populated
    // directly into Requirement by collect_report; Fact intentionally remains
    // an integer measurement carrier.

    try {
        Fact fact = measure_layout(name, plugin, live_context);
        if (fact.value) {
            return fact;
        }
        fact = measure_internal(name);
        if (fact.value) {
            return fact;
        }
        fact = measure_stl(name);
        if (fact.value) {
            return fact;
        }
        return measure_vtable(name);
    }
    catch (const std::exception &) {
        return unresolved("measurement expression", "measurement expression threw; requirement unresolved");
    }
    catch (...) {
        return unresolved("measurement expression", "measurement expression raised a non-standard exception");
    }
}

bool resolved(const Requirement &requirement)
{
    return !std::holds_alternative<std::monostate>(requirement.value) &&
           requirement.provenance != Provenance::Unresolved;
}

std::size_t unresolved_count(const std::vector<Requirement> &requirements)
{
    return static_cast<std::size_t>(std::count_if(requirements.begin(), requirements.end(), [](const Requirement &item) {
        return !resolved(item);
    }));
}

std::optional<std::int64_t> integer_value(const Requirement &requirement)
{
    if (const auto *value = std::get_if<std::int64_t>(&requirement.value)) {
        return *value;
    }
    return std::nullopt;
}

void validate_description_layout(const std::vector<Requirement> &requirements, std::vector<std::string> &errors)
{
    std::map<std::string_view, std::int64_t> values;
    for (const auto &requirement : requirements) {
        if (const auto value = integer_value(requirement)) {
            values.emplace(requirement.name, *value);
        }
    }
    constexpr std::array<std::string_view, 4> names = {
        "ES_PLUGIN_OFF_DESCRIPTION", "ES_DESCRIPTION_SIZE", "ES_PLUGIN_IMPL_SIZE", "ES_DESCRIPTION_ALIGN"};
    for (const auto name : names) {
        if (!values.contains(name)) {
            return;
        }
    }
    const auto offset = values.at("ES_PLUGIN_OFF_DESCRIPTION");
    const auto size = values.at("ES_DESCRIPTION_SIZE");
    const auto impl_size = values.at("ES_PLUGIN_IMPL_SIZE");
    const auto alignment = values.at("ES_DESCRIPTION_ALIGN");
    if (offset < 0 || size < 0 || impl_size < 0 || offset + size > impl_size) {
        errors.emplace_back("description layout invariant failed: PluginDescription exceeds MinimalPlugin");
    }
    if (offset + size != impl_size) {
        errors.emplace_back("description layout invariant failed: PluginDescription does not end at MinimalPlugin size");
    }
    if (alignment <= 0 || offset % alignment != 0) {
        errors.emplace_back("description layout invariant failed: PluginDescription offset is misaligned");
    }
}

}  // namespace

ProbeReport collect_report(const endstone::Plugin *plugin, bool live_context, std::string_view run_id,
                           std::string_view output_path)
{
    ProbeReport result;
    result.run_id = std::string(run_id);
    result.output_path = std::string(output_path);
    result.probe_loaded = live_context && plugin != nullptr;
    result.clean_start = environment_clean_start();

    const auto validation = validate_registry();
    if (!validation.valid) {
        result.errors.emplace_back("registry validation failed: " + validation.error);
        return result;
    }
    if (result.run_id.empty()) {
        result.errors.emplace_back("ABI_PROBE_RUN_ID is missing");
    }
    if (result.output_path.empty()) {
        result.errors.emplace_back("ABI_PROBE_OUTPUT resolved to an empty path");
    }
    if (live_context && plugin == nullptr) {
        result.errors.emplace_back("live context has no loaded Plugin object");
    }

    for (const auto &entry : registry()) {
        if (!applies_to_current_platform(entry.platform)) {
            continue;
        }

        Requirement requirement;
        requirement.name = std::string(entry.name);
        requirement.value = std::monostate{};
        requirement.provenance = Provenance::Unresolved;
        requirement.category = std::string(requirement_category(entry.name));
        requirement.contract_identity = std::string(requirement_contract_identity(entry.name));
        requirement.runtime_required = requirement_runtime_required(entry.name);
        requirement.method = std::string(entry.expression);
        requirement.evidence = "no exact measurement was produced";
        requirement.dependencies = split_dependencies(entry.dependencies);

        if (entry.name == "ES_API_VERSION") {
#ifdef ENDSTONE_API_VERSION
            requirement.value = std::string(ENDSTONE_API_VERSION);
            requirement.provenance = Provenance::CompileMeasured;
            requirement.method = "compile-time SDK API version expression";
            requirement.evidence = "ENDSTONE_API_VERSION from the loaded SDK headers";
#else
            requirement.evidence = "ENDSTONE_API_VERSION is not defined";
#endif
            result.requirements.emplace_back(std::move(requirement));
            continue;
        }

        const Fact fact = measure_fact(entry.name, plugin, live_context);
        if (fact.value && fact.provenance != Provenance::Unresolved) {
            requirement.value = *fact.value;
            requirement.method = std::string(fact.method);
            requirement.evidence = std::string(fact.evidence);
            if (is_runtime_object(entry.name)) {
                requirement.provenance = Provenance::RuntimeObject;
            }
            else if (is_derived(entry.name)) {
                requirement.provenance = Provenance::RuntimeDerived;
            }
            else if (fact.provenance == Provenance::RuntimeDerived) {
                // Some exact compiler/layout probes calculate their result from
                // private or member-pointer intermediates which are not ABI
                // requirements themselves.  They are primary RUNTIME_PROBE
                // evidence, not a dependency-addressable final derivation.
                requirement.provenance = Provenance::RuntimeProbe;
            }
            else {
                requirement.provenance = fact.provenance;
            }
        }
        else {
            requirement.method = std::string(fact.method);
            requirement.evidence = std::string(fact.evidence);
            if (requirement.evidence.empty()) {
                requirement.evidence = "unresolved: " + std::string(entry.expression);
            }
        }
        result.requirements.emplace_back(std::move(requirement));
    }

    std::map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < result.requirements.size(); ++index) {
        indices.emplace(result.requirements[index].name, index);
    }
    for (auto &requirement : result.requirements) {
        if (!resolved(requirement) || requirement.dependencies.empty()) {
            continue;
        }
        for (const auto &dependency : requirement.dependencies) {
            const auto dependency_index = indices.find(dependency);
            if (dependency_index == indices.end() || !resolved(result.requirements[dependency_index->second])) {
                requirement.value = std::monostate{};
                requirement.provenance = Provenance::Unresolved;
                requirement.evidence += "; declared dependency is unresolved: " + dependency;
                result.errors.emplace_back(requirement.name + " dependency unresolved: " + dependency);
                break;
            }
        }
    }

    validate_description_layout(result.requirements, result.errors);

    if (!result.probe_loaded) {
        result.errors.emplace_back("live plugin context is not available");
    }
    if (!environment_metadata_complete()) {
        result.errors.emplace_back("required Endstone/compiler/source/BDS metadata is incomplete");
    }
    if (!result.clean_start) {
        result.errors.emplace_back("ABI_PROBE_CLEAN_START=1 was not explicitly provided");
    }

    result.complete = result.probe_loaded && result.clean_start && environment_metadata_complete() &&
                      result.errors.empty() && unresolved_count(result.requirements) == 0;
    return result;
}

SelfTestResult run_measurement_self_test()
{
    SelfTestResult result;
    const auto validation = validate_registry();
    if (!validation.valid) {
        result.failures.emplace_back("registry validation failed: " + validation.error);
        return result;
    }

    struct TestFact {
        std::string name;
        Fact fact;
        std::vector<std::string> dependencies;
    };
    std::vector<TestFact> facts;
    for (const auto &entry : registry()) {
        if (!applies_to_current_platform(entry.platform) || is_runtime_object(entry.name)) {
            continue;
        }
        if (entry.name == "ES_API_VERSION") {
#ifdef ENDSTONE_API_VERSION
            if (std::string_view(ENDSTONE_API_VERSION).empty()) {
                ++result.unresolved;
                result.failures.emplace_back("ES_API_VERSION: SDK API version is empty");
            }
#else
            ++result.unresolved;
            result.failures.emplace_back("ES_API_VERSION: ENDSTONE_API_VERSION is not defined");
#endif
            continue;
        }
        TestFact current{std::string(entry.name),
                         measure_fact(entry.name, nullptr, false),
                         split_dependencies(entry.dependencies)};
        if (!current.fact.value || current.fact.provenance == Provenance::Unresolved) {
            ++result.unresolved;
            result.failures.emplace_back(current.name + ": " + std::string(current.fact.evidence));
        }
        facts.emplace_back(std::move(current));
    }

    std::map<std::string, const TestFact *> by_name;
    for (const auto &fact : facts) {
        by_name.emplace(fact.name, &fact);
    }
    for (const auto &fact : facts) {
        if (!fact.fact.value || fact.fact.provenance == Provenance::Unresolved) {
            continue;
        }
        for (const auto &dependency : fact.dependencies) {
            const auto found = by_name.find(dependency);
            if (found == by_name.end() || !found->second->fact.value ||
                found->second->fact.provenance == Provenance::Unresolved) {
                result.failures.emplace_back(fact.name + ": dependency unresolved: " + dependency);
            }
        }
    }
    result.valid = result.failures.empty();
    return result;
}

}  // namespace abi_probe

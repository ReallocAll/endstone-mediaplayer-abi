#include "abi_probe.h"
#include "runtime_probe.h"
#include "version.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <string>
#include <string_view>

namespace {

std::string environment_value(const char *name)
{
    if (const char *value = std::getenv(name); value != nullptr) {
        return value;
    }
    return {};
}

std::string output_path()
{
    auto value = environment_value("ABI_PROBE_OUTPUT");
    if (!value.empty()) {
        return value;
    }
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    return error ? std::string{"abi-runtime.json"} : (current / "abi-runtime.json").string();
}

std::size_t unresolved_count(const abi_probe::ProbeReport &report)
{
    return static_cast<std::size_t>(std::count_if(report.requirements.begin(), report.requirements.end(),
                                                   [](const abi_probe::Requirement &item) {
                                                       return std::holds_alternative<std::monostate>(item.value) ||
                                                              item.provenance == abi_probe::Provenance::Unresolved;
                                                   }));
}

}  // namespace

void AbiProbePlugin::onLoad()
{
    load_attempted_ = true;
    const auto run_id = environment_value("ABI_PROBE_RUN_ID");
    const auto path = output_path();
    try {
        report_ = abi_probe::collect_report(this, false, run_id, path);
    }
    catch (const std::exception &error) {
        report_.run_id = run_id;
        report_.output_path = path;
        report_.complete = false;
        report_.probe_loaded = false;
        report_.requirements.clear();
        (void)error;
    }
}

void AbiProbePlugin::onEnable()
{
    if (completed_) {
        return;
    }

    const auto run_id = environment_value("ABI_PROBE_RUN_ID");
    const auto path = output_path();
    if (run_id.empty()) {
        getLogger().error("ABI_PROBE_FAILED missing ABI_PROBE_RUN_ID");
        return;
    }

    try {
        report_ = abi_probe::collect_report(this, true, run_id, path);
        if (!report_.complete) {
            getLogger().error("ABI_PROBE_FAILED unresolved_count={} errors={}", unresolved_count(report_), report_.errors.size());
            for (const auto &error : report_.errors) {
                getLogger().error("ABI_PROBE_FAILED detail={}", error);
            }
            return;
        }
        if (!abi_probe::write_report_atomically(report_)) {
            getLogger().error("ABI_PROBE_FAILED unable to atomically write report path={}", path);
            return;
        }
        completed_ = true;
        getLogger().info("ABI_PROBE_COMPLETE run_id={} report={}", run_id, path);
    }
    catch (const std::exception &error) {
        getLogger().error("ABI_PROBE_FAILED exception={}", error.what());
    }
}

void AbiProbePlugin::onDisable()
{
    // The report is intentionally emitted once during onEnable. The harness
    // owns server shutdown and no Endstone API is touched during disable.
}

ENDSTONE_PLUGIN("abi_probe", ENDSTONE_ABI_PROBE_VERSION, AbiProbePlugin)
{
    prefix = "ABIProbe";
    description = "Runtime-first Endstone ABI measurement probe";
    authors = {"Endstone MediaPlayer ABI Probe"};
}

#pragma once

#include <cstddef>
#include <vector>

#include "report.h"

namespace endstone {
class Plugin;
}

namespace abi_probe {

ProbeReport collect_report(const endstone::Plugin *plugin, bool live_context, std::string_view run_id,
                           std::string_view output_path);

struct SelfTestResult {
    bool valid{false};
    std::size_t unresolved{0};
    std::vector<std::string> failures;
};

SelfTestResult run_measurement_self_test();

}  // namespace abi_probe

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "report.h"

namespace endstone {
class Plugin;
}

namespace abi_probe {

struct Fact {
    std::optional<std::int64_t> value;
    std::string_view method;
    std::string_view evidence;
    Provenance provenance{Provenance::RuntimeProbe};
};

Fact measure_layout(std::string_view name, const endstone::Plugin *live_plugin, bool live_context);

}  // namespace abi_probe

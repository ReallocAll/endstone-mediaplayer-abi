#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace abi_probe {

enum class Platform {
    Windows,
    Linux,
    Both,
};

struct RegistryEntry {
    std::string_view name;
    Platform platform;
    std::string_view expression;
    std::string_view dependencies;
};

std::span<const RegistryEntry> registry();
bool applies_to_current_platform(Platform platform);

struct RegistryValidation {
    bool valid{false};
    std::string error;
};

[[nodiscard]] RegistryValidation validate_registry();

}  // namespace abi_probe

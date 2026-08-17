#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace abi_probe {

enum class Provenance {
    RuntimeObject,
    RuntimeProbe,
    RuntimeDerived,
    CompileMeasured,
    StaticVerified,
    Unresolved,
};

using Value = std::variant<std::monostate, std::int64_t, std::string>;

struct Requirement {
    std::string name;
    Value value;
    Provenance provenance{Provenance::Unresolved};
    std::string category;
    std::string contract_identity;
    bool runtime_required{false};
    std::string method;
    std::string evidence;
    std::vector<std::string> dependencies;
};

struct ProbeReport {
    std::string run_id;
    std::string output_path;
    bool complete{false};
    bool probe_loaded{false};
    bool clean_start{false};
    std::vector<Requirement> requirements;
    std::vector<std::string> errors;
};

[[nodiscard]] std::string environment_platform();
[[nodiscard]] std::string environment_compiler();
[[nodiscard]] std::string environment_stdlib();
[[nodiscard]] std::string environment_compiler_version();
[[nodiscard]] std::string environment_target();
[[nodiscard]] std::string environment_stdlib_version();
[[nodiscard]] std::string environment_abi_flags();
[[nodiscard]] bool environment_clean_start();
[[nodiscard]] bool environment_metadata_complete();

bool write_report_atomically(const ProbeReport &report);

}  // namespace abi_probe

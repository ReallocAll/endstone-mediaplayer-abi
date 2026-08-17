#include "report.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string_view>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <endstone/version.h>

namespace abi_probe {
namespace {

void json_escape(std::ostream &out, std::string_view value)
{
    out.put('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20) {
                out << "\\u00" << hex[(byte >> 4) & 0x0F] << hex[byte & 0x0F];
            }
            else {
                out.put(static_cast<char>(byte));
            }
            break;
        }
    }
    out.put('"');
}

void json_string_or_null(std::ostream &out, std::string_view value)
{
    if (value.empty()) {
        out << "null";
    }
    else {
        json_escape(out, value);
    }
}

void json_value(std::ostream &out, const Value &value)
{
    std::visit(
        [&out](const auto &item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out << "null";
            }
            else if constexpr (std::is_same_v<T, std::int64_t>) {
                out << item;
            }
            else {
                json_escape(out, item);
            }
        },
        value);
}

[[nodiscard]] std::string provenance_name(Provenance provenance)
{
    switch (provenance) {
    case Provenance::RuntimeObject: return "RUNTIME_OBJECT";
    case Provenance::RuntimeProbe: return "RUNTIME_PROBE";
    case Provenance::RuntimeDerived: return "RUNTIME_DERIVED";
    case Provenance::CompileMeasured: return "COMPILE_MEASURED";
    case Provenance::StaticVerified: return "STATIC_VERIFIED";
    case Provenance::Unresolved: return "UNRESOLVED";
    }
    return "UNRESOLVED";
}

[[nodiscard]] std::string macro_string(std::string_view value)
{
    return value.empty() ? std::string{} : std::string(value);
}

[[nodiscard]] std::string endstone_version()
{
#ifdef ENDSTONE_VERSION
    return macro_string(ENDSTONE_VERSION);
#else
    return {};
#endif
}

[[nodiscard]] std::string api_version()
{
#ifdef ENDSTONE_API_VERSION
    return macro_string(ENDSTONE_API_VERSION);
#else
    return {};
#endif
}

[[nodiscard]] std::string source_ref()
{
#ifdef ABI_PROBE_SOURCE_REF
    return macro_string(ABI_PROBE_SOURCE_REF);
#else
    return {};
#endif
}

[[nodiscard]] std::string source_commit()
{
#ifdef ABI_PROBE_SOURCE_COMMIT
    return macro_string(ABI_PROBE_SOURCE_COMMIT);
#else
    return {};
#endif
}

[[nodiscard]] std::string bds_version()
{
#ifdef ABI_PROBE_BDS_VERSION
    return macro_string(ABI_PROBE_BDS_VERSION);
#else
    return {};
#endif
}

bool replace_file(const std::filesystem::path &temporary, const std::filesystem::path &destination)
{
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

}  // namespace

std::string environment_platform()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return {};
#endif
}

std::string environment_compiler()
{
#if defined(_MSC_VER) && defined(__clang__)
    return "clang-cl";
#elif defined(__clang__)
    return "clang";
#elif defined(_MSC_VER)
    return "msvc";
#else
    return {};
#endif
}

std::string environment_stdlib()
{
#if defined(_LIBCPP_VERSION)
    return "libc++";
#elif defined(_MSVC_STL_VERSION)
    return "msvc-stl";
#else
    return {};
#endif
}

std::string environment_compiler_version()
{
#if defined(__clang__)
    std::ostringstream version;
    version << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#if defined(_MSC_VER)
    version << " (msvc-compatible " << _MSC_VER << ')';
#endif
    return version.str();
#elif defined(_MSC_VER)
    return std::to_string(_MSC_VER);
#else
    return {};
#endif
}

std::string environment_target()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "x86_64-pc-windows-msvc";
#elif defined(__linux__) && defined(__x86_64__)
    return "x86_64-pc-linux-gnu";
#else
    return {};
#endif
}

std::string environment_stdlib_version()
{
#if defined(_MSVC_STL_VERSION)
    std::string value = std::to_string(_MSVC_STL_VERSION);
#if defined(_MSVC_STL_UPDATE)
    value += "." + std::to_string(_MSVC_STL_UPDATE);
#endif
    return value;
#elif defined(_LIBCPP_VERSION)
    return std::to_string(_LIBCPP_VERSION);
#else
    return {};
#endif
}

std::string environment_abi_flags()
{
    std::ostringstream flags;
#if defined(_MSVC_STL_VERSION)
    flags << "_MSVC_STL_VERSION=" << _MSVC_STL_VERSION << ';';
#if defined(_MSVC_STL_UPDATE)
    flags << "_MSVC_STL_UPDATE=" << _MSVC_STL_UPDATE << ';';
#endif
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    flags << "_ITERATOR_DEBUG_LEVEL=" << _ITERATOR_DEBUG_LEVEL << ';';
#endif
#if defined(_HAS_ITERATOR_DEBUGGING)
    flags << "_HAS_ITERATOR_DEBUGGING=" << _HAS_ITERATOR_DEBUGGING << ';';
#endif
#if defined(_LIBCPP_ABI_VERSION)
    flags << "_LIBCPP_ABI_VERSION=" << _LIBCPP_ABI_VERSION << ';';
#endif
#if defined(_LIBCPP_ABI_MICROSOFT)
    flags << "_LIBCPP_ABI_MICROSOFT=1;";
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI)
    flags << "_GLIBCXX_USE_CXX11_ABI=" << _GLIBCXX_USE_CXX11_ABI << ';';
#endif
    flags << "__cplusplus=" << __cplusplus;
    return flags.str();
}

bool environment_clean_start()
{
    const char *value = std::getenv("ABI_PROBE_CLEAN_START");
    return value != nullptr && std::string_view(value) == "1";
}

bool environment_metadata_complete()
{
    return !environment_platform().empty() && !environment_target().empty() &&
           !environment_compiler().empty() && !environment_compiler_version().empty() &&
           !environment_stdlib().empty() && !environment_stdlib_version().empty() &&
           !environment_abi_flags().empty() && !endstone_version().empty() && !api_version().empty() &&
           !source_ref().empty() && !source_commit().empty() && !bds_version().empty();
}

bool write_report_atomically(const ProbeReport &report)
{
    if (!report.complete || report.output_path.empty()) {
        return false;
    }

    const std::filesystem::path destination(report.output_path);
    const auto parent = destination.parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    auto requirements = report.requirements;
    std::stable_sort(requirements.begin(), requirements.end(), [](const Requirement &left, const Requirement &right) {
        return left.name < right.name;
    });

    const auto temporary = std::filesystem::path(report.output_path + ".tmp");
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    out << "{\n  \"schema_version\":1,\n  \"run_id\":";
    json_string_or_null(out, report.run_id);
    out << ",\n  \"complete\":" << (report.complete ? "true" : "false");
    out << ",\n  \"environment\":{\n";
    out << "    \"platform\":";
    json_string_or_null(out, environment_platform());
    out << ",\n    \"arch\":\"x86_64\",\n    \"pointer_size\":" << sizeof(void *) << ",\n";
    out << "    \"endstone_runtime_version\":";
    json_string_or_null(out, endstone_version());
    out << ",\n    \"api_version\":";
    json_string_or_null(out, api_version());
    out << ",\n    \"source_ref\":";
    json_string_or_null(out, source_ref());
    out << ",\n    \"source_commit\":";
    json_string_or_null(out, source_commit());
    out << ",\n    \"compiler\":";
    json_string_or_null(out, environment_compiler());
    out << ",\n    \"compiler_version\":";
    json_string_or_null(out, environment_compiler_version());
    out << ",\n    \"target\":";
    json_string_or_null(out, environment_target());
    out << ",\n    \"stdlib\":";
    json_string_or_null(out, environment_stdlib());
    out << ",\n    \"stdlib_version\":";
    json_string_or_null(out, environment_stdlib_version());
    out << ",\n    \"abi_flags\":";
    json_string_or_null(out, environment_abi_flags());
    out << ",\n    \"bds_version\":";
    json_string_or_null(out, bds_version());
    out << ",\n    \"probe_loaded\":" << (report.probe_loaded ? "true" : "false");
    out << ",\n    \"clean_start\":" << (report.clean_start ? "true" : "false") << "\n  },\n  \"requirements\":[\n";

    for (std::size_t index = 0; index < requirements.size(); ++index) {
        const auto &requirement = requirements[index];
        out << "    {\"name\":";
        json_escape(out, requirement.name);
        out << ",\"value\":";
        json_value(out, requirement.value);
        out << ",\"category\":";
        json_escape(out, requirement.category);
        out << ",\"contract_identity\":";
        json_escape(out, requirement.contract_identity);
        out << ",\"runtime_required\":" << (requirement.runtime_required ? "true" : "false");
        out << ",\"provenance\":";
        json_escape(out, provenance_name(requirement.provenance));
        out << ",\"method\":";
        json_escape(out, requirement.method);
        out << ",\"evidence\":";
        json_escape(out, requirement.evidence);
        if (!requirement.dependencies.empty()) {
            out << ",\"dependencies\":[";
            for (std::size_t dependency = 0; dependency < requirement.dependencies.size(); ++dependency) {
                if (dependency != 0) {
                    out.put(',');
                }
                json_escape(out, requirement.dependencies[dependency]);
            }
            out.put(']');
        }
        out << "}" << (index + 1 == requirements.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    out.flush();
    if (!out) {
        out.close();
        std::filesystem::remove(temporary, error);
        return false;
    }
    out.close();

    if (!replace_file(temporary, destination)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

}  // namespace abi_probe

cmake_minimum_required(VERSION 3.21)

project(endstone_abi_probe VERSION 1.0.0 LANGUAGES CXX)

string(TOUPPER "${PROJECT_NAME}" PROJECT_NAME_UPPER)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENDSTONE_REF "v0.11.8" CACHE STRING "Endstone git ref used by the probe")
set(ENDSTONE_SOURCE_DIR "" CACHE PATH "Optional read-only Endstone source tree")
set(BDS_VERSION "" CACHE STRING "BDS version supplied by the runtime harness")
set(ABI_PROBE_SOURCE_REF "" CACHE STRING "Source ref to record in the runtime report")
set(ABI_PROBE_SOURCE_COMMIT "" CACHE STRING "Source commit to record in the runtime report")
set(ABI_PROBE_EXTRA_INCLUDE_DIRS "" CACHE STRING "Additional read-only dependency include roots")
set(ABI_PROBE_CONAN_CACHE "" CACHE PATH "Optional read-only Conan cache containing Endstone dependencies")

if(UNIX AND NOT APPLE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Linux builds require Clang for Endstone ABI compatibility")
    endif()
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

if(WIN32 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Windows builds require clang-cl for Endstone ABI compatibility")
endif()
if(WIN32 AND DEFINED CMAKE_CXX_COMPILER_FRONTEND_VARIANT AND
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Windows builds require the clang-cl (MSVC frontend) driver")
endif()
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The ABI probe supports x86_64 only (8-byte pointers)")
endif()

include(FetchContent)
if(ENDSTONE_SOURCE_DIR)
    if(NOT IS_DIRECTORY "${ENDSTONE_SOURCE_DIR}")
        message(FATAL_ERROR "ENDSTONE_SOURCE_DIR is not a directory: ${ENDSTONE_SOURCE_DIR}")
    endif()
    FetchContent_Declare(endstone SOURCE_DIR "${ENDSTONE_SOURCE_DIR}")
else()
    FetchContent_Declare(
        endstone
        GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
        GIT_TAG "${ENDSTONE_REF}"
    )
endif()
FetchContent_MakeAvailable(endstone)

# Endstone's internal record headers include the same dependency headers as
# its bedrock/core targets.  The public plugin target does not expose those
# include directories, so resolve them explicitly for the read-only probe TU.
file(GLOB _abi_probe_gsl_roots "$ENV{USERPROFILE}/.conan2/p/ms-gs*/p/include")
file(GLOB _abi_probe_json_roots "$ENV{USERPROFILE}/.conan2/p/nloh*/p/include")
file(GLOB _abi_probe_gsl_home_roots "$ENV{HOME}/.conan2/p/ms-gs*/p/include")
file(GLOB _abi_probe_json_home_roots "$ENV{HOME}/.conan2/p/nloh*/p/include")
list(APPEND _abi_probe_gsl_roots ${_abi_probe_gsl_home_roots})
list(APPEND _abi_probe_json_roots ${_abi_probe_json_home_roots})
file(GLOB _abi_probe_conan_roots "$ENV{USERPROFILE}/.conan2/p/*/p/include")
file(GLOB _abi_probe_conan_home_roots "$ENV{HOME}/.conan2/p/*/p/include")
list(APPEND _abi_probe_conan_roots ${_abi_probe_conan_home_roots})
file(GLOB _abi_probe_conan_build_roots "$ENV{USERPROFILE}/.conan2/p/*/*/p/include" "$ENV{USERPROFILE}/.conan2/p/*/*/*/src/Source")
file(GLOB _abi_probe_conan_home_build_roots "$ENV{HOME}/.conan2/p/*/*/p/include" "$ENV{HOME}/.conan2/p/*/*/*/src/Source")
list(APPEND _abi_probe_conan_build_roots ${_abi_probe_conan_home_build_roots})
list(APPEND _abi_probe_conan_roots ${_abi_probe_conan_build_roots})
if(ABI_PROBE_CONAN_CACHE)
    file(GLOB _abi_probe_explicit_conan_roots
        "${ABI_PROBE_CONAN_CACHE}/p/*/p/include"
        "${ABI_PROBE_CONAN_CACHE}/p/*/*/p/include"
        "${ABI_PROBE_CONAN_CACHE}/p/*/*/*/src/Source")
    list(APPEND _abi_probe_conan_roots ${_abi_probe_explicit_conan_roots})
endif()
file(GLOB _abi_probe_endstone_conan_roots "${CMAKE_CURRENT_SOURCE_DIR}/../endstone/.conan2/p/*/p/include")
file(GLOB _abi_probe_endstone_conan_build_roots
    "${CMAKE_CURRENT_SOURCE_DIR}/../endstone/.conan2/p/*/*/p/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../endstone/.conan2/p/*/*/*/src/Source")
list(APPEND _abi_probe_conan_roots ${_abi_probe_endstone_conan_roots})
list(APPEND _abi_probe_conan_roots ${_abi_probe_endstone_conan_build_roots})
file(GLOB _abi_probe_endstone_gsl_roots "${CMAKE_CURRENT_SOURCE_DIR}/../endstone/.conan2/p/ms-gs*/p/include")
file(GLOB _abi_probe_endstone_json_roots "${CMAKE_CURRENT_SOURCE_DIR}/../endstone/.conan2/p/nloh*/p/include")
list(APPEND _abi_probe_gsl_roots ${_abi_probe_endstone_gsl_roots})
list(APPEND _abi_probe_json_roots ${_abi_probe_endstone_json_roots})
find_path(ABI_PROBE_GSL_INCLUDE_DIR gsl/gsl PATHS ${_abi_probe_gsl_roots})
find_path(ABI_PROBE_JSON_INCLUDE_DIR nlohmann/json.hpp PATHS ${_abi_probe_json_roots})
if(NOT ABI_PROBE_GSL_INCLUDE_DIR OR NOT ABI_PROBE_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "Internal Endstone probes require gsl/gsl and nlohmann/json.hpp include roots")
endif()

# Resolve the exact source identity before any target is compiled.  A supplied
# source tree is read-only input: a short commit is accepted only after it is
# resolved and compared with that tree's actual HEAD.
execute_process(
    COMMAND git -C "${endstone_SOURCE_DIR}" rev-parse --verify HEAD
    RESULT_VARIABLE _endstone_git_result
    OUTPUT_VARIABLE _endstone_git_commit_detected
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _endstone_git_result EQUAL 0 OR NOT _endstone_git_commit_detected)
    message(FATAL_ERROR "Unable to determine exact Endstone source HEAD: ${endstone_SOURCE_DIR}")
endif()
if(ABI_PROBE_SOURCE_COMMIT)
    execute_process(
        COMMAND git -C "${endstone_SOURCE_DIR}" rev-parse --verify "${ABI_PROBE_SOURCE_COMMIT}^{commit}"
        RESULT_VARIABLE _endstone_requested_commit_result
        OUTPUT_VARIABLE _endstone_requested_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _endstone_requested_commit_result EQUAL 0 OR
       NOT "${_endstone_requested_commit}" STREQUAL "${_endstone_git_commit_detected}")
        message(FATAL_ERROR
            "ABI_PROBE_SOURCE_COMMIT does not match Endstone HEAD: requested=${ABI_PROBE_SOURCE_COMMIT} "
            "resolved=${_endstone_requested_commit} detected=${_endstone_git_commit_detected}")
    endif()
endif()
set(ABI_PROBE_SOURCE_COMMIT "${_endstone_git_commit_detected}")

# Record the exact fetched/source-tree tag when available. An empty ref is
# deliberately retained as null in the report; it is never replaced with a
# guessed identity.
if(NOT ABI_PROBE_SOURCE_REF)
    if(ENDSTONE_SOURCE_DIR)
        execute_process(
            COMMAND git -C "${endstone_SOURCE_DIR}" describe --tags --exact-match HEAD
            RESULT_VARIABLE _endstone_tag_result
            OUTPUT_VARIABLE _endstone_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_endstone_tag_result EQUAL 0)
            set(ABI_PROBE_SOURCE_REF "${_endstone_tag}")
        endif()
    else()
        set(ABI_PROBE_SOURCE_REF "${ENDSTONE_REF}")
    endif()
endif()

set(_abi_probe_sources
    src/plugin.cpp
    src/registry.cpp
    src/vtable_probe.cpp
    src/layout_probe.cpp
    src/internal_probe.cpp
    src/stl_probe.cpp
    src/runtime_probe.cpp
    src/report.cpp)

endstone_add_plugin(abi_probe ${_abi_probe_sources})
target_include_directories(abi_probe PRIVATE
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${endstone_SOURCE_DIR}/include
    ${endstone_SOURCE_DIR}/src
    ${endstone_BINARY_DIR}/include
    ${endstone_BINARY_DIR}/include/generated
    ${ABI_PROBE_GSL_INCLUDE_DIR}
    ${ABI_PROBE_JSON_INCLUDE_DIR}
    ${ABI_PROBE_EXTRA_INCLUDE_DIRS}
    ${_abi_probe_conan_roots})
target_compile_definitions(abi_probe PRIVATE
    ENTT_SPARSE_PAGE=2048
    ENTT_PACKED_PAGE=128
    ENTT_NO_MIXIN
    ABI_PROBE_SOURCE_REF="${ABI_PROBE_SOURCE_REF}"
    ABI_PROBE_SOURCE_COMMIT="${ABI_PROBE_SOURCE_COMMIT}"
    ABI_PROBE_BDS_VERSION="${BDS_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(WIN32)
        target_compile_options(abi_probe PRIVATE /clang:-Xclang /clang:-fno-access-control)
    else()
        target_compile_options(abi_probe PRIVATE -Xclang -fno-access-control)
    endif()
endif()
set_target_properties(abi_probe PROPERTIES OUTPUT_NAME "abi_probe")

# The internal block record header contains inline static identifiers whose
# constructors are defined by Endstone's bedrock sources.  Compile those two
# exact, read-only source files into an ABI-tree sidecar library so the probe
# does not invent stubs or modify/build Endstone in place.
set(_abi_probe_internal_sources
    "${endstone_SOURCE_DIR}/src/bedrock/core/string/string_hash.cpp"
    "${endstone_SOURCE_DIR}/src/bedrock/resources/base_game_version.cpp")
foreach(_abi_probe_internal_source IN LISTS _abi_probe_internal_sources)
    if(NOT EXISTS "${_abi_probe_internal_source}")
        message(FATAL_ERROR "Required read-only Endstone internal source is missing: ${_abi_probe_internal_source}")
    endif()
endforeach()
add_library(abi_probe_endstone_internal STATIC ${_abi_probe_internal_sources})
target_include_directories(abi_probe_endstone_internal PRIVATE
    ${endstone_SOURCE_DIR}/include
    ${endstone_SOURCE_DIR}/src
    ${endstone_BINARY_DIR}/include
    ${endstone_BINARY_DIR}/include/generated
    ${ABI_PROBE_GSL_INCLUDE_DIR}
    ${ABI_PROBE_JSON_INCLUDE_DIR}
    ${ABI_PROBE_EXTRA_INCLUDE_DIRS}
    ${_abi_probe_conan_roots})
target_compile_definitions(abi_probe_endstone_internal PRIVATE
    ENTT_SPARSE_PAGE=2048
    ENTT_PACKED_PAGE=128
    ENTT_NO_MIXIN)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(WIN32)
        target_compile_options(abi_probe_endstone_internal PRIVATE /clang:-Xclang /clang:-fno-access-control)
    else()
        target_compile_options(abi_probe_endstone_internal PRIVATE -Xclang -fno-access-control)
    endif()
endif()
target_link_libraries(abi_probe PRIVATE abi_probe_endstone_internal)

add_executable(abi_probe_self_test
    src/self_test.cpp
    src/registry.cpp
    src/vtable_probe.cpp
    src/layout_probe.cpp
    src/internal_probe.cpp
    src/stl_probe.cpp
    src/runtime_probe.cpp
    src/report.cpp)
target_include_directories(abi_probe_self_test PRIVATE
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${endstone_SOURCE_DIR}/include
    ${endstone_SOURCE_DIR}/src
    ${endstone_BINARY_DIR}/include
    ${endstone_BINARY_DIR}/include/generated
    ${ABI_PROBE_GSL_INCLUDE_DIR}
    ${ABI_PROBE_JSON_INCLUDE_DIR}
    ${ABI_PROBE_EXTRA_INCLUDE_DIRS}
    ${_abi_probe_conan_roots})
target_compile_definitions(abi_probe_self_test PRIVATE
    ENTT_SPARSE_PAGE=2048
    ENTT_PACKED_PAGE=128
    ENTT_NO_MIXIN
    ABI_PROBE_SOURCE_REF="${ABI_PROBE_SOURCE_REF}"
    ABI_PROBE_SOURCE_COMMIT="${ABI_PROBE_SOURCE_COMMIT}"
    ABI_PROBE_BDS_VERSION="${BDS_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(WIN32)
        target_compile_options(abi_probe_self_test PRIVATE /clang:-Xclang /clang:-fno-access-control)
    else()
        target_compile_options(abi_probe_self_test PRIVATE -Xclang -fno-access-control)
    endif()
endif()
target_link_libraries(abi_probe_self_test PRIVATE abi_probe_endstone_internal)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME abi_probe_measurements COMMAND abi_probe_self_test)
endif()

configure_file(include/version.h.in include/version.h)

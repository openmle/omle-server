cmake_minimum_required(VERSION 3.21)
project(omle_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Any translation unit that pulls in <windows.h> gets min and max as
# function-like macros unless NOMINMAX is set. gRPC's headers are what surface
# this: grpc/event_engine/memory_request.h declares
#
#     size_t min() const { return min_; }
#
# and `min()` with no arguments does not expand, so MSVC reports
# "C2059: syntax error: ')'" inside a gRPC header that is perfectly valid.
# The parse then derails mid-namespace and grpc_event_engine is never closed,
# so every later include lands inside it — which is why the follow-on errors
# name types like grpc_event_engine::absl::lts_20260107::Status and look like
# an Abseil problem rather than a preprocessor one.
#
# Set before add_subdirectory below so the tests inherit it too. omle-runtime's
# CMakeLists sets the same pair for its own targets; directory scope does not
# carry upward, so this is not redundant.
if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# -march=native is deliberately absent: it bakes the build machine's ISA into
# the binary, so a server image built on one host can fault on another. There is
# no hand-written SIMD in this project to keep alive either -- the intrinsics
# live in omle-runtime, whose own CMakeLists sets an explicit AVX2+FMA baseline
# on x86. That matters here because this file calls add_compile_options before
# add_subdirectory below, so anything set here is inherited by the runtime
# subproject and would override its baseline.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-O3 -DNDEBUG)
    elseif(MSVC)
        add_compile_options(/O2 /DNDEBUG)
    endif()
endif()

# ── Self-contained build (opt-in) ──────────────────────────────────────────
# A default build links 112 shared libraries — 79 of them Abseil, arriving
# transitively through gRPC — so the binary only runs where all of them are
# installed at the same versions, and the deployment unit is a container image.
# With this ON every dependency is compiled from pinned source as a static
# archive and folded into one executable that can simply be copied.
#
# Off by default: it builds gRPC (with protobuf, Abseil, BoringSSL, re2,
# c-ares, zlib) and Drogon from source, which takes far longer than finding
# them with find_package.
option(OMLE_SERVER_BUNDLE_DEPS
    "Build every dependency from source and link one self-contained binary" OFF)

# Deps come first in bundled mode: gRPC's superbuild defines protobuf and the
# absl:: targets, and omle-runtime below reuses them rather than calling
# find_package(Protobuf) and pulling in a second, system copy.
if(OMLE_SERVER_BUNDLE_DEPS)
    include(cmake/bundled_deps.cmake)
endif()

# ── omle-runtime ───────────────────────────────────────────────────────────
set(OMLE_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../omle-runtime"
    CACHE PATH "Path to omle-runtime source directory")

# Disable sub-projects we don't need
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory("${OMLE_RUNTIME_DIR}" omle_runtime_build EXCLUDE_FROM_ALL)

# ── Third-party deps ──────────────────────────────────────────────────────────
if(NOT OMLE_SERVER_BUNDLE_DEPS)
    include(cmake/deps.cmake)
endif()

# ── Proto code generation ─────────────────────────────────────────────────────
set(PROTO_FILE  "${CMAKE_CURRENT_SOURCE_DIR}/proto/open_inference_grpc.proto")
set(PROTO_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GRPC_GENDIR "${CMAKE_CURRENT_BINARY_DIR}/grpc_gen")
file(MAKE_DIRECTORY "${GRPC_GENDIR}")

add_custom_command(
    OUTPUT
        "${GRPC_GENDIR}/open_inference_grpc.pb.cc"
        "${GRPC_GENDIR}/open_inference_grpc.pb.h"
        "${GRPC_GENDIR}/open_inference_grpc.grpc.pb.cc"
        "${GRPC_GENDIR}/open_inference_grpc.grpc.pb.h"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
        "--proto_path=${PROTO_DIR}"
        "--cpp_out=${GRPC_GENDIR}"
        "--grpc_out=${GRPC_GENDIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating OIP gRPC C++ stubs"
    VERBATIM
)

add_library(oip_proto STATIC
    "${GRPC_GENDIR}/open_inference_grpc.pb.cc"
    "${GRPC_GENDIR}/open_inference_grpc.grpc.pb.cc"
)
target_include_directories(oip_proto PUBLIC "${GRPC_GENDIR}")
target_link_libraries(oip_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

# ── omle-server executable ─────────────────────────────────────────────────
add_executable(omle_server
    src/main.cpp
    src/model_registry.cpp
    src/oip_codec.cpp
    src/batcher.cpp
    src/metrics.cpp
    src/rest/handlers.cpp
    src/grpc/inference_service.cpp
)

target_include_directories(omle_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# omle_runtime_static vs omle_runtime: the latter is the SHARED library, which
# would leave libomleruntime.dylib next to the binary and defeat the point.
if(OMLE_SERVER_BUNDLE_DEPS)
    set(OMLE_RUNTIME_LINK_TARGET omle_runtime_static)
else()
    set(OMLE_RUNTIME_LINK_TARGET omle_runtime)
endif()

target_link_libraries(omle_server PRIVATE
    ${OMLE_RUNTIME_LINK_TARGET}
    oip_proto
    Drogon::Drogon
    simdjson::simdjson
    nlohmann_json::nlohmann_json
)

# MSVC does not understand the GCC/Clang warning flags.
target_compile_options(omle_server PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wno-unused-parameter>)

# ── Tests (optional) ──────────────────────────────────────────────────────────
option(BUILD_SERVER_TESTS "Build omle-server unit tests" OFF)
if(BUILD_SERVER_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Build type         : ${CMAKE_BUILD_TYPE}")
message(STATUS "omle-runtime    : ${OMLE_RUNTIME_DIR}")

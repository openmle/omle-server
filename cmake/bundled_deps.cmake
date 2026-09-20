# cmake/bundled_deps.cmake — build every dependency from pinned source as a
# static archive, so the server links into one self-contained executable.
#
# Enabled by -DOMLE_SERVER_BUNDLE_DEPS=ON. Off by default because this is a
# long build: gRPC alone carries Abseil, protobuf, BoringSSL, re2, c-ares and
# zlib as submodules, and all of them are compiled here.
#
# Why it exists: a default build links 112 shared libraries, 79 of them Abseil,
# pulled in transitively by gRPC. That binary only runs on a machine with the
# same versions of all of them in the same places, which makes the deployment
# unit a container image rather than a file you can copy.
#
# ORDER MATTERS. This file must be included BEFORE add_subdirectory() on
# omle-runtime. gRPC's superbuild defines protobuf::libprotobuf and the whole
# absl:: family; omle-runtime then reuses those targets instead of calling
# find_package(Protobuf), which would drag in a second, system protobuf and put
# two incompatible copies in one link.
#
# macOS note: a fully static executable is not possible there — Apple ships no
# static libSystem. "Self-contained" on macOS therefore means no dylibs beyond
# the OS ones (libSystem, libc++, CoreFoundation).

include(FetchContent)

# BUILD_SHARED_LIBS is the global every one of these projects keys off to decide
# archive vs shared. Set once here, and each dependency below builds static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Static archives are linked into an executable here, but omle-runtime also
# builds a SHARED libomleruntime from the same objects, and on Linux a
# non-PIC object cannot go into a shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# ── gRPC (brings protobuf, Abseil, BoringSSL, re2, c-ares, zlib) ─────────────
# Pinned to the version the unbundled build was developed against, so switching
# modes does not silently change the wire implementation.
set(OMLE_GRPC_VERSION "v1.84.0" CACHE STRING "gRPC tag for bundled builds")

set(gRPC_BUILD_TESTS                    OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL                        OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT               OFF CACHE BOOL "" FORCE)
# The codegen plugins are the one thing that must still be built: the proto
# rules below invoke grpc_cpp_plugin, and protoc, as host executables.
set(gRPC_BUILD_GRPC_CPP_PLUGIN          ON  CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_NODE_PLUGIN         OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN  OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PHP_PLUGIN          OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PYTHON_PLUGIN       OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_RUBY_PLUGIN         OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_CSHARP_PLUGIN       OFF CACHE BOOL "" FORCE)

# "module" makes gRPC build its third_party submodules rather than looking for
# system copies — which is the entire point of this file.
set(gRPC_ABSL_PROVIDER      "module" CACHE STRING "" FORCE)
set(gRPC_PROTOBUF_PROVIDER  "module" CACHE STRING "" FORCE)
set(gRPC_SSL_PROVIDER       "module" CACHE STRING "" FORCE)
set(gRPC_ZLIB_PROVIDER      "module" CACHE STRING "" FORCE)
set(gRPC_CARES_PROVIDER     "module" CACHE STRING "" FORCE)
set(gRPC_RE2_PROVIDER       "module" CACHE STRING "" FORCE)

set(protobuf_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL        OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD  ON  CACHE BOOL "" FORCE)

FetchContent_Declare(grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG        ${OMLE_GRPC_VERSION}
    GIT_SHALLOW    TRUE
    # Submodules carry protobuf, Abseil, BoringSSL, re2, c-ares and zlib. They
    # are what makes the checkout large and the build long.
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(grpc)

# ── Hand omle-runtime the protobuf that gRPC just built ─────────────────────
# Its CMakeLists skips find_package(Protobuf) when protobuf::libprotobuf is
# already defined, and reads these three for the codegen rule. Without them it
# would find the system protobuf and the link would carry two versions.
set(Protobuf_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>"          CACHE STRING "" FORCE)
set(Protobuf_INCLUDE_DIRS      "${grpc_SOURCE_DIR}/third_party/protobuf/src" CACHE STRING "" FORCE)
set(Protobuf_VERSION           "bundled with gRPC ${OMLE_GRPC_VERSION}"      CACHE STRING "" FORCE)

# The proto rule in the top-level CMakeLists looks this up; with a vendored
# build it is a target rather than something on PATH.
set(GRPC_CPP_PLUGIN_EXECUTABLE "$<TARGET_FILE:grpc_cpp_plugin>" CACHE STRING "" FORCE)

# ── jsoncpp ─────────────────────────────────────────────────────────────────
# Drogon requires jsoncpp and has no vendoring option of its own, so it is
# fetched here first — a subproject target is already defined by the time
# Drogon looks, so its find_package does not reach for a system copy.
#
# Worth doing beyond tidiness: jsoncpp was the last non-TLS shared library the
# binary still named, and it is the dependency with no apt/brew equivalent on
# Windows, so vendoring it is also what makes a Windows build plausible.
set(JSONCPP_WITH_TESTS          OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT   OFF CACHE BOOL "" FORCE)
set(BUILD_OBJECT_LIBS           OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS           ON  CACHE BOOL "" FORCE)
FetchContent_Declare(jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(jsoncpp)

# Point Drogon's FindJsoncpp at what was just built, rather than defining
# Jsoncpp_lib here. That module runs find_path/find_library — which is how the
# system copy got linked — and then creates Jsoncpp_lib itself and calls
# set_target_properties on it. Pre-defining that target as an ALIAS makes its
# `if(NOT TARGET ...)` guard skip the creation while set_target_properties
# still runs, and CMake refuses to set properties on an alias.
#
# Seeding the two cache variables instead stops both find_ calls from searching
# (they honour a value already in the cache) and leaves Drogon to build its own
# imported target around the static library.
set(JSONCPP_INCLUDE_DIRS "${jsoncpp_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(JSONCPP_LIBRARIES    jsoncpp_static                  CACHE STRING "" FORCE)

# ── Drogon (brings trantor) ─────────────────────────────────────────────────
set(OMLE_DROGON_VERSION "v1.9.12" CACHE STRING "Drogon tag for bundled builds")

set(BUILD_CTL       OFF CACHE BOOL "" FORCE)  # drogon_ctl scaffolding tool
set(BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(BUILD_ORM       OFF CACHE BOOL "" FORCE)  # no database access in this server
set(BUILD_DROGON_SHARED OFF CACHE BOOL "" FORCE)
set(USE_STATIC_LIBS_ONLY ON CACHE BOOL "" FORCE)

# Each of these pulled a shared library into the "self-contained" binary.
# Brotli was three dylibs for response compression the OIP payloads do not
# need — zlib still provides gzip. The YAML config reader is dead weight
# beside a server configured with JSON. c-ares is trantor's async DNS
# resolver, which matters for a client making outbound connections, not for a
# server binding two listen ports.
set(BUILD_BROTLI      OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
set(BUILD_C-ARES      OFF CACHE BOOL "" FORCE)

# OpenSSL is deliberately left alone. Drogon uses it for REST TLS, and gRPC's
# vendored BoringSSL is not a drop-in replacement, so dropping it would mean
# giving up HTTPS rather than just shedding a dependency. libssl/libcrypto
# therefore remain external; see the README on what that means for deployment.
FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        ${OMLE_DROGON_VERSION}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(drogon)

# ── simdjson ────────────────────────────────────────────────────────────────
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.10.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson)

# ── nlohmann/json (header-only) ─────────────────────────────────────────────
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── GoogleTest (only when the C++ tests are being built) ────────────────────
# tests/CMakeLists.txt calls find_package(GTest REQUIRED). That works where a
# package manager supplies it, but a bundled build is by definition running
# somewhere that has no system packages — Windows most of all. Fetching it here
# keeps `-DBUILD_SERVER_TESTS=ON -DOMLE_SERVER_BUNDLE_DEPS=ON` a working
# combination on every platform.
if(BUILD_SERVER_TESTS)
    # GoogleTest defaults to linking the shared CRT on MSVC while everything
    # else here is static; mismatching those is a link error, not a warning.
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ── Namespaced aliases ──────────────────────────────────────────────────────
# find_package() defines Drogon::Drogon, gRPC::grpc++ and friends from each
# project's installed config file. Built as subprojects they define the bare
# targets instead (drogon, grpc++, ...), so the rest of the build — written
# against the find_package spelling — would not resolve them. Aliasing here
# keeps CMakeLists.txt identical in both modes.
foreach(_pair "Drogon::Drogon=drogon"
              "gRPC::grpc++=grpc++"
              "gRPC::grpc=grpc"
              "simdjson::simdjson=simdjson"
              "nlohmann_json::nlohmann_json=nlohmann_json"
              "GTest::GTest=gtest"
              "GTest::Main=gtest_main"
              "GTest::gtest=gtest"
              "GTest::gtest_main=gtest_main")
    string(REPLACE "=" ";" _parts "${_pair}")
    list(GET _parts 0 _alias)
    list(GET _parts 1 _real)
    if(NOT TARGET ${_alias} AND TARGET ${_real})
        add_library(${_alias} ALIAS ${_real})
        message(STATUS "  alias ${_alias} -> ${_real}")
    endif()
endforeach()

message(STATUS "Bundled deps       : gRPC ${OMLE_GRPC_VERSION}, Drogon ${OMLE_DROGON_VERSION} (static)")

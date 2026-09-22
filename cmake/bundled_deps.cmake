# cmake/bundled_deps.cmake — build every dependency from pinned source as a
# static archive, so the server links into one self-contained executable.
#
# Scope: only libraries the shipped executable links. A test-only dependency
# has no business here — it never reaches the binary, and fetching it would tie
# "make the artifact self-contained" to "run the test suite", which are
# unrelated. GoogleTest is fetched by tests/CMakeLists.txt instead.
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

# Nothing here is ever installed — the dependencies are linked into one
# executable and that binary is copied where it is needed. Leaving their
# install rules defined is not merely dead weight: Drogon exports a
# DrogonTargets set, and once ZLIB::ZLIB points at gRPC's zlibstatic (below)
# CMake refuses to generate, because an exported target may not depend on one
# outside any export set.
set(CMAKE_SKIP_INSTALL_RULES ON)

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
# "package", not "module": module builds BoringSSL, and Drogon links the
# system OpenSSL, so both end up in one link defining ERR_get_error_line,
# X509_STORE_CTX_get_error and friends — hundreds of duplicate symbols. One
# SSL implementation has to win, and it is OpenSSL, because that is the one
# the operating system keeps patched. This is the same reason OpenSSL is left
# out of the published wheel.
set(gRPC_SSL_PROVIDER       "package" CACHE STRING "" FORCE)
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

# ── zlib: hand Drogon the copy gRPC just built ──────────────────────────────
# Drogon does find_package(ZLIB REQUIRED) and links ZLIB::ZLIB. gRPC compiled
# zlib as a submodule, but FindZLIB only searches the system, so on a machine
# with no system zlib — any Windows runner — the configure fails outright.
#
# FindZLIB skips creating ZLIB::ZLIB when the target already exists, so the
# alias below wins. The two cache variables exist purely so that
# find_package_handle_standard_args sees non-empty values and reports success;
# nothing reads them once ZLIB::ZLIB resolves.
#
# The include path needs both directories: zlib.h is in the source tree while
# zconf.h is generated into the build tree.
foreach(_zlib_tgt zlibstatic zlib)
    if(TARGET ${_zlib_tgt} AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS ${_zlib_tgt})
        set(ZLIB_LIBRARY ${_zlib_tgt} CACHE STRING "" FORCE)
        set(ZLIB_INCLUDE_DIR
            "${grpc_SOURCE_DIR}/third_party/zlib;${grpc_BINARY_DIR}/third_party/zlib"
            CACHE STRING "" FORCE)
        message(STATUS "  zlib: using gRPC's ${_zlib_tgt}")
    endif()
endforeach()

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

# Resolve UUID ourselves, before Drogon can fail to.
#
# USE_STATIC_LIBS_ONLY above makes Drogon narrow CMAKE_FIND_LIBRARY_SUFFIXES to
# .a for the rest of its configure, and its find_package(UUID) then searches
# only for libuuid.a. No mainstream distribution ships one — libuuid-devel on
# AlmaLinux, Debian and Ubuntu carries libuuid.so alone — so configure dies
# with "Could not find UUID" on a machine where UUID is plainly installed.
#
# macOS never hit this: Drogon's FindUUID.cmake accepts an empty library on
# Apple and BSD, so only Linux fails, which is why the bundled macOS build
# passed while the manylinux wheel did not.
#
# FindUUID.cmake short-circuits when both cache variables are already set, so
# finding them here with the normal suffixes skips its restricted search
# entirely. The spellings mirror what it would have produced: the library by
# plain name, and the directory *containing* uuid.h — Drogon's Utilities.cc
# includes <uuid.h>, not <uuid/uuid.h>.
#
# libuuid stays a shared system library, like libssl. It is part of util-linux
# and present everywhere; absorbing it would gain nothing.
if(UNIX AND NOT APPLE)
    find_library(OMLE_UUID_LIBRARY NAMES uuid)
    find_path(OMLE_UUID_INCLUDE_DIR NAMES uuid.h PATH_SUFFIXES uuid)
    if(OMLE_UUID_LIBRARY AND OMLE_UUID_INCLUDE_DIR)
        set(UUID_LIBRARIES    "${OMLE_UUID_LIBRARY}"     CACHE STRING "" FORCE)
        set(UUID_INCLUDE_DIRS "${OMLE_UUID_INCLUDE_DIR}" CACHE STRING "" FORCE)
        message(STATUS "UUID (system)      : ${UUID_LIBRARIES}")
    else()
        message(FATAL_ERROR
            "libuuid not found. Install it before configuring: "
            "uuid-dev on Debian/Ubuntu, libuuid-devel on RHEL/Alma/Fedora. "
            "Drogon requires it and its own search cannot see a shared one "
            "once USE_STATIC_LIBS_ONLY narrows the library suffixes.")
    endif()
endif()

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
              "nlohmann_json::nlohmann_json=nlohmann_json")
    string(REPLACE "=" ";" _parts "${_pair}")
    list(GET _parts 0 _alias)
    list(GET _parts 1 _real)
    if(NOT TARGET ${_alias} AND TARGET ${_real})
        add_library(${_alias} ALIAS ${_real})
        message(STATUS "  alias ${_alias} -> ${_real}")
    endif()
endforeach()

message(STATUS "Bundled deps       : gRPC ${OMLE_GRPC_VERSION}, Drogon ${OMLE_DROGON_VERSION} (static)")

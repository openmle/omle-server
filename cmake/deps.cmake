# cmake/deps.cmake — locate or fetch all third-party dependencies.
#
# Required system packages (install before building):
#   macOS:  brew install drogon grpc
#           conda install -c conda-forge grpc-cpp   (alternative for gRPC)
#   Ubuntu: apt install libdrogon-dev libgrpc++-dev protobuf-compiler-grpc
#
# nlohmann/json is downloaded automatically via FetchContent (header-only).
# simdjson is found via find_package; install with: brew install simdjson

include(FetchContent)

# ── gRPC + Protobuf ───────────────────────────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH
    "/opt/anaconda3"
    "/opt/homebrew"
    "/opt/homebrew/share/cmake"
    "/usr/local"
)

find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf REQUIRED)

# Locate grpc_cpp_plugin binary
if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(GRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin LOCATION)
else()
    find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin
        HINTS /opt/anaconda3/bin /opt/homebrew/bin /usr/local/bin)
endif()
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found. Install gRPC first.")
endif()
message(STATUS "grpc_cpp_plugin: ${GRPC_CPP_PLUGIN_EXECUTABLE}")

# ── Drogon ────────────────────────────────────────────────────────────────────
# REQUIRED rather than a hand-written error. Without it a failure inside
# DrogonConfig.cmake is swallowed and all that reaches the user is
# "Drogon not found", which points at the wrong thing: the usual cause is not a
# missing libdrogon-dev but one of its transitive dependencies. Ubuntu builds
# Drogon with ORM support, so its config resolves PostgreSQL, SQLite, MySQL,
# Boost, Hiredis and yaml-cpp, and any one of them failing looks identical.
# REQUIRED lets the real message through, naming the dependency that failed.
find_package(Drogon CONFIG REQUIRED)

# Reaching here means Drogon resolved. What follows is for the reader who hits
# a find_dependency failure above and needs to know what to install:
#
#   macOS:  brew install drogon
#   Ubuntu: apt install libdrogon-dev libjsoncpp-dev uuid-dev zlib1g-dev \
#                       libpq-dev libsqlite3-dev libmariadb-dev \
#                       libmariadb-dev-compat libhiredis-dev libyaml-cpp-dev \
#                       libboost-dev libbrotli-dev
#
# The Ubuntu list is long because of that ORM build, not because this server
# uses any of it. libmariadb-dev-compat is the surprising entry: Drogon's
# FindMySQL wants the mysql_config script, which default-libmysqlclient-dev
# does not ship.

# ── simdjson ─────────────────────────────────────────────────────────────────
find_package(simdjson CONFIG
    HINTS /opt/homebrew/lib/cmake/simdjson /usr/local/lib/cmake/simdjson)
if(NOT simdjson_FOUND)
    message(STATUS "simdjson not found via find_package; fetching via FetchContent")
    FetchContent_Declare(simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG        v3.10.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(simdjson)
endif()

# ── nlohmann/json (header-only) ───────────────────────────────────────────────
find_package(nlohmann_json CONFIG QUIET
    HINTS /opt/homebrew/share/cmake/nlohmann_json
          /usr/local/share/cmake/nlohmann_json)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found; fetching via FetchContent")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

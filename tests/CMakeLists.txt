# GoogleTest is a dependency of the tests, not of the server, so it is resolved
# here rather than in cmake/bundled_deps.cmake — nothing it provides reaches the
# shipped executable, and tying it to OMLE_SERVER_BUNDLE_DEPS would mean the
# tests could only be built in the mode used for release artifacts.
#
# Prefer an installed copy, fall back to source. The fallback is what makes
# -DBUILD_SERVER_TESTS=ON work on a machine with no package manager, Windows in
# particular, where find_package(GTest REQUIRED) would fail the configure.
if(NOT TARGET GTest::GTest AND NOT TARGET gtest)
  find_package(GTest QUIET)
endif()

if(NOT TARGET GTest::GTest AND NOT TARGET gtest)
  message(STATUS "GTest not found; fetching it from source")
  include(FetchContent)
  # GoogleTest links the shared CRT by default on MSVC. The server and its
  # dependencies use the static one in a bundled build, and mixing the two is a
  # link error rather than a warning.
  set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2
      GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(googletest)
endif()

# find_package spells these GTest::GTest / GTest::Main; the subproject defines
# gtest / gtest_main. Alias so add_server_test() below reads the same either way.
foreach(_pair "GTest::GTest=gtest" "GTest::Main=gtest_main")
  string(REPLACE "=" ";" _parts "${_pair}")
  list(GET _parts 0 _alias)
  list(GET _parts 1 _real)
  if(NOT TARGET ${_alias} AND TARGET ${_real})
    add_library(${_alias} ALIAS ${_real})
  endif()
endforeach()

# ── Helper for unit tests (link GTest::Main, no custom main) ──────────────────

function(add_server_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_BINARY_DIR}/../grpc_gen"
  )
  target_link_libraries(${name} PRIVATE GTest::GTest GTest::Main)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wno-unused-parameter>)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# ── Unit tests ────────────────────────────────────────────────────────────────

# test_oip_codec: dtype round-trips, JSON parse/serialize.
add_server_test(test_oip_codec
  test_oip_codec.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/oip_codec.cpp"
)
target_link_libraries(test_oip_codec PRIVATE
  omle_runtime oip_proto simdjson::simdjson nlohmann_json::nlohmann_json)

# test_metrics: counters, histograms, Prometheus text format.
add_server_test(test_metrics
  test_metrics.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics.cpp"
)

# ── Integration test: starts the server binary, exercises REST + gRPC ─────────

# Path to the compiled server binary (built in the same tree).
set(SERVER_BINARY_FOR_TESTS "${CMAKE_BINARY_DIR}/omle_server"
    CACHE FILEPATH "omle_server binary used by the integration test")

# Path to the 2-feature test model copied into the temp model directory.
set(TEST_MODEL_SRC
    "${CMAKE_SOURCE_DIR}/../omle-runtime/spark/src/test/resources/test_model_2f.omle"
    CACHE FILEPATH "Test model source for integration tests")

# Integration test has its own main() (uses AddGlobalTestEnvironment).
add_executable(test_server_integration test_server_integration.cpp)
target_include_directories(test_server_integration PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_CURRENT_BINARY_DIR}/../grpc_gen"
)
target_link_libraries(test_server_integration PRIVATE
  GTest::GTest gRPC::grpc++ oip_proto nlohmann_json::nlohmann_json)
target_compile_options(test_server_integration PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wno-unused-parameter>)
target_compile_definitions(test_server_integration PRIVATE
  SERVER_BINARY_PATH="${SERVER_BINARY_FOR_TESTS}"
  TEST_MODEL_SRC_PATH="${TEST_MODEL_SRC}")

# Ensure the server binary is built before the integration test runs.
add_dependencies(test_server_integration omle_server)

add_test(NAME test_server_integration COMMAND test_server_integration)

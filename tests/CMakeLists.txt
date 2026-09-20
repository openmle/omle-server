# Skipped when an enclosing build already supplies GoogleTest — a bundled
# build fetches it from source, because a machine with no package manager (any
# Windows runner) has nothing for find_package to find, and REQUIRED would fail
# the configure outright.
if(NOT TARGET GTest::GTest AND NOT TARGET gtest)
  find_package(GTest REQUIRED)
endif()

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

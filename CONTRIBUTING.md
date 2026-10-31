# Contributing to omle-server

Thanks for your interest. This document covers how to work on this repository —
setup, the layout, and the checks a change needs to pass.

By participating you agree to abide by the [Code of Conduct](CODE_OF_CONDUCT.md).

## What lives here

A C++ inference server that serves OMLE models over the
[Open Inference Protocol](https://github.com/kserve/open-inference-protocol)
(KServe OIP), on both REST and gRPC.

Scoring itself is done by
[omle-runtime](https://github.com/openmle/omle-runtime), which this
repository links against — a change to *how a model executes* belongs there.
This repository owns the protocol surface, batching, model registry, and metrics.

## Getting started

Requires a C++17 compiler, CMake, and these system packages:

```bash
# macOS
brew install cmake protobuf grpc drogon simdjson

# Ubuntu
apt install cmake libgrpc++-dev protobuf-compiler-grpc libdrogon-dev libsimdjson-dev
```

nlohmann/json is fetched automatically via FetchContent if not found.

```bash
mkdir -p build && cd build
cmake -DBUILD_SERVER_TESTS=ON ..
cmake --build . -j8
ctest --output-on-failure
```

**`BUILD_SERVER_TESTS` defaults to `OFF`** — without it the test targets are not
generated and `ctest` finds nothing to run. Pass it whenever you are developing.

The build expects a sibling `omle-runtime` checkout; CMake reports the path it
resolved. If `find_package(gRPC CONFIG REQUIRED)` fails with
`Could not find a package configuration file provided by "gRPC"`, gRPC is not
installed — it is a separate package from protobuf.

3 test targets: `test_metrics`, `test_oip_codec`, `test_server_integration`.
The integration suite forks the built server binary and drives it over real HTTP
and gRPC, so it needs `omle_server` built first (CMake enforces the
dependency).

## Layout

```
include/omle_server/   public headers
  oip_codec.h             OIP request/response encoding (REST JSON + gRPC proto)
  batcher.h               request batching
  model_registry.h        model discovery and loading
  metrics.h               Prometheus exposition
  server_config.h         configuration
src/
  main.cpp                entry point
  rest/handlers.*         Drogon REST handlers
  grpc/inference_service.* gRPC service
  oip_codec.cpp, batcher.cpp, metrics.cpp, model_registry.cpp
tests/                    GoogleTest suites
```

## Working with the runtime API

The server consumes `omle::rt` types from omle-runtime's public headers.
Two things to know:

- **`Tensor`'s buffer is private.** Use `raw_data()` to read bytes — it already
  returns the scalar buffer for `Kind::Scalar`, so branching on `kind` before
  reading is unnecessary.
- **Proto types live in namespace `omle`**, the public runtime API in
  `omle::rt`. There is no `omle::v1`.

When omle-runtime changes its public headers, this repository has to be
rebuilt against them; it is the first place an accidental API break shows up.

## Metrics

`Metrics::prometheus_text()` emits the `# HELP`/`# TYPE` headers for every
metric family even when no model has recorded traffic, so a scraper can discover
the families before the server's first request. Keep that property — the
per-model sample loops emit nothing on their own when the registry is empty.

## Code style

C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
strictly — 2-space indent, 80 columns, no deviations. `.clang-format` is just
`BasedOnStyle: Google`.

```bash
find src include tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
```

Headers use `#define` include guards named `<PROJECT>_<PATH>_<FILE>_H_` with the
source root stripped — `include/omle_server/metrics.h` is
`OMLE_SERVER_METRICS_H_`, `src/grpc/inference_service.h` is
`OMLE_SERVER_GRPC_INFERENCE_SERVICE_H_`. Not `#pragma once`.

## Pre-commit hooks (optional)

```bash
pip install pre-commit
pre-commit install
```

Runs `clang-format` plus the hygiene hooks.

## Tests

Add a GoogleTest case with any behaviour change. Protocol changes should be
covered in `test_server_integration.cpp` against both transports — REST and
gRPC are separate code paths and have drifted apart before.

## Reporting bugs

Include the request that triggered it (curl command or gRPC call), the server
log, and the model being served. For protocol issues, say which transport —
a bug in one often does not exist in the other.

## License

Contributions are accepted under the [Apache License 2.0](LICENSE), in
accordance with section 5 of that license. There is no separate CLA.

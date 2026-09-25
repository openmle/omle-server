# OMLE Server

[![PyPI](https://img.shields.io/pypi/v/omle-server.svg)](https://pypi.org/project/omle-server/)
[![Tests](https://github.com/openmle/omle-server/actions/workflows/test.yml/badge.svg)](https://github.com/openmle/omle-server/actions/workflows/test.yml)

High-performance C++ inference server for OMLE models.  Implements the
[Open Inference Protocol](https://github.com/kserve/open-inference-protocol)
(kserve OIP) over both REST (HTTP/1.1) and gRPC.

---

## Installation

```bash
pip install omle-server
omle-server                        # configs/server.json, or built-in defaults
```

The wheel carries a statically linked executable — gRPC, Abseil, protobuf,
Drogon, jsoncpp, simdjson and the OMLE runtime are all inside it.

**OpenSSL 3 must be present on the system.** It is the one library deliberately
left dynamic: bundling it would freeze a cryptographic library at build time,
cut off from the operating system's security updates and patchable only by
cutting a new release. Every current distribution ships it (`libssl3` on
Debian/Ubuntu, `openssl-libs` on Fedora/RHEL, `openssl@3` on Homebrew).

Wheels cover linux-x86_64, macos-arm64 and windows-x86_64; anything else builds
from source.

---

## Running

```bash
omle-server --model-dir /path/to/models --rest-port 8080 --grpc-port 8081
omle-server --help                     # every flag, with defaults
```

Nothing has to exist first: with no config file and no flags the built-in
defaults start a server on 8080/8081 reading `/models`.

### Configuration precedence

Four sources, each overriding the one above it:

| | source | example |
|---|---|---|
| 1 | built-in defaults | `rest_port` 8080 |
| 2 | config file | `--config server.json`, else `configs/server.json` |
| 3 | environment | `OMLE_REST_PORT=9000` |
| 4 | command-line flags | `--rest-port 9000` |

Environment sits below flags so a container image can set a baseline that
`docker run` still overrides, without rewriting the config file.

```bash
# start from the defaults, then edit to taste
omle-server init-config -o server.json
omle-server --config server.json

# or skip the file entirely
OMLE_MODEL_DIR=/models omle-server --rest-port 9000
```

`init-config` writes every setting with its default value, generated from the
same struct the loader reads back, so it cannot drift from the code. It refuses
to clobber an existing file unless given `--force`.

A bare config path — `omle-server server.json` — still works, as the first
release accepted it, but `--config` is the documented spelling.

### Environment variables

| variable | equivalent flag |
|---|---|
| `OMLE_CONFIG` | `--config` |
| `OMLE_MODEL_DIR` | `--model-dir` |
| `OMLE_REST_PORT` | `--rest-port` |
| `OMLE_GRPC_PORT` | `--grpc-port` |
| `OMLE_REST_THREADS` | `--rest-threads` |
| `OMLE_GRPC_THREADS` | `--grpc-threads` |
| `OMLE_MODEL_THREADS` | `--model-threads` |
| `OMLE_LOG_LEVEL` | `--log-level` |

### Example REST inference

```bash
curl -X POST http://localhost:8080/v2/models/iris_classifier/infer \
  -H "Content-Type: application/json" \
  -d '{
    "inputs": [{
      "name": "input",
      "datatype": "FP64",
      "shape": [1, 4],
      "data": [5.1, 3.5, 1.4, 0.2]
    }]
  }'
```

Response:
```json
{
  "model_name": "iris_classifier",
  "outputs": [
    { "name": "prediction", "datatype": "STRING", "shape": [1, 1], "data": ["setosa"] },
    { "name": "probability", "datatype": "FP64",  "shape": [1, 3], "data": [0.97, 0.02, 0.01] }
  ]
}
```

---

## Docker

```bash
docker pull omle/omle-server

docker run -p 8080:8080 -p 8081:8081 \
    -v "$PWD/models:/models:ro" \
    omle/omle-server
```

Published on a release tag, multi-architecture for `linux/amd64` and
`linux/arm64`. `latest` tracks the most recent final release and never a release
candidate, so pulling unqualified will not hand you an rc.

To build it yourself — from the repository root, since the context has to contain
the sources, hence `-f` rather than `cd docker`:

```bash
docker build -f docker/Dockerfile -t omle-server .
```

`/models` is where the server looks by default. Flags go after the image name,
since the entrypoint is the binary itself:

```bash
docker run ... omle-server --rest-port 9000 --batching --max-batch-size 64
docker run ... omle-server --help
```

`init-config` writes a file rather than printing, so it needs somewhere writable
mounted — the image has no writable working directory:

```bash
docker run --rm -v "$PWD:/out" omle-server init-config -o /out/server.json
docker run -v "$PWD/server.json:/etc/omle/server.json:ro" ... \
    omle-server --config /etc/omle/server.json
```

The build is a two-stage one and takes a while: the first stage compiles every
dependency from pinned source, and cloning gRPC's submodule tree — Abseil,
protobuf, BoringSSL, re2, c-ares — dominates. The second stage keeps the
resulting executable and three libraries, nothing else.

It needs `omle-runtime` and `omle` as well, since this project's CMake adds the
former as a subproject and the former generates `omle.pb.cc` from the latter's
schema. The Dockerfile clones both itself, at the refs `.github/workflows/
publish.yml` pins, so no sibling checkouts are required. Override them to test a
different pairing:

```bash
docker build -f docker/Dockerfile \
    --build-arg OMLE_RUNTIME_REF=v0.1.0-rc11 \
    --build-arg OMLE_REF=v0.1.1 \
    -t omle-server .
```

The base is Debian 12, chosen for two reasons rather than habit. It ships
OpenSSL 3, so the `libssl.so.3` the binary records as a dependency resolves —
an OpenSSL 1.1 base would leave it unsatisfiable. And it is glibc, matching what
CI actually tests: the Linux job builds on `ubuntu-latest` and the wheels come
out of manylinux. An Alpine image would be roughly 65 MB smaller but would
recompile every dependency against musl, which nothing here exercises, for a
saving that is modest beside a ~22 MB statically linked executable.

The container runs as an unprivileged user (uid 10001) and declares a
`HEALTHCHECK` against `/v2/health/ready` rather than `/v2/health/live`: ready
reflects the registry having loaded, so a container pointed at an empty or
unreadable model directory reports unhealthy instead of accepting traffic it
cannot serve.

---

## Architecture

Built on top of **omle-runtime** for model execution, **Drogon** for the REST
layer, and the **gRPC C++** library for the gRPC layer.

```
                ┌──────────────────────────────────────────┐
                │               omle-server                │
                │                                          │
  REST :8080 ──►│  Drogon (N I/O threads)                  │
                │    OIP handlers → OipCodec               │
                │                       │                  │
  gRPC :8081 ──►│  gRPC server (M threads)                 │
                │    InferenceServiceImpl → OipCodec       │
                │                       │                  │
                │              ModelRegistry               │
                │         (shared_ptr<Model> map)          │
                │                       │                  │
                │          omle::Model::predict()          │
                └──────────────────────────────────────────┘
```

`ModelRegistry` loads all models at startup from `model_dir`.  The registry is
read-only after startup — `omle::Model` is thread-safe, so all I/O threads
call `predict()` concurrently with no locks.

---

## OIP Endpoints

### REST (port 8080)

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/v2/health/live` | Liveness check |
| `GET` | `/v2/health/ready` | Readiness check |
| `GET` | `/v2` | Server metadata |
| `GET` | `/v2/models/{name}` | Model metadata |
| `GET` | `/v2/models/{name}/versions/{ver}` | Versioned model metadata |
| `GET` | `/v2/models/{name}/ready` | Model readiness |
| `POST` | `/v2/models/{name}/infer` | Run inference |
| `POST` | `/v2/models/{name}/versions/{ver}/infer` | Versioned inference |

### gRPC (port 8081)

Service: `inference.GRPCInferenceService` (see `proto/open_inference_grpc.proto`)

RPCs: `ServerLive`, `ServerReady`, `ModelReady`, `ServerMetadata`,
`ModelMetadata`, `ModelInfer`, `ModelStreamInfer`.

---

## Model Directory Layout

Two conventions are supported:

```
# Flat (version = "1")
/models/
  iris_classifier.omle
  price_regressor.omle

# Versioned
/models/
  iris_classifier/
    1/model.omle
    2/model.omle
```

---

## Building

### Prerequisites

```bash
# macOS
brew install drogon nlohmann-json simdjson
conda install -c conda-forge grpc-cpp   # or via conda base env

# Ubuntu — the long tail is Drogon's, not this project's. Ubuntu builds
# libdrogon-dev with ORM support, so its CMake config insists on PostgreSQL,
# SQLite, MySQL, Boost, Hiredis and yaml-cpp headers even though the server
# touches no database. libmariadb-dev-compat supplies the mysql_config script
# that Drogon's FindMySQL looks for; default-libmysqlclient-dev does not.
apt install libdrogon-dev libjsoncpp-dev uuid-dev zlib1g-dev \
            libpq-dev libsqlite3-dev libmariadb-dev libmariadb-dev-compat \
            libhiredis-dev libyaml-cpp-dev libboost-dev libbrotli-dev \
            libgrpc++-dev protobuf-compiler protobuf-compiler-grpc libprotobuf-dev \
            libgtest-dev libsimdjson-dev nlohmann-json3-dev
```

If that list is unwelcome, `-DOMLE_SERVER_BUNDLE_DEPS=ON` builds every
dependency from pinned source and needs only `libssl-dev` and `uuid-dev` — at
the cost of a much longer build. See *Self-contained build* below.

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

The build will also compile **omle-runtime** from the sibling directory
`../omle-runtime` automatically.

### Self-contained build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOMLE_SERVER_BUNDLE_DEPS=ON
cmake --build build --target omle_server
```

Compiles every dependency from pinned source and links them statically, which
is how the published wheels are built. It takes considerably longer — gRPC and
its submodules are built from scratch — and produces one ~25 MB executable that
can be copied to a machine with nothing installed but a C++ runtime and
OpenSSL. A default build links 112 shared libraries, 79 of them Abseil, and so
only runs where all of them are present at matching versions.

It is also the only way to build where no package manager supplies Drogon and
gRPC, which is why CI uses it for Windows.

---

## Configuration (`configs/server.json`)

| Key | Default | Description |
|-----|---------|-------------|
| `model_dir` | `/models` | Directory to scan for `.omle` files |
| `rest_port` | `8080` | REST listen port |
| `grpc_port` | `8081` | gRPC listen port |
| `rest_threads` | `0` | Drogon I/O threads (0 = auto) |
| `grpc_threads` | `0` | gRPC completion queue threads (0 = auto) |
| `model_n_threads` | `1` | Per-model inference threads (>1 for batch) |
| `run_verification` | `true` | Run built-in verification after model load |
| `log_level` | `"info"` | Log verbosity |

---

## Testing

### C++ unit tests

```bash
cmake -S . -B build_tests -DBUILD_SERVER_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests --parallel
cd build_tests && ctest --output-on-failure
```

Tests cover:
- **`test_oip_codec`** — dtype round-trips, JSON request parse (FP32/FP64/STRING/INT64/batch/dynamic-dim), error cases, response serialisation
- **`test_metrics`** — counters, histograms, Prometheus text format
- **`test_server_integration`** — starts the server binary, exercises all REST and gRPC endpoints end-to-end (health, metadata, model readiness, inference, Prometheus, gRPC streaming)

### Python integration tests

```bash
cd python
pip install -e ".[dev]"
pytest tests/ -v
```

Tests cover all REST and gRPC OIP endpoints:
- Health (`/v2/health/live`, `/v2/health/ready`)
- Server metadata (`/v2`)
- Model metadata and readiness (`/v2/models/{name}`, `/v2/models/{name}/ready`, versioned variants)
- Inference (`/v2/models/{name}/infer`, versioned, with request-id, error cases)
- Prometheus metrics (`/metrics`)
- gRPC: `ServerLive`, `ServerReady`, `ModelReady`, `ServerMetadata`, `ModelMetadata`, `ModelInfer`, `ModelStreamInfer`

The server fixture auto-skips if the binary is not found or fails to start.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and the checks a
change needs to pass, and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community
expectations.

## License

[Apache License 2.0](LICENSE)

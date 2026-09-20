# omle-server

High-performance C++ inference server for OMLE models.  Implements the
[Open Inference Protocol](https://github.com/kserve/open-inference-protocol)
(kserve OIP) over both REST (HTTP/1.1) and gRPC.

Built on top of **omle-runtime** for model execution,
**Drogon** for the REST layer, and the **gRPC C++** library for the gRPC layer.

---

## Architecture

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
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

The build will also compile **omle-runtime** from the sibling directory
`../omle-runtime` automatically.

---

## Running

```bash
./build/omle_server configs/server.json
```

Or with environment variable overrides:

```bash
OMLE_MODEL_DIR=/path/to/models \
OMLE_REST_PORT=8080 \
OMLE_GRPC_PORT=8081 \
./build/omle_server
```

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

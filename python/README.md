# OMLE Server

[![PyPI](https://img.shields.io/pypi/v/omle-server.svg)](https://pypi.org/project/omle-server/)
[![Tests](https://github.com/openmle/omle-server/actions/workflows/test.yml/badge.svg)](https://github.com/openmle/omle-server/actions/workflows/test.yml)

Inference server for OMLE models, speaking the
[Open Inference Protocol](https://github.com/kserve/open-inference-protocol)
over both REST and gRPC.

```bash
pip install omle-server
omle-server                        # built-in defaults
omle-server --help                 # every flag
omle-server --model-dir ./models --rest-port 9000
omle-server --config /etc/omle/server.json
```

This package is a delivery vehicle for a compiled binary, not a Python library.
Installing it puts `omle-server` on PATH; that console script `exec`s the native
executable, so no Python remains in the process once the server is running.

## Requirements

**OpenSSL 3** must be present on the system. Everything else the server needs —
gRPC, Abseil, protobuf, Drogon, jsoncpp, simdjson and the OMLE runtime itself —
is linked statically into the binary, so there is nothing else to install.

OpenSSL is the deliberate exception. Bundling it would freeze a cryptographic
library at build time, cut off from the security updates the operating system
provides, and it could then only be patched by releasing a new wheel. It ships
with every current Linux distribution and is in the Homebrew baseline on macOS:

```bash
# only if it is somehow missing
apt install libssl3          # Debian / Ubuntu
dnf install openssl-libs     # Fedora / RHEL
brew install openssl@3       # macOS
```

Windows is the exception: there is no system OpenSSL to link, so that wheel
carries its own statically. Nothing to install, but a fix there needs a new
release rather than an operating-system update.

Wheels are published for linux-x86_64, macos-arm64 and windows-x86_64. Other
platforms build from source — see the repository README.

## Configuring

Four sources, each overriding the one above it: built-in defaults, then a JSON
config file, then the environment, then command-line flags.

```bash
omle-server init-config -o server.json    # every setting, at its default
omle-server --config server.json
```

| Variable | Flag | Default | Meaning |
|---|---|---|---|
| `OMLE_CONFIG` | `--config` | `configs/server.json` | config file |
| `OMLE_MODEL_DIR` | `--model-dir` | `/models` | directory scanned for `.omle` files |
| `OMLE_REST_PORT` | `--rest-port` | `8080` | REST listen port |
| `OMLE_GRPC_PORT` | `--grpc-port` | `8081` | gRPC listen port |
| `OMLE_REST_THREADS` | `--rest-threads` | one per core | REST workers |
| `OMLE_GRPC_THREADS` | `--grpc-threads` | one per core | gRPC workers |
| `OMLE_MODEL_THREADS` | `--model-threads` | `1` | threads per model |
| `OMLE_LOG_LEVEL` | `--log-level` | `info` | trace…error |

```bash
OMLE_MODEL_DIR=./models omle-server --rest-port 9000
omle-server --help                        # the full list
```

Environment sits below flags so a container image can set a baseline that
`docker run` still overrides.

There is deliberately no Python wrapper translating arguments into these. It
could only restate what the binary already does, and would drift from it. To
launch the server from Python, run it as a subprocess like any other program:

```python
import os, subprocess
import omle_server

proc = subprocess.Popen(
    [omle_server.binary_path()],
    env={**os.environ, "OMLE_MODEL_DIR": "/models"},
)
```

`omle_server.binary_path()` returns the absolute path to the bundled
executable, and is the only function this package exposes.

## Checking it works

```bash
curl localhost:8080/v2/health/ready          # 200 once the registry has loaded
curl localhost:8080/v2/models/<name>         # input/output metadata
```

## Related packages

- [`omle`](https://pypi.org/project/omle/) — the model IR, protobuf I/O and
  validation
- [`omle-convert`](https://pypi.org/project/omle-convert/) — converters from
  trained scikit-learn, XGBoost, LightGBM, CatBoost and Spark ML models
- [`omle-runtime`](https://pypi.org/project/omle-runtime/) — the C++ inference
  runtime, with a scikit-learn-style API
- [`omle-spark`](https://pypi.org/project/omle-spark/) — Spark ML transformer
  for scoring DataFrames from PySpark
- [`omle-viewer`](https://pypi.org/project/omle-viewer/) — interactive DAG
  viewer for Jupyter and the browser

## License

Apache-2.0

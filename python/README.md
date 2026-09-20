# omle-server

Inference server for OMLE models, speaking the
[Open Inference Protocol](https://github.com/kserve/open-inference-protocol)
over both REST and gRPC.

```bash
pip install omle-server
omle-server                        # configs/server.json, or built-in defaults
omle-server /etc/omle/server.json  # an explicit config
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

Wheels are published for linux-x86_64, linux-aarch64 and macos-arm64. Other
platforms build from source — see the repository README.

## Configuring

The binary reads a JSON config file, and these environment variables override
whatever it contains:

| Variable | Default | Meaning |
|---|---|---|
| `OMLE_MODEL_DIR` | `/models` | directory scanned for `.omle` files |
| `OMLE_REST_PORT` | `8080` | REST listen port |
| `OMLE_GRPC_PORT` | `8081` | gRPC listen port |

```bash
OMLE_MODEL_DIR=./models OMLE_REST_PORT=9000 omle-server
```

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

- [`omle`](https://pypi.org/project/omle/) — the model IR and converters
- [`omle-runtime`](https://pypi.org/project/omle-runtime/) — the local runtime
- [`omle-spark`](https://pypi.org/project/omle-spark/) — PySpark transformer

## License

Apache-2.0

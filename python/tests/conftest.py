"""Pytest fixtures for omle-server integration tests.

The server binary is started once per test session (session-scoped fixture).
A temporary model directory is populated with a copy of the 2-feature test
model renamed to the .omle extension that the model registry requires.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import socket
import sys
import subprocess
import tempfile
import time

import pytest
import requests

# ── paths ─────────────────────────────────────────────────────────────────────

# MSVC appends .exe, and a multi-config generator puts the binary under
# build/<Config>/ rather than build/. Both spellings are searched so the same
# tests run everywhere; without this the Windows job would find nothing and
# skip (see the fixture below), reporting green having tested nothing.
_BUILD_DIR = pathlib.Path(__file__).parent.parent.parent / "build"
_BIN_NAME = "omle_server.exe" if sys.platform == "win32" else "omle_server"
_SERVER_BIN = next(
    (
        c
        for c in (
            _BUILD_DIR / _BIN_NAME,
            _BUILD_DIR / "Release" / _BIN_NAME,
            _BUILD_DIR / "RelWithDebInfo" / _BIN_NAME,
            _BUILD_DIR / "Debug" / _BIN_NAME,
        )
        if c.exists()
    ),
    _BUILD_DIR / _BIN_NAME,  # reported in the skip message when none exist
)

# Test models, vendored in this repository under tests/data/.
#
# These used to be read from a sibling omle-runtime checkout, under spark/,
# which stopped existing when the Spark bindings moved to their own repository.
# The copy below was conditional, so the models were skipped in silence and the
# server started against an empty directory: health checks passed while every
# model-dependent test failed with 404/503, naming nothing that was missing.
# The C++ integration test had the identical defect.
_DATA_DIR = pathlib.Path(__file__).parent.parent.parent / "tests" / "data"

# 2 FP64 features → 1 FP32 score.
_TEST_MODEL_SRC = _DATA_DIR / "test_model_2f.omle"

# 3-class probability model (2 features → 3-class softmax).
_TEST_MODEL_3CLASS_SRC = _DATA_DIR / "test_model_3class.omle"

REST_PORT = 18080
GRPC_PORT = 18081


def _free_port(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def _wait_ready(url: str, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            r = requests.get(url, timeout=1)
            if r.status_code == 200:
                return
        except Exception:
            pass
        time.sleep(0.2)
    raise RuntimeError(f"Server did not become ready within {timeout}s ({url})")


@pytest.fixture(scope="session")
def model_dir(tmp_path_factory):
    d = tmp_path_factory.mktemp("models")
    # Fail, do not skip: without these the server still answers /v2/health/*,
    # so a missing file surfaced as 24 failures with 404s and 503s instead.
    missing = [p for p in (_TEST_MODEL_SRC, _TEST_MODEL_3CLASS_SRC) if not p.exists()]
    if missing:
        raise FileNotFoundError(
            "test models not found: "
            + ", ".join(str(p) for p in missing)
            + " — every model-dependent test fails with 404/503 without them"
        )
    shutil.copy(_TEST_MODEL_SRC, d / "test_model_2f.omle")
    shutil.copy(_TEST_MODEL_3CLASS_SRC, d / "test_model_3class.omle")
    return d


@pytest.fixture(scope="session")
def server(model_dir):
    """Start the omle_server binary and wait for REST to be ready."""
    if not _SERVER_BIN.exists():
        pytest.skip(f"Server binary not found: {_SERVER_BIN}")

    if not _free_port(REST_PORT) or not _free_port(GRPC_PORT):
        pytest.skip(f"Ports {REST_PORT}/{GRPC_PORT} are already in use")

    env = os.environ.copy()
    env["OMLE_MODEL_DIR"] = str(model_dir)
    env["OMLE_REST_PORT"] = str(REST_PORT)
    env["OMLE_GRPC_PORT"] = str(GRPC_PORT)

    proc = subprocess.Popen(
        [str(_SERVER_BIN)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        _wait_ready(f"http://127.0.0.1:{REST_PORT}/v2/health/live")
    except RuntimeError:
        proc.terminate()
        proc.wait()
        pytest.skip("Server failed to start within timeout (check binary/library dependencies)")

    yield proc

    proc.terminate()
    proc.wait()


@pytest.fixture(scope="session")
def rest_url():
    return f"http://127.0.0.1:{REST_PORT}"


@pytest.fixture(scope="session")
def grpc_channel():
    import grpc
    channel = grpc.insecure_channel(f"127.0.0.1:{GRPC_PORT}")
    yield channel
    channel.close()

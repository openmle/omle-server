"""Integration tests for all omle-server REST (OIP) endpoints.

Requires the server fixture (defined in conftest.py) to be running.
Model 'test_model_2f' has a single input 'X' (FP64, shape [-1, 2]) and
one output 'score'.
"""
from __future__ import annotations

import json
import pathlib
import struct

import pytest
import requests

MODEL_NAME = "test_model_2f"
MODEL_3CLASS = "test_model_3class"


# ── /v2/health/live ───────────────────────────────────────────────────────────

def test_server_live(server, rest_url):
    r = requests.get(f"{rest_url}/v2/health/live")
    assert r.status_code == 200
    assert r.json() == {}


# ── /v2/health/ready ─────────────────────────────────────────────────────────

def test_server_ready(server, rest_url):
    r = requests.get(f"{rest_url}/v2/health/ready")
    assert r.status_code == 200
    assert r.json() == {}


# ── /v2 (server metadata) ─────────────────────────────────────────────────────

def test_server_metadata(server, rest_url):
    r = requests.get(f"{rest_url}/v2")
    assert r.status_code == 200
    body = r.json()
    assert "name" in body
    assert "version" in body
    assert "extensions" in body
    assert isinstance(body["extensions"], list)


def test_absent_model_dir_does_not_serve_models_from_elsewhere():
    """An absent model directory yields an empty registry, not someone else's models.

    The fallback used to be ModelRegistry::load("/tmp"), so a server whose model
    directory was missing published every .omle file left in a world-writable
    directory — models the operator never chose — while logging "continuing with
    empty registry".

    This goes through the DEFAULT path deliberately. An explicitly requested
    directory that does not exist is now fatal, so the fallback is unreachable
    that way; the default being absent is the one case that still reaches it.
    """
    import socket
    import subprocess
    import time

    from conftest import _SERVER_BIN

    if not _SERVER_BIN.exists():
        pytest.skip(f"Server binary not found: {_SERVER_BIN}")
    if pathlib.Path("/models").exists():
        pytest.skip("/models exists here, so the default resolves and the "
                    "fallback is not reached")
    if not pathlib.Path("/tmp").is_dir():
        pytest.skip("no /tmp on this platform")

    # The literal /tmp, because that is the path the old fallback hardcoded.
    # tempfile.gettempdir() is wrong here: on macOS it is a per-user
    # /var/folders/... directory, so planting there tests nothing — which is
    # exactly how the first version of this test passed against the bug.
    planted = pathlib.Path("/tmp") / "pytest_planted_model.omle"
    planted.write_bytes((pathlib.Path(__file__).parent.parent.parent
                         / "tests" / "data" / "test_model_2f.omle").read_bytes())

    def _free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    rest_port, grpc_port = _free_port(), _free_port()

    # No --model-dir: the default is used, and it does not exist.
    proc = subprocess.Popen(
        [str(_SERVER_BIN), "serve",
         "--rest-port", str(rest_port), "--grpc-port", str(grpc_port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        base = f"http://127.0.0.1:{rest_port}"
        for _ in range(80):
            if proc.poll() is not None:
                # Fail with the server's own output rather than skipping: a skip
                # here looks like a pass and this test would assert nothing.
                out, _ = proc.communicate()
                pytest.fail(f"server exited during startup (rc={proc.returncode}):\n{out}")
            try:
                requests.get(f"{base}/v2/health/live", timeout=1)
                break
            except requests.RequestException:
                time.sleep(0.25)
        else:
            proc.terminate()
            out, _ = proc.communicate(timeout=10)
            pytest.fail(f"server never answered on {base}:\n{out}")

        r = requests.get(f"{base}/v2/models/pytest_planted_model/ready", timeout=5)
        assert r.status_code == 503, (
            "a model from /tmp is being served; the empty-registry fallback is "
            f"reading it (status {r.status_code})"
        )
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        planted.unlink(missing_ok=True)


# ── /v2/models/{name}/ready ───────────────────────────────────────────────────

def test_model_ready_known(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/{MODEL_NAME}/ready")
    assert r.status_code == 200


def test_model_ready_unknown(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/does_not_exist/ready")
    assert r.status_code == 503


# ── /v2/models/{name}/versions/{ver}/ready ────────────────────────────────────

def test_model_version_ready(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/{MODEL_NAME}/versions/1/ready")
    assert r.status_code == 200


def test_model_version_ready_wrong_version(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/{MODEL_NAME}/versions/99/ready")
    assert r.status_code == 503


# ── /v2/models/{name} (model metadata) ───────────────────────────────────────

def test_model_metadata_known(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/{MODEL_NAME}")
    assert r.status_code == 200
    body = r.json()
    assert body["name"] == MODEL_NAME
    assert "platform" in body
    assert "inputs" in body
    assert "outputs" in body
    # Model has one FP64 input with 2 features.
    assert len(body["inputs"]) >= 1
    inp = body["inputs"][0]
    assert inp["datatype"] == "FP64"
    assert 2 in inp["shape"]


def test_model_metadata_unknown(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/no_such_model")
    assert r.status_code == 404
    assert "error" in r.json()


# ── /v2/models/{name}/versions/{ver} (versioned model metadata) ───────────────

def test_model_version_metadata(server, rest_url):
    r = requests.get(f"{rest_url}/v2/models/{MODEL_NAME}/versions/1")
    assert r.status_code == 200
    body = r.json()
    assert body["name"] == MODEL_NAME


# ── /v2/models/{name}/infer ───────────────────────────────────────────────────

def _infer(rest_url, model, inputs_payload):
    body = {"inputs": inputs_payload}
    return requests.post(f"{rest_url}/v2/models/{model}/infer", json=body)


def test_infer_single_row(server, rest_url):
    r = _infer(rest_url, MODEL_NAME, [
        {"name": "X", "datatype": "FP64", "shape": [1, 2], "data": [1.0, 2.0]}
    ])
    assert r.status_code == 200
    body = r.json()
    assert "outputs" in body
    assert body["model_name"] == MODEL_NAME
    assert len(body["outputs"]) >= 1
    out = body["outputs"][0]
    assert "data" in out or "shape" in out


def test_infer_batch(server, rest_url):
    r = _infer(rest_url, MODEL_NAME, [
        {"name": "X", "datatype": "FP64", "shape": [3, 2],
         "data": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]}
    ])
    assert r.status_code == 200
    out = r.json()["outputs"][0]
    assert out["shape"][0] == 3


def test_infer_with_request_id(server, rest_url):
    payload = {
        "id": "my-req-42",
        "inputs": [{"name": "X", "datatype": "FP64", "shape": [1, 2], "data": [0.0, 1.0]}]
    }
    r = requests.post(f"{rest_url}/v2/models/{MODEL_NAME}/infer", json=payload)
    assert r.status_code == 200
    assert r.json().get("id") == "my-req-42"


def test_infer_dynamic_batch_dim(server, rest_url):
    """shape [-1, 2] should be auto-resolved from data length."""
    r = _infer(rest_url, MODEL_NAME, [
        {"name": "X", "datatype": "FP64", "shape": [-1, 2],
         "data": [1.0, 2.0, 3.0, 4.0]}
    ])
    assert r.status_code == 200
    out = r.json()["outputs"][0]
    assert out["shape"][0] == 2


def test_infer_unknown_model_returns_404(server, rest_url):
    r = _infer(rest_url, "no_such_model", [
        {"name": "X", "datatype": "FP64", "shape": [1, 2], "data": [0.0, 0.0]}
    ])
    assert r.status_code == 404


def test_infer_bad_json_returns_400(server, rest_url):
    r = requests.post(
        f"{rest_url}/v2/models/{MODEL_NAME}/infer",
        data="not valid json",
        headers={"Content-Type": "application/json"},
    )
    assert r.status_code == 400


def test_infer_missing_inputs_returns_400(server, rest_url):
    r = requests.post(
        f"{rest_url}/v2/models/{MODEL_NAME}/infer",
        json={"model_name": MODEL_NAME},
    )
    assert r.status_code == 400


def test_infer_unknown_datatype_returns_400(server, rest_url):
    r = _infer(rest_url, MODEL_NAME, [
        {"name": "X", "datatype": "FLOAT32", "shape": [1, 2], "data": [0.0, 0.0]}
    ])
    assert r.status_code == 400


# ── /v2/models/{name}/versions/{ver}/infer ────────────────────────────────────

def test_infer_versioned_endpoint(server, rest_url):
    payload = {"inputs": [
        {"name": "X", "datatype": "FP64", "shape": [1, 2], "data": [1.0, 0.5]}
    ]}
    r = requests.post(
        f"{rest_url}/v2/models/{MODEL_NAME}/versions/1/infer", json=payload
    )
    assert r.status_code == 200
    assert "outputs" in r.json()


# ── /metrics (Prometheus) ─────────────────────────────────────────────────────

def test_metrics_endpoint(server, rest_url):
    r = requests.get(f"{rest_url}/metrics")
    assert r.status_code == 200
    text = r.text
    assert "omle_models_loaded" in text
    assert "omle_requests_total" in text


def test_metrics_updated_after_infer(server, rest_url):
    # Run an inference first.
    _infer(rest_url, MODEL_NAME, [
        {"name": "X", "datatype": "FP64", "shape": [1, 2], "data": [2.0, 3.0]}
    ])
    r = requests.get(f"{rest_url}/metrics")
    assert r.status_code == 200
    assert MODEL_NAME in r.text

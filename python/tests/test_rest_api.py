"""Integration tests for all omle-server REST (OIP) endpoints.

Requires the server fixture (defined in conftest.py) to be running.
Model 'test_model_2f' has a single input 'X' (FP64, shape [-1, 2]) and
one output 'score'.
"""
from __future__ import annotations

import json
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

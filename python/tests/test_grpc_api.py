"""Integration tests for all omle-server gRPC (OIP) methods.

All 7 gRPC methods are covered:
  ServerLive, ServerReady, ModelReady, ServerMetadata, ModelMetadata,
  ModelInfer, ModelStreamInfer.

Requires the server fixture (conftest.py) and the pre-generated stubs
open_inference_grpc_pb2.py / open_inference_grpc_pb2_grpc.py in this
directory.
"""
from __future__ import annotations

import struct

import grpc
import pytest

import open_inference_grpc_pb2 as pb
import open_inference_grpc_pb2_grpc as pb_grpc

MODEL_NAME = "test_model_2f"


@pytest.fixture(scope="module")
def stub(server, grpc_channel):
    return pb_grpc.GRPCInferenceServiceStub(grpc_channel)


# ── ServerLive ────────────────────────────────────────────────────────────────

def test_grpc_server_live(stub):
    resp = stub.ServerLive(pb.ServerLiveRequest())
    assert resp.live is True


# ── ServerReady ───────────────────────────────────────────────────────────────

def test_grpc_server_ready(stub):
    resp = stub.ServerReady(pb.ServerReadyRequest())
    assert resp.ready is True


# ── ModelReady ────────────────────────────────────────────────────────────────

def test_grpc_model_ready_known(stub):
    resp = stub.ModelReady(pb.ModelReadyRequest(name=MODEL_NAME))
    assert resp.ready is True


def test_grpc_model_ready_versioned(stub):
    resp = stub.ModelReady(pb.ModelReadyRequest(name=MODEL_NAME, version="1"))
    assert resp.ready is True


def test_grpc_model_ready_unknown(stub):
    resp = stub.ModelReady(pb.ModelReadyRequest(name="does_not_exist"))
    assert resp.ready is False


def test_grpc_model_ready_wrong_version(stub):
    resp = stub.ModelReady(pb.ModelReadyRequest(name=MODEL_NAME, version="99"))
    assert resp.ready is False


# ── ServerMetadata ────────────────────────────────────────────────────────────

def test_grpc_server_metadata(stub):
    resp = stub.ServerMetadata(pb.ServerMetadataRequest())
    assert resp.name != ""
    assert resp.version != ""


# ── ModelMetadata ─────────────────────────────────────────────────────────────

def test_grpc_model_metadata_known(stub):
    resp = stub.ModelMetadata(pb.ModelMetadataRequest(name=MODEL_NAME))
    assert resp.name == MODEL_NAME
    assert len(resp.inputs) >= 1
    assert len(resp.outputs) >= 1
    inp = resp.inputs[0]
    assert inp.datatype == "FP64"
    assert 2 in inp.shape


def test_grpc_model_metadata_versioned(stub):
    resp = stub.ModelMetadata(pb.ModelMetadataRequest(name=MODEL_NAME, version="1"))
    assert resp.name == MODEL_NAME


def test_grpc_model_metadata_unknown(stub):
    with pytest.raises(grpc.RpcError) as exc:
        stub.ModelMetadata(pb.ModelMetadataRequest(name="no_such_model"))
    assert exc.value.code() == grpc.StatusCode.NOT_FOUND


# ── ModelInfer ────────────────────────────────────────────────────────────────

def _make_fp64_raw(values: list[float]) -> bytes:
    return struct.pack(f"<{len(values)}d", *values)


def _infer_grpc(stub, model, name, shape, raw_bytes):
    inp = pb.ModelInferRequest.InferInputTensor(
        name=name, datatype="FP64", shape=shape
    )
    req = pb.ModelInferRequest(
        model_name=model,
        inputs=[inp],
        raw_input_contents=[raw_bytes],
    )
    return stub.ModelInfer(req)


def test_grpc_infer_single_row(stub):
    raw = _make_fp64_raw([1.0, 2.0])
    resp = _infer_grpc(stub, MODEL_NAME, "X", [1, 2], raw)
    assert resp.model_name == MODEL_NAME
    assert len(resp.outputs) >= 1
    out = resp.outputs[0]
    assert out.shape[0] == 1  # 1 output row


def test_grpc_infer_batch(stub):
    raw = _make_fp64_raw([1.0, 2.0, 3.0, 4.0, 5.0, 6.0])
    resp = _infer_grpc(stub, MODEL_NAME, "X", [3, 2], raw)
    assert resp.outputs[0].shape[0] == 3


def test_grpc_infer_with_request_id(stub):
    raw = _make_fp64_raw([0.0, 1.0])
    inp = pb.ModelInferRequest.InferInputTensor(
        name="X", datatype="FP64", shape=[1, 2]
    )
    req = pb.ModelInferRequest(
        model_name=MODEL_NAME,
        id="grpc-req-1",
        inputs=[inp],
        raw_input_contents=[raw],
    )
    resp = stub.ModelInfer(req)
    assert resp.id == "grpc-req-1"


def test_grpc_infer_typed_contents(stub):
    """ModelInfer using typed fp64_contents instead of raw_input_contents."""
    contents = pb.InferTensorContents(fp64_contents=[1.5, 2.5])
    inp = pb.ModelInferRequest.InferInputTensor(
        name="X", datatype="FP64", shape=[1, 2], contents=contents
    )
    req = pb.ModelInferRequest(model_name=MODEL_NAME, inputs=[inp])
    resp = stub.ModelInfer(req)
    assert resp.model_name == MODEL_NAME
    assert len(resp.outputs) >= 1


def test_grpc_infer_unknown_model(stub):
    raw = _make_fp64_raw([1.0, 2.0])
    with pytest.raises(grpc.RpcError) as exc:
        _infer_grpc(stub, "no_such_model", "X", [1, 2], raw)
    assert exc.value.code() == grpc.StatusCode.NOT_FOUND


def test_grpc_infer_unknown_datatype(stub):
    inp = pb.ModelInferRequest.InferInputTensor(
        name="X", datatype="FLOAT32", shape=[1, 2]
    )
    req = pb.ModelInferRequest(model_name=MODEL_NAME, inputs=[inp])
    with pytest.raises(grpc.RpcError) as exc:
        stub.ModelInfer(req)
    assert exc.value.code() in (
        grpc.StatusCode.INVALID_ARGUMENT, grpc.StatusCode.INTERNAL
    )


# ── ModelStreamInfer ──────────────────────────────────────────────────────────

def test_grpc_stream_infer_single(stub):
    """Send one request in the stream, receive one response."""
    raw = _make_fp64_raw([1.0, 2.0])
    inp = pb.ModelInferRequest.InferInputTensor(
        name="X", datatype="FP64", shape=[1, 2]
    )
    req = pb.ModelInferRequest(
        model_name=MODEL_NAME,
        inputs=[inp],
        raw_input_contents=[raw],
    )

    def request_iter():
        yield req

    responses = list(stub.ModelStreamInfer(request_iter()))
    assert len(responses) == 1
    assert responses[0].model_name == MODEL_NAME


def test_grpc_stream_infer_multiple(stub):
    """Send multiple requests in one stream, get one response per request."""
    def _req(v1, v2):
        raw = _make_fp64_raw([v1, v2])
        inp = pb.ModelInferRequest.InferInputTensor(
            name="X", datatype="FP64", shape=[1, 2]
        )
        return pb.ModelInferRequest(
            model_name=MODEL_NAME,
            inputs=[inp],
            raw_input_contents=[raw],
        )

    def request_iter():
        yield _req(1.0, 2.0)
        yield _req(3.0, 4.0)
        yield _req(5.0, 6.0)

    responses = list(stub.ModelStreamInfer(request_iter()))
    assert len(responses) == 3
    for resp in responses:
        assert resp.model_name == MODEL_NAME
        assert len(resp.outputs) >= 1

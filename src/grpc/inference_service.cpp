#include "grpc/inference_service.h"

#include <chrono>
#include <iostream>

#include "omle_server/metrics.h"
#include "omle_server/oip_codec.h"

namespace omle_server {

InferenceServiceImpl::InferenceServiceImpl(
    std::shared_ptr<ModelRegistry> registry,
    std::shared_ptr<BatcherRegistry> batchers, const ServerConfig& cfg)
    : registry_(std::move(registry)),
      batchers_(std::move(batchers)),
      cfg_(cfg) {}

// ── Health
// ────────────────────────────────────────────────────────────────────

grpc::Status InferenceServiceImpl::ServerLive(
    grpc::ServerContext*, const inference::ServerLiveRequest*,
    inference::ServerLiveResponse* resp) {
  resp->set_live(true);
  return grpc::Status::OK;
}

grpc::Status InferenceServiceImpl::ServerReady(
    grpc::ServerContext*, const inference::ServerReadyRequest*,
    inference::ServerReadyResponse* resp) {
  resp->set_ready(true);
  return grpc::Status::OK;
}

grpc::Status InferenceServiceImpl::ModelReady(
    grpc::ServerContext*, const inference::ModelReadyRequest* req,
    inference::ModelReadyResponse* resp) {
  resp->set_ready(registry_->is_ready(req->name(), req->version()));
  return grpc::Status::OK;
}

// ── Metadata
// ──────────────────────────────────────────────────────────────────

grpc::Status InferenceServiceImpl::ServerMetadata(
    grpc::ServerContext*, const inference::ServerMetadataRequest*,
    inference::ServerMetadataResponse* resp) {
  resp->set_name(cfg_.server_name);
  resp->set_version(cfg_.server_version);
  resp->add_extensions("model-repository");
  return grpc::Status::OK;
}

grpc::Status InferenceServiceImpl::ModelMetadata(
    grpc::ServerContext*, const inference::ModelMetadataRequest* req,
    inference::ModelMetadataResponse* resp) {
  auto model = registry_->get(req->name(), req->version());
  if (!model) return {grpc::NOT_FOUND, "model '" + req->name() + "' not found"};

  resp->set_name(req->name());
  resp->set_platform("omle");
  for (auto& v : registry_->versions(req->name())) resp->add_versions(v);

  for (auto& s : model->inputs()) {
    auto* t = resp->add_inputs();
    t->set_name(s.name);
    t->set_datatype(std::string(OipCodec::omle_to_oip_dtype(s.dtype)));
    for (int64_t d : s.shape) t->add_shape(d);
  }
  for (auto& s : model->outputs()) {
    auto* t = resp->add_outputs();
    t->set_name(s.name);
    t->set_datatype(std::string(OipCodec::omle_to_oip_dtype(s.dtype)));
    for (int64_t d : s.shape) t->add_shape(d);
  }
  return grpc::Status::OK;
}

// ── Inference
// ─────────────────────────────────────────────────────────────────

grpc::Status InferenceServiceImpl::ModelInfer(
    grpc::ServerContext*, const inference::ModelInferRequest* req,
    inference::ModelInferResponse* resp) {
  const auto& mname = req->model_name();
  const auto& mver = req->model_version();

  auto model = registry_->get(mname, mver);
  if (!model) return {grpc::NOT_FOUND, "model '" + mname + "' not found"};

  auto inputs_or = OipCodec::decode_grpc_request(*req);
  if (!inputs_or.ok()) return {grpc::INVALID_ARGUMENT, inputs_or.message()};

  std::vector<std::string> filter;
  filter.reserve(req->outputs_size());
  for (int i = 0; i < req->outputs_size(); ++i)
    filter.push_back(req->outputs(i).name());

  auto t0 = std::chrono::steady_clock::now();

  // Acquire future — from batcher or direct async.
  std::future<InferResult> fut;
  if (auto* batcher = batchers_->get(mname, mver)) {
    fut = batcher->submit(std::move(*inputs_or), filter);
  } else {
    auto m2 = model;
    auto in = std::move(*inputs_or);
    fut = std::async(std::launch::async,
                     [m2, in = std::move(in), filter]() mutable {
                       return m2->predict(in, filter);
                     });
  }

  // Block with timeout (gRPC thread is a real thread — blocking is fine).
  bool timed_out = false;
  if (cfg_.inference_timeout_ms > 0) {
    auto status =
        fut.wait_for(std::chrono::milliseconds(cfg_.inference_timeout_ms));
    timed_out = (status == std::future_status::timeout);
  }

  double dur =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  if (timed_out) {
    Metrics::instance().record_timeout(mname);
    return {grpc::DEADLINE_EXCEEDED,
            "inference timeout after " +
                std::to_string(cfg_.inference_timeout_ms) + "ms"};
  }

  auto result = fut.get();
  Metrics::instance().record_request(mname, result.ok(), dur);

  if (!result.ok()) return {grpc::INTERNAL, result.message()};

  OipCodec::encode_grpc_response(mname, mver, req->id(), *result,
                                 model->outputs(), resp);
  return grpc::Status::OK;
}

grpc::Status InferenceServiceImpl::ModelStreamInfer(
    grpc::ServerContext* ctx,
    grpc::ServerReaderWriter<inference::ModelInferResponse,
                             inference::ModelInferRequest>* stream) {
  inference::ModelInferRequest req;
  while (stream->Read(&req)) {
    inference::ModelInferResponse resp;
    auto status = ModelInfer(ctx, &req, &resp);
    if (!status.ok()) return status;
    stream->Write(resp);
  }
  return grpc::Status::OK;
}

// ── Server startup
// ────────────────────────────────────────────────────────────

std::unique_ptr<grpc::Server> start_grpc_server(
    std::shared_ptr<ModelRegistry> registry,
    std::shared_ptr<BatcherRegistry> batchers, const ServerConfig& cfg) {
  auto* service =
      new InferenceServiceImpl(std::move(registry), std::move(batchers), cfg);

  int threads = cfg.grpc_threads > 0 ? cfg.grpc_threads
                                     : (int)std::thread::hardware_concurrency();
  grpc::ResourceQuota rq;
  rq.SetMaxThreads(threads);

  grpc::ServerBuilder builder;
  builder.SetResourceQuota(rq);
  builder.AddListeningPort("0.0.0.0:" + std::to_string(cfg.grpc_port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(service);

  auto server = builder.BuildAndStart();
  std::cout << "[gRPC] Listening on port " << cfg.grpc_port << " (" << threads
            << " threads)\n";
  return server;
}

}  // namespace omle_server

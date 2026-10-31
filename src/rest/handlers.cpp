#include "rest/handlers.h"

#include <drogon/drogon.h>

#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>

#include "omle_server/batcher.h"
#include "omle_server/metrics.h"
#include "omle_server/oip_codec.h"

using json = nlohmann::json;
using namespace drogon;

namespace omle_server {

namespace {

// ── Response helpers
// ──────────────────────────────────────────────────────────

HttpResponsePtr json_response(const std::string& body,
                              HttpStatusCode code = k200OK) {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(code);
  resp->setContentTypeCode(CT_APPLICATION_JSON);
  resp->setBody(body);
  return resp;
}

HttpResponsePtr error_response(HttpStatusCode code, const std::string& msg) {
  return json_response("{\"error\":\"" + msg + "\"}", code);
}

// ── Core inference helper (used by both handle_infer and REST batch)
// ──────────

// Submit an inference, wait with timeout in a detached thread, then invoke cb.
// If timeout_ms <= 0, no timeout is applied.
void async_infer(std::future<InferResult> fut, const std::string& model_name,
                 const std::string& model_version,
                 const std::string& request_id,
                 const std::vector<omle::rt::OutputSpec> output_specs,
                 int timeout_ms, std::chrono::steady_clock::time_point t0,
                 std::function<void(const HttpResponsePtr&)> cb) {
  std::thread([fut = std::move(fut), model_name, model_version, request_id,
               output_specs, timeout_ms, t0, cb = std::move(cb)]() mutable {
    bool timed_out = false;
    if (timeout_ms > 0) {
      auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
      timed_out = (status == std::future_status::timeout);
    }

    double dur =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    if (timed_out) {
      // Both: the specific timeout counter, and the request total, which a
      // timeout previously escaped entirely.
      Metrics::instance().record_timeout(model_name, model_version);
      Metrics::instance().record_rejected(model_name, model_version);
      cb(error_response(
          k504GatewayTimeout,
          "inference timeout after " + std::to_string(timeout_ms) + "ms"));
      return;
    }

    auto result = fut.get();
    Metrics::instance().record_request(model_name, model_version, result.ok(),
                                       dur);

    if (!result.ok()) {
      cb(error_response(k500InternalServerError, result.message()));
      return;
    }
    cb(json_response(OipCodec::build_infer_response_json(
        model_name, request_id, *result, output_specs)));
  }).detach();
}

// ── Health
// ────────────────────────────────────────────────────────────────────

void handle_server_live(const HttpRequestPtr&,
                        std::function<void(const HttpResponsePtr&)>&& cb) {
  cb(json_response("{}"));
}

void handle_server_ready(const HttpRequestPtr&,
                         std::function<void(const HttpResponsePtr&)>&& cb) {
  cb(json_response("{}"));
}

void handle_model_ready(const HttpRequestPtr&,
                        std::function<void(const HttpResponsePtr&)>&& cb,
                        std::shared_ptr<ModelRegistry> registry,
                        const std::string& model_name,
                        const std::string& version) {
  if (!registry->is_ready(model_name, version)) {
    cb(error_response(k503ServiceUnavailable, "model not ready"));
    return;
  }
  cb(json_response("{}"));
}

// ── Server metadata
// ───────────────────────────────────────────────────────────

void handle_server_metadata(const HttpRequestPtr&,
                            std::function<void(const HttpResponsePtr&)>&& cb,
                            const ServerConfig& cfg) {
  // extensions is empty, and deliberately so. It names the OIP extensions this
  // server implements, and this server implements none: there are no
  // /v2/repository/* routes, the proto declares no repository RPCs, and
  // ModelRegistry is built once by a static load() with no add, remove or
  // refresh — so model-repository is not a missing handler but a missing
  // capability. Advertising it sent a conformant client to endpoints that 404.
  //
  // json::array() rather than {} because nlohmann would serialise an empty
  // initialiser list as {} — an object, where the schema says array.
  json body = {{"name", cfg.server_name},
               {"version", cfg.server_version},
               {"extensions", json::array()}};
  cb(json_response(body.dump()));
}

// ── Model metadata
// ────────────────────────────────────────────────────────────

void handle_model_metadata(const HttpRequestPtr&,
                           std::function<void(const HttpResponsePtr&)>&& cb,
                           std::shared_ptr<ModelRegistry> registry,
                           const std::string& model_name,
                           const std::string& version) {
  auto model = registry->get(model_name, version);
  if (!model) {
    cb(error_response(k404NotFound, "model '" + model_name + "' not found"));
    return;
  }

  json body;
  body["name"] = model_name;
  body["platform"] = "omle";
  body["versions"] = registry->versions(model_name);

  body["inputs"] = json::array();
  for (auto& s : model->inputs())
    body["inputs"].push_back(
        {{"name", s.name},
         {"datatype", OipCodec::omle_to_oip_dtype(s.dtype)},
         {"shape", s.shape}});
  body["outputs"] = json::array();
  for (auto& s : model->outputs())
    body["outputs"].push_back(
        {{"name", s.name},
         {"datatype", OipCodec::omle_to_oip_dtype(s.dtype)},
         {"shape", s.shape}});
  cb(json_response(body.dump()));
}

// ── Prometheus metrics
// ────────────────────────────────────────────────────────

void handle_metrics(const HttpRequestPtr&,
                    std::function<void(const HttpResponsePtr&)>&& cb) {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(k200OK);
  resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
  resp->setBody(Metrics::instance().prometheus_text());
  cb(resp);
}

// ── Inference
// ─────────────────────────────────────────────────────────────────

void handle_infer(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& cb,
                  std::shared_ptr<ModelRegistry> registry,
                  std::shared_ptr<BatcherRegistry> batchers,
                  const ServerConfig& cfg, const std::string& model_name,
                  const std::string& version) {
  auto model = registry->get(model_name, version);
  if (!model) {
    // Not model_name: it is caller-controlled and names nothing here.
    Metrics::instance().record_rejected(Metrics::kUnknownModel, "");
    cb(error_response(k404NotFound, "model '" + model_name + "' not found"));
    return;
  }

  // What actually ran, not what was asked for: a request for "" and one for "1"
  // that resolve to the same model must share a series.
  const std::string resolved = registry->resolve_version(model_name, version);

  auto inputs_or = OipCodec::parse_infer_request_json(req->body());
  if (!inputs_or.ok()) {
    Metrics::instance().record_rejected(model_name, resolved);
    cb(error_response(k400BadRequest, inputs_or.message()));
    return;
  }

  // Extract optional fields from the request body (output filter, request id).
  std::vector<std::string> output_filter;
  std::string request_id;
  try {
    auto j = json::parse(req->body());
    if (j.contains("id")) request_id = j["id"].get<std::string>();
    if (j.contains("outputs"))
      for (auto& o : j["outputs"])
        if (o.contains("name"))
          output_filter.push_back(o["name"].get<std::string>());
  } catch (...) {
  }

  auto t0 = std::chrono::steady_clock::now();

  // Acquire inference future.
  std::future<InferResult> fut;
  if (auto* batcher = batchers->get(model_name, version)) {
    fut = batcher->submit(std::move(*inputs_or), output_filter);
  } else {
    auto m2 = model;
    auto in = std::move(*inputs_or);
    fut = std::async(std::launch::async,
                     [m2, in = std::move(in), output_filter]() mutable {
                       return m2->predict(in, output_filter);
                     });
  }

  auto output_specs = model->outputs();
  async_infer(std::move(fut), model_name, resolved, request_id, output_specs,
              cfg.inference_timeout_ms, t0, std::move(cb));
}

}  // namespace

// ── Route registration
// ────────────────────────────────────────────────────────

void register_rest_handlers(std::shared_ptr<ModelRegistry> registry,
                            std::shared_ptr<BatcherRegistry> batchers,
                            const ServerConfig& cfg) {
  auto& app = drogon::app();

  app.registerHandler("/v2/health/live",
                      [](const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& cb) {
                        handle_server_live(req, std::move(cb));
                      },
                      {Get});

  app.registerHandler("/v2/health/ready",
                      [](const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& cb) {
                        handle_server_ready(req, std::move(cb));
                      },
                      {Get});

  // Prometheus metrics endpoint.
  app.registerHandler("/metrics",
                      [](const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& cb) {
                        handle_metrics(req, std::move(cb));
                      },
                      {Get});

  ServerConfig cfg_copy = cfg;
  app.registerHandler(
      "/v2",
      [cfg_copy](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& cb) {
        handle_server_metadata(req, std::move(cb), cfg_copy);
      },
      {Get});

  app.registerHandler(
      "/v2/models/{model_name}/ready",
      [registry](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& cb,
                 const std::string& model_name) {
        handle_model_ready(req, std::move(cb), registry, model_name, "");
      },
      {Get});

  app.registerHandler(
      "/v2/models/{model_name}/versions/{version}/ready",
      [registry](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& cb,
                 const std::string& model_name, const std::string& version) {
        handle_model_ready(req, std::move(cb), registry, model_name, version);
      },
      {Get});

  app.registerHandler(
      "/v2/models/{model_name}",
      [registry](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& cb,
                 const std::string& model_name) {
        handle_model_metadata(req, std::move(cb), registry, model_name, "");
      },
      {Get});

  app.registerHandler(
      "/v2/models/{model_name}/versions/{version}",
      [registry](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& cb,
                 const std::string& model_name, const std::string& version) {
        handle_model_metadata(req, std::move(cb), registry, model_name,
                              version);
      },
      {Get});

  app.registerHandler("/v2/models/{model_name}/infer",
                      [registry, batchers, cfg_copy](
                          const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& cb,
                          const std::string& model_name) {
                        handle_infer(req, std::move(cb), registry, batchers,
                                     cfg_copy, model_name, "");
                      },
                      {Post});

  app.registerHandler(
      "/v2/models/{model_name}/versions/{version}/infer",
      [registry, batchers, cfg_copy](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& cb,
          const std::string& model_name, const std::string& version) {
        handle_infer(req, std::move(cb), registry, batchers, cfg_copy,
                     model_name, version);
      },
      {Post});
}

}  // namespace omle_server

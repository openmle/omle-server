#include <drogon/drogon.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "grpc/inference_service.h"
#include "omle_server/batcher.h"
#include "omle_server/metrics.h"
#include "omle_server/model_registry.h"
#include "omle_server/server_config.h"
#include "rest/handlers.h"

using json = nlohmann::json;

// ── ServerConfig loader
// ───────────────────────────────────────────────────────

namespace omle_server {

ServerConfig ServerConfig::from_file(const std::string& path) {
  ServerConfig cfg;
  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "[Config] " << path << " not found — using defaults\n";
    return cfg;
  }
  try {
    json j;
    f >> j;
    if (j.contains("model_dir")) cfg.model_dir = j["model_dir"];
    if (j.contains("rest_port")) cfg.rest_port = j["rest_port"];
    if (j.contains("grpc_port")) cfg.grpc_port = j["grpc_port"];
    if (j.contains("rest_threads")) cfg.rest_threads = j["rest_threads"];
    if (j.contains("grpc_threads")) cfg.grpc_threads = j["grpc_threads"];
    if (j.contains("model_n_threads"))
      cfg.model_n_threads = j["model_n_threads"];
    if (j.contains("run_verification"))
      cfg.run_verification = j["run_verification"];
    if (j.contains("batching_enabled"))
      cfg.batching_enabled = j["batching_enabled"];
    if (j.contains("max_batch_size")) cfg.max_batch_size = j["max_batch_size"];
    if (j.contains("batch_timeout_ms"))
      cfg.batch_timeout_ms = j["batch_timeout_ms"];
    if (j.contains("inference_timeout_ms"))
      cfg.inference_timeout_ms = j["inference_timeout_ms"];
    if (j.contains("log_level")) cfg.log_level = j["log_level"];
    if (j.contains("server_name")) cfg.server_name = j["server_name"];
    if (j.contains("server_version")) cfg.server_version = j["server_version"];
    if (j.contains("model_configs")) {
      for (auto& [mname, mv] : j["model_configs"].items()) {
        ModelBatchConfig mc;
        if (mv.contains("max_batch_size"))
          mc.max_batch_size = mv["max_batch_size"];
        if (mv.contains("batch_timeout_ms"))
          mc.batch_timeout_ms = mv["batch_timeout_ms"];
        cfg.model_configs[mname] = mc;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "[Config] Parse error: " << e.what() << " — using defaults\n";
  }
  return cfg;
}

}  // namespace omle_server

// ── main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  std::string config_path = "configs/server.json";
  if (argc >= 2) config_path = argv[1];

  auto cfg = omle_server::ServerConfig::from_file(config_path);

  if (const char* v = std::getenv("OMLE_MODEL_DIR")) cfg.model_dir = v;
  if (const char* v = std::getenv("OMLE_REST_PORT"))
    cfg.rest_port = std::atoi(v);
  if (const char* v = std::getenv("OMLE_GRPC_PORT"))
    cfg.grpc_port = std::atoi(v);

  std::cout << "[Server] model_dir=" << cfg.model_dir
            << "  rest=" << cfg.rest_port << "  grpc=" << cfg.grpc_port
            << "  batching=" << (cfg.batching_enabled ? "on" : "off")
            << "  timeout_ms=" << cfg.inference_timeout_ms << "\n";

  omle::rt::LoadOptions load_opts;
  load_opts.n_threads = cfg.model_n_threads;
  load_opts.run_verification = cfg.run_verification;

  auto registry_or = omle_server::ModelRegistry::load(cfg.model_dir, load_opts);
  if (!registry_or.ok())
    std::cerr << "[Server] Warning: " << registry_or.message()
              << " — continuing with empty registry\n";

  std::shared_ptr<omle_server::ModelRegistry> registry;
  if (registry_or.ok()) {
    registry = std::move(*registry_or);
  } else {
    auto empty = omle_server::ModelRegistry::load("/tmp", load_opts);
    if (!empty.ok()) {
      std::cerr << "[Server] Fatal: cannot init registry\n";
      return 1;
    }
    registry = std::move(*empty);
  }

  // Update models-loaded gauge.
  omle_server::Metrics::instance().set_models_loaded(
      static_cast<int>(registry->model_names().size()));

  // Build batcher registry (no-op when batching disabled).
  auto batchers = std::make_shared<omle_server::BatcherRegistry>(registry, cfg);

  // Start gRPC server in background.
  auto grpc_server = omle_server::start_grpc_server(registry, batchers, cfg);

  // Register Drogon REST routes.
  omle_server::register_rest_handlers(registry, batchers, cfg);

  int threads = cfg.rest_threads > 0 ? cfg.rest_threads
                                     : (int)std::thread::hardware_concurrency();
  std::cout << "[REST] Listening on port " << cfg.rest_port << " (" << threads
            << " threads)\n";

  drogon::app()
      .setLogLevel(trantor::Logger::kInfo)
      .setThreadNum(threads)
      .addListener("0.0.0.0", cfg.rest_port)
      .run();

  grpc_server->Shutdown();
  return 0;
}

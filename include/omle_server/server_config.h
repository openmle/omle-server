#ifndef OMLE_SERVER_SERVER_CONFIG_H_
#define OMLE_SERVER_SERVER_CONFIG_H_

#include <string>
#include <unordered_map>

namespace omle_server {

// Per-model batching overrides.  -1 means "use the global default".
struct ModelBatchConfig {
  int max_batch_size = -1;
  int batch_timeout_ms = -1;
};

struct ServerConfig {
  // Network
  std::string model_dir = "/models";
  int rest_port = 8080;
  int grpc_port = 8081;
  int rest_threads = 0;  // 0 = hardware_concurrency
  int grpc_threads = 0;

  // Model loading
  int model_n_threads = 1;
  bool run_verification = true;

  // Dynamic batching (global defaults)
  bool batching_enabled = false;
  int max_batch_size = 32;
  int batch_timeout_ms = 5;  // max wait before flushing a partial batch

  // Per-model overrides: key is model name (version-independent).
  std::unordered_map<std::string, ModelBatchConfig> model_configs;

  // Per-request inference timeout
  int inference_timeout_ms = 30000;  // 0 = no timeout

  // Misc
  std::string log_level = "info";
  std::string server_name = "omle-server";
  std::string server_version = "0.1.0";

  static ServerConfig from_file(const std::string& path);
};

}  // namespace omle_server

#endif  // OMLE_SERVER_SERVER_CONFIG_H_

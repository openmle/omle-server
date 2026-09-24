#include <drogon/drogon.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
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

// ── CLI ─────────────────────────────────────────────────────────────────────
//
// Settings come from four places. Later wins:
//
//   1. built-in defaults          (ServerConfig's member initialisers)
//   2. config file                --config, or configs/server.json
//   3. environment                OMLE_*
//   4. command-line flags         --model-dir, --rest-port, ...
//
// Environment sits under flags so a container image can set a baseline that
// `docker run` can still override without rewriting the config file.

namespace {

constexpr const char* kDefaultConfigPath = "configs/server.json";

void print_help(std::ostream& os) {
  os << R"(omle-server — OIP inference server

Usage:
  omle-server [serve] [options]      Start the server
  omle-server init-config [options]  Write a default config file
  omle-server --help | --version

Serve options:
  -c, --config PATH          Config file to read
                             (default: )"
     << kDefaultConfigPath << R"(; missing is not an error)
      --model-dir PATH       Directory scanned for .omle models
      --rest-port N          REST listen port
      --grpc-port N          gRPC listen port
      --rest-threads N       REST worker threads (0 = one per core)
      --grpc-threads N       gRPC worker threads (0 = one per core)
      --model-threads N      Threads each model may use internally
      --batching             Enable dynamic batching
      --no-batching          Disable dynamic batching
      --max-batch-size N     Largest batch to dispatch
      --batch-timeout-ms N   Longest wait before flushing a partial batch
      --inference-timeout-ms N
                             Per-request timeout (0 = none)
      --log-level LEVEL      trace, debug, info, warn, error
      --no-verification      Skip model verification on load (faster startup)

init-config options:
  -o, --output PATH          Where to write (default: )"
     << kDefaultConfigPath << R"()
      --force                Overwrite an existing file

Environment (overridden by the flags above):
  OMLE_CONFIG                Config file path
  OMLE_MODEL_DIR             Model directory
  OMLE_REST_PORT             REST port
  OMLE_GRPC_PORT             gRPC port
  OMLE_REST_THREADS          REST worker threads
  OMLE_GRPC_THREADS          gRPC worker threads
  OMLE_MODEL_THREADS         Per-model threads
  OMLE_LOG_LEVEL             Log level

Examples:
  omle-server --model-dir ./models --rest-port 9000
  omle-server init-config -o server.json && omle-server -c server.json
)";
}

// Serialised from a live ServerConfig rather than a stored template, so the
// generated file cannot drift from the struct's defaults. Round-trips through
// from_file() above: every key here is one that loader reads.
json to_json(const omle_server::ServerConfig& cfg) {
  json j;
  j["model_dir"] = cfg.model_dir;
  j["rest_port"] = cfg.rest_port;
  j["grpc_port"] = cfg.grpc_port;
  j["rest_threads"] = cfg.rest_threads;
  j["grpc_threads"] = cfg.grpc_threads;
  j["model_n_threads"] = cfg.model_n_threads;
  j["run_verification"] = cfg.run_verification;
  j["batching_enabled"] = cfg.batching_enabled;
  j["max_batch_size"] = cfg.max_batch_size;
  j["batch_timeout_ms"] = cfg.batch_timeout_ms;
  j["inference_timeout_ms"] = cfg.inference_timeout_ms;
  j["log_level"] = cfg.log_level;
  j["server_name"] = cfg.server_name;
  j["server_version"] = cfg.server_version;
  // An empty object, not an example: a generated file should start a server
  // that works, and per-model overrides naming models the user does not have
  // would only be noise to delete.
  j["model_configs"] = json::object();
  return j;
}

int cmd_init_config(const std::string& path, bool force) {
  {
    std::ifstream probe(path);
    if (probe.is_open() && !force) {
      std::cerr << "omle-server: " << path
                << " already exists; pass --force to overwrite\n";
      return 1;
    }
  }
  std::ofstream out(path);
  if (!out.is_open()) {
    std::cerr << "omle-server: cannot write " << path << "\n";
    return 1;
  }
  out << to_json(omle_server::ServerConfig{}).dump(2) << "\n";
  if (!out) {
    std::cerr << "omle-server: failed while writing " << path << "\n";
    return 1;
  }
  std::cout << "Wrote " << path << "\n";
  return 0;
}

// Reads the value for a flag that takes one, supporting both `--flag value`
// and `--flag=value`. Advances i past whatever it consumed.
bool take_value(int argc, char** argv, int& i, const std::string& arg,
                const std::string& name, std::string* out) {
  if (arg == name) {
    if (i + 1 >= argc) {
      std::cerr << "omle-server: " << name << " needs a value\n";
      return false;
    }
    *out = argv[++i];
    return true;
  }
  *out = arg.substr(name.size() + 1);  // arg is "name=value"
  return true;
}

bool parse_int(const std::string& text, const std::string& flag, int* out) {
  try {
    size_t used = 0;
    int value = std::stoi(text, &used);
    if (used != text.size()) throw std::invalid_argument("trailing");
    *out = value;
    return true;
  } catch (const std::exception&) {
    std::cerr << "omle-server: " << flag << " expects a number, got '" << text
              << "'\n";
    return false;
  }
}

}  // namespace

// ── main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  std::string config_path = kDefaultConfigPath;
  if (const char* v = std::getenv("OMLE_CONFIG")) config_path = v;

  // Flags are collected before the config file is read, because --config
  // decides which file that is. They are applied after it, last, so they win.
  struct Overrides {
    std::optional<std::string> model_dir, log_level;
    std::optional<int> rest_port, grpc_port, rest_threads, grpc_threads,
        model_threads, max_batch_size, batch_timeout_ms, inference_timeout_ms;
    std::optional<bool> batching, verification;
  } ov;

  int argi = 1;
  bool init_config = false;
  std::string init_output = kDefaultConfigPath;
  bool init_force = false;

  if (argi < argc) {
    std::string first = argv[argi];
    if (first == "serve") {
      ++argi;
    } else if (first == "init-config") {
      init_config = true;
      ++argi;
    } else if (!first.empty() && first[0] != '-') {
      // Backwards compatibility: the first release took a bare config path.
      // Still honoured so existing invocations keep working, but --config is
      // what the help advertises.
      config_path = first;
      ++argi;
    }
  }

  for (; argi < argc; ++argi) {
    const std::string arg = argv[argi];
    const auto is = [&arg](const char* name) {
      const std::string n(name);
      return arg == n || arg.rfind(n + std::string("="), 0) == 0;
    };
    std::string value;
    int number = 0;

    if (arg == "-h" || arg == "--help") {
      print_help(std::cout);
      return 0;
    } else if (arg == "-V" || arg == "--version") {
      std::cout << omle_server::ServerConfig{}.server_version << "\n";
      return 0;
    } else if (is("-c") || is("--config")) {
      if (!take_value(argc, argv, argi, arg, arg[1] == '-' ? "--config" : "-c",
                      &value))
        return 2;
      config_path = value;
    } else if (is("-o") || is("--output")) {
      if (!take_value(argc, argv, argi, arg, arg[1] == '-' ? "--output" : "-o",
                      &value))
        return 2;
      init_output = value;
    } else if (arg == "--force") {
      init_force = true;
    } else if (is("--model-dir")) {
      if (!take_value(argc, argv, argi, arg, "--model-dir", &value)) return 2;
      ov.model_dir = value;
    } else if (is("--log-level")) {
      if (!take_value(argc, argv, argi, arg, "--log-level", &value)) return 2;
      ov.log_level = value;
    } else if (is("--rest-port")) {
      if (!take_value(argc, argv, argi, arg, "--rest-port", &value) ||
          !parse_int(value, "--rest-port", &number))
        return 2;
      ov.rest_port = number;
    } else if (is("--grpc-port")) {
      if (!take_value(argc, argv, argi, arg, "--grpc-port", &value) ||
          !parse_int(value, "--grpc-port", &number))
        return 2;
      ov.grpc_port = number;
    } else if (is("--rest-threads")) {
      if (!take_value(argc, argv, argi, arg, "--rest-threads", &value) ||
          !parse_int(value, "--rest-threads", &number))
        return 2;
      ov.rest_threads = number;
    } else if (is("--grpc-threads")) {
      if (!take_value(argc, argv, argi, arg, "--grpc-threads", &value) ||
          !parse_int(value, "--grpc-threads", &number))
        return 2;
      ov.grpc_threads = number;
    } else if (is("--model-threads")) {
      if (!take_value(argc, argv, argi, arg, "--model-threads", &value) ||
          !parse_int(value, "--model-threads", &number))
        return 2;
      ov.model_threads = number;
    } else if (is("--max-batch-size")) {
      if (!take_value(argc, argv, argi, arg, "--max-batch-size", &value) ||
          !parse_int(value, "--max-batch-size", &number))
        return 2;
      ov.max_batch_size = number;
    } else if (is("--batch-timeout-ms")) {
      if (!take_value(argc, argv, argi, arg, "--batch-timeout-ms", &value) ||
          !parse_int(value, "--batch-timeout-ms", &number))
        return 2;
      ov.batch_timeout_ms = number;
    } else if (is("--inference-timeout-ms")) {
      if (!take_value(argc, argv, argi, arg, "--inference-timeout-ms",
                      &value) ||
          !parse_int(value, "--inference-timeout-ms", &number))
        return 2;
      ov.inference_timeout_ms = number;
    } else if (arg == "--batching") {
      ov.batching = true;
    } else if (arg == "--no-batching") {
      ov.batching = false;
    } else if (arg == "--no-verification") {
      ov.verification = false;
    } else {
      std::cerr << "omle-server: unknown option '" << arg
                << "'\nTry 'omle-server --help'.\n";
      return 2;
    }
  }

  if (init_config) return cmd_init_config(init_output, init_force);

  auto cfg = omle_server::ServerConfig::from_file(config_path);

  // Environment: above the file, below the flags.
  const auto env_str = [](const char* name, std::string* out) {
    if (const char* v = std::getenv(name)) *out = v;
  };
  const auto env_int = [](const char* name, int* out) {
    if (const char* v = std::getenv(name)) {
      int parsed = 0;
      if (parse_int(v, name, &parsed)) *out = parsed;
    }
  };
  env_str("OMLE_MODEL_DIR", &cfg.model_dir);
  env_str("OMLE_LOG_LEVEL", &cfg.log_level);
  env_int("OMLE_REST_PORT", &cfg.rest_port);
  env_int("OMLE_GRPC_PORT", &cfg.grpc_port);
  env_int("OMLE_REST_THREADS", &cfg.rest_threads);
  env_int("OMLE_GRPC_THREADS", &cfg.grpc_threads);
  env_int("OMLE_MODEL_THREADS", &cfg.model_n_threads);

  if (ov.model_dir) cfg.model_dir = *ov.model_dir;
  if (ov.log_level) cfg.log_level = *ov.log_level;
  if (ov.rest_port) cfg.rest_port = *ov.rest_port;
  if (ov.grpc_port) cfg.grpc_port = *ov.grpc_port;
  if (ov.rest_threads) cfg.rest_threads = *ov.rest_threads;
  if (ov.grpc_threads) cfg.grpc_threads = *ov.grpc_threads;
  if (ov.model_threads) cfg.model_n_threads = *ov.model_threads;
  if (ov.max_batch_size) cfg.max_batch_size = *ov.max_batch_size;
  if (ov.batch_timeout_ms) cfg.batch_timeout_ms = *ov.batch_timeout_ms;
  if (ov.inference_timeout_ms)
    cfg.inference_timeout_ms = *ov.inference_timeout_ms;
  if (ov.batching) cfg.batching_enabled = *ov.batching;
  if (ov.verification) cfg.run_verification = *ov.verification;

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

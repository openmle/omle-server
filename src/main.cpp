#include <drogon/drogon.h>

#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "grpc/inference_service.h"
#include "omle_server/batcher.h"
#include "omle_server/build_info.h"
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

// Which sibling sources went into this binary, alongside its own version.
//
// publish.yml resolves omle-runtime and omle to their latest GitHub release at
// release time rather than pinning them, so two builds of the same omle-server
// tag can legitimately differ. Printing the refs lets a deployed binary answer
// "built against what?" by itself, instead of that living in a CI log that
// expires.
//
// A local build that did not pass them reports "unknown", which is accurate.
// Tags are written "v0.2.0" but read as versions here, so a leading v is
// dropped. Anything else is left alone: a test build reports "main@a1b2c3d" and
// a plain local build "unknown", neither of which should be rewritten.
std::string as_version(const std::string& ref) {
  if (ref.size() > 1 && ref[0] == 'v' &&
      std::isdigit(static_cast<unsigned char>(ref[1])))
    return ref.substr(1);
  return ref;
}

void print_version(std::ostream& os) {
  os << OMLE_SERVER_VERSION
     << " (built on omle=" << as_version(OMLE_SERVER_OMLE_REF)
     << ", runtime=" << as_version(OMLE_SERVER_OMLE_RUNTIME_REF) << ")\n";
}

void print_help(std::ostream& os) {
  os << R"(omle-server — OIP inference server

Usage:
  omle-server [serve] [options]      Start the server
  omle-server init-config [options]  Write a default config file
  omle-server --help | --version

--version also reports the omle-runtime and omle sources this binary was
built against, which a release resolves rather than pins.

Serve options:
  -c, --config PATH          Config file to read
                             (default: )"
     << kDefaultConfigPath << R"(; missing is not an error)
      --model-dir PATH       Directory scanned for .omle models
      --model PATH           Serve a single .omle file, named after its
                             stem. Mutually exclusive with --model-dir
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
    std::optional<std::string> model_dir, model, log_level;
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
      print_version(std::cout);
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
    } else if (is("--model")) {
      if (!take_value(argc, argv, argi, arg, "--model", &value)) return 2;
      ov.model = value;
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

  // Whether model_dir was chosen, or is just the built-in default. A directory
  // the operator named and that does not exist is a mistake worth stopping for;
  // the default being absent is an ordinary state — a container whose volume
  // has not mounted yet, say — so that only warns.
  //
  // Comparing against a default-constructed config catches the config file. A
  // file that sets model_dir to the default value reads as "not explicit",
  // which is harmless: both mean the same directory.
  bool model_dir_explicit =
      cfg.model_dir != omle_server::ServerConfig{}.model_dir;

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
  if (std::getenv("OMLE_MODEL_DIR")) model_dir_explicit = true;

  // A single model file, for serving one model without arranging a directory
  // around it. Deliberately not a ServerConfig field: a config file describing
  // a deployment names a directory, and this exists for a one-off check.
  std::optional<std::string> model_file;
  if (const char* v = std::getenv("OMLE_MODEL")) model_file = v;
  env_str("OMLE_LOG_LEVEL", &cfg.log_level);
  env_int("OMLE_REST_PORT", &cfg.rest_port);
  env_int("OMLE_GRPC_PORT", &cfg.grpc_port);
  env_int("OMLE_REST_THREADS", &cfg.rest_threads);
  env_int("OMLE_GRPC_THREADS", &cfg.grpc_threads);
  env_int("OMLE_MODEL_THREADS", &cfg.model_n_threads);

  if (ov.model_dir) {
    cfg.model_dir = *ov.model_dir;
    model_dir_explicit = true;
  }
  if (ov.model) model_file = *ov.model;

  // Both would be an ambiguous request, and guessing which one wins is worse
  // than saying so: one names a single file, the other a tree to scan.
  if (model_file && (ov.model_dir || std::getenv("OMLE_MODEL_DIR"))) {
    std::cerr << "[Server] Fatal: --model and --model-dir are mutually "
                 "exclusive.\n"
              << "  --model serves one file; --model-dir scans a directory.\n";
    return 2;
  }
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

  if (model_file)
    std::cout << "[Server] model=" << *model_file;
  else
    std::cout << "[Server] model_dir=" << cfg.model_dir;
  std::cout << "  rest=" << cfg.rest_port << "  grpc=" << cfg.grpc_port
            << "  batching=" << (cfg.batching_enabled ? "on" : "off")
            << "  timeout_ms=" << cfg.inference_timeout_ms << "\n";

  omle::rt::LoadOptions load_opts;
  load_opts.n_threads = cfg.model_n_threads;
  load_opts.run_verification = cfg.run_verification;

  std::shared_ptr<omle_server::ModelRegistry> registry;

  if (model_file) {
    // One named file. This never falls back: the caller named exactly one
    // model, so a missing or unloadable file is a mistake, not a reason to come
    // up serving nothing.
    auto one = omle_server::ModelRegistry::load_file(*model_file, load_opts);
    if (!one.ok()) {
      std::cerr << "[Server] Fatal: " << one.message() << "\n";
      return 1;
    }
    registry = std::move(*one);
  } else {
    auto registry_or =
        omle_server::ModelRegistry::load(cfg.model_dir, load_opts);

    if (registry_or.ok()) {
      registry = std::move(*registry_or);
    } else if (model_dir_explicit) {
      // The only way load() fails is the directory not existing — a model that
      // will not parse is reported and skipped — so this is always "the path
      // you gave me is not there", and a typo'd --model-dir should not leave a
      // server running that answers every inference request with 404.
      std::cerr << "[Server] Fatal: " << registry_or.message() << "\n"
                << "  This path was requested explicitly; serving nothing is\n"
                << "  almost certainly not what you meant. Create it, or\n"
                << "  point at another with --model-dir PATH.\n";
      return 1;
    } else {
      // The default, absent. Legitimate at startup — a volume may mount a
      // moment later — so the server comes up empty rather than refusing to
      // start.
      //
      // An actually empty registry: this used to load("/tmp"), which turned a
      // missing model directory into "serve whatever is in /tmp", models the
      // operator never chose, from a directory any local user can write.
      std::cerr << "[Server] Warning: " << registry_or.message() << "\n"
                << "  Set one with --model-dir PATH, the OMLE_MODEL_DIR "
                   "environment variable,\n"
                << "  or \"model_dir\" in the config file.\n"
                << "  Continuing with an empty registry; no models will be "
                   "served.\n";
      registry = omle_server::ModelRegistry::empty();
    }
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

  // Drogon builds a 256-way sharded temp tree under its upload path when it
  // starts, and the default path is "uploads" relative to the working
  // directory. Nothing here uploads files, but the tree is created regardless —
  // and in the container the working directory is / and the process is
  // unprivileged, so the default produced 256 "Permission denied" errors at
  // every boot, before the first request. Point it somewhere writable instead
  // of relying on wherever the server happened to be started from.
  const auto upload_path =
      std::filesystem::temp_directory_path() / "omle-server-uploads";
  std::error_code upload_ec;
  std::filesystem::create_directories(upload_path, upload_ec);
  if (upload_ec)
    std::cerr << "[Server] Warning: cannot create " << upload_path << ": "
              << upload_ec.message() << "\n";

  drogon::app()
      .setLogLevel(trantor::Logger::kInfo)
      .setThreadNum(threads)
      .setUploadPath(upload_path.string())
      .addListener("0.0.0.0", cfg.rest_port)
      .run();

  grpc_server->Shutdown();
  return 0;
}

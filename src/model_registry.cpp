#include "omle_server/model_registry.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace omle_server {

namespace {

// Returns true and fills name/version if path matches
// <root>/<name>/<ver>/model.omle
bool parse_versioned(const fs::path& root, const fs::path& p, std::string& name,
                     std::string& version) {
  // p must be exactly 3 levels under root: <name>/<version>/model.omle
  auto rel = fs::relative(p, root);
  auto it = rel.begin();
  if (it == rel.end()) return false;
  name = (it++)->string();
  if (it == rel.end()) return false;
  version = (it++)->string();
  if (it == rel.end()) return false;
  return (it++)->string() == "model.omle" && it == rel.end();
}

}  // namespace

omle::rt::StatusOr<std::unique_ptr<ModelRegistry>> ModelRegistry::load_file(
    const std::string& model_path, const omle::rt::LoadOptions& opts) {
  // A directory here is a mistake worth naming rather than silently scanning:
  // the caller asked for one model, and --model-dir is the flag that takes a
  // directory.
  std::error_code ec;
  if (fs::is_directory(model_path, ec))
    return {omle::rt::ErrorCode::InvalidArgument,
            "model is a directory, not a file: " + model_path +
                " (use --model-dir for a directory)"};
  if (!fs::exists(model_path))
    return {omle::rt::ErrorCode::FileNotFound,
            "model does not exist: " + model_path};

  auto model = omle::rt::Model::load(model_path, opts);
  if (!model.ok()) {
    // Unlike a directory scan, which reports a bad model and moves on, there is
    // nothing else to serve: fail rather than start empty.
    return {model.status().code(),
            "cannot load " + model_path + ": " + model.message()};
  }

  const std::string name = fs::path(model_path).stem().string();
  auto registry = std::unique_ptr<ModelRegistry>(new ModelRegistry());
  registry->models_[name]["1"] = std::move(*model);
  std::cout << "[ModelRegistry] Loaded " << name << " v1 from \"" << model_path
            << "\"\n";
  return registry;
}

std::unique_ptr<ModelRegistry> ModelRegistry::empty() {
  // new rather than make_unique: the constructor is private.
  return std::unique_ptr<ModelRegistry>(new ModelRegistry());
}

omle::rt::StatusOr<std::unique_ptr<ModelRegistry>> ModelRegistry::load(
    const std::string& root_dir, const omle::rt::LoadOptions& opts) {
  if (!fs::exists(root_dir)) {
    return {omle::rt::ErrorCode::FileNotFound,
            "model_dir does not exist: " + root_dir};
  }

  auto registry = std::unique_ptr<ModelRegistry>(new ModelRegistry());

  // Walk the directory tree looking for model files.
  for (auto& entry : fs::recursive_directory_iterator(root_dir)) {
    if (!entry.is_regular_file()) continue;
    const auto& p = entry.path();

    std::string name, version;

    if (p.extension() == ".omle" && p.parent_path() == fs::path(root_dir)) {
      // Flat layout: <root>/<name>.omle  →  version "1"
      name = p.stem().string();
      version = "1";
    } else if (p.filename() == "model.omle") {
      // Versioned layout: <root>/<name>/<version>/model.omle
      if (!parse_versioned(root_dir, p, name, version)) continue;
    } else {
      continue;
    }

    auto result = omle::rt::Model::load(p.string(), opts);
    if (!result.ok()) {
      std::cerr << "[ModelRegistry] Failed to load " << p << ": "
                << result.message() << "\n";
      continue;
    }

    registry->models_[name][version] = std::move(*result);
    std::cout << "[ModelRegistry] Loaded " << name << " v" << version
              << " from " << p << "\n";
  }

  return registry;
}

std::shared_ptr<omle::rt::Model> ModelRegistry::get(
    const std::string& name, const std::string& version) const {
  std::shared_lock lock(mu_);
  auto it = models_.find(name);
  if (it == models_.end()) return nullptr;
  const auto& vmap = it->second;
  if (vmap.empty()) return nullptr;

  if (version.empty()) {
    // Return highest version (map is lexicographically sorted; last = highest).
    return vmap.rbegin()->second;
  }
  auto vit = vmap.find(version);
  return vit != vmap.end() ? vit->second : nullptr;
}

std::string ModelRegistry::resolve_version(const std::string& name,
                                           const std::string& version) const {
  std::shared_lock lock(mu_);
  auto it = models_.find(name);
  if (it == models_.end() || it->second.empty()) return {};
  const auto& vmap = it->second;
  // Mirrors get(): empty means the highest, and the map is sorted.
  if (version.empty()) return vmap.rbegin()->first;
  return vmap.count(version) ? version : std::string{};
}

bool ModelRegistry::is_ready(const std::string& name,
                             const std::string& version) const {
  return get(name, version) != nullptr;
}

std::vector<std::string> ModelRegistry::model_names() const {
  std::shared_lock lock(mu_);
  std::vector<std::string> names;
  names.reserve(models_.size());
  for (auto& [n, _] : models_) names.push_back(n);
  return names;
}

std::vector<std::string> ModelRegistry::versions(
    const std::string& name) const {
  std::shared_lock lock(mu_);
  auto it = models_.find(name);
  if (it == models_.end()) return {};
  std::vector<std::string> vers;
  for (auto& [v, _] : it->second) vers.push_back(v);
  return vers;
}

}  // namespace omle_server

#ifndef OMLE_SERVER_MODEL_REGISTRY_H_
#define OMLE_SERVER_MODEL_REGISTRY_H_

#include <omle/runtime.h>
#include <omle/status.h>

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omle_server {

// Thread-safe registry of loaded omle models.
//
// Directory layout (two supported conventions):
//
//   Flat:      <root>/<name>.omle              → version "1"
//   Versioned: <root>/<name>/<version>/model.omle
//
// All models are loaded eagerly at construction time.
// The registry is read-only after construction (no lock needed on hot path
// once the shared_ptr is obtained).
class ModelRegistry {
 public:
  static omle::rt::StatusOr<std::unique_ptr<ModelRegistry>> load(
      const std::string& root_dir, const omle::rt::LoadOptions& opts = {});

  // Returns nullptr if not found.  version="" picks the highest version.
  std::shared_ptr<omle::rt::Model> get(const std::string& name,
                                       const std::string& version = "") const;

  bool is_ready(const std::string& name, const std::string& version = "") const;

  std::vector<std::string> model_names() const;
  std::vector<std::string> versions(const std::string& name) const;

 private:
  ModelRegistry() = default;

  // Outer key = model name, inner key = version string (sorted
  // lexicographically).
  using VersionMap = std::map<std::string, std::shared_ptr<omle::rt::Model>>;
  std::unordered_map<std::string, VersionMap> models_;
  mutable std::shared_mutex mu_;
};

}  // namespace omle_server

#endif  // OMLE_SERVER_MODEL_REGISTRY_H_

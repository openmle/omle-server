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

  // A registry holding exactly one model, named after the file's stem and
  // version "1" — the same convention load() applies to a flat directory, so
  // <name>.omle is reachable at /v2/models/<name> either way.
  //
  // For serving a single file without arranging a directory around it, which is
  // what a quick check against one model needs.
  static omle::rt::StatusOr<std::unique_ptr<ModelRegistry>> load_file(
      const std::string& model_path, const omle::rt::LoadOptions& opts = {});

  // A registry holding no models, for a server whose model directory is absent
  // or unreadable.
  //
  // Constructed, not loaded. Obtaining an "empty" registry by scanning some
  // other directory serves whatever happens to be in it: the previous code
  // scanned /tmp, so a server told to use a non-existent --model-dir silently
  // published every .omle file any local user had left in a world-writable
  // directory, while logging "empty registry".
  static std::unique_ptr<ModelRegistry> empty();

  // Returns nullptr if not found.  version="" picks the highest version.
  std::shared_ptr<omle::rt::Model> get(const std::string& name,
                                       const std::string& version = "") const;

  bool is_ready(const std::string& name, const std::string& version = "") const;

  // The version get() would serve for this request: the argument when it names
  // one, otherwise the highest available. Empty when the model is unknown.
  //
  // Metrics label with this rather than the requested string, so a client
  // asking for "1" and one asking for latest — the same execution — land in the
  // same series instead of two.
  std::string resolve_version(const std::string& name,
                              const std::string& version = "") const;

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

#include "omle_server/batcher.h"

#include <cassert>
#include <cstring>
#include <unordered_set>

#include "omle_server/metrics.h"

namespace omle_server {

// ── Tensor helpers
// ────────────────────────────────────────────────────────────

omle::rt::Tensor Batcher::concat_rows(
    const std::vector<const omle::rt::Tensor*>& ts) {
  assert(!ts.empty());
  auto dtype = ts[0]->dtype;
  int nc = ts[0]->n_cols;

  int total = 0;
  for (auto* t : ts) total += t->n_rows;

  if (dtype == omle::rt::DataType::String) {
    std::vector<std::string> strs;
    strs.reserve(static_cast<size_t>(total) * nc);
    for (auto* t : ts)
      for (int r = 0; r < t->n_rows; ++r)
        for (int c = 0; c < nc; ++c) strs.push_back(t->str_at(r, c));
    return omle::rt::Tensor::strings(total, nc, std::move(strs));
  }

  int esz = omle::rt::dtype_size(dtype);
  std::vector<uint8_t> raw(static_cast<size_t>(total) * nc * esz);
  uint8_t* dst = raw.data();
  for (auto* t : ts) {
    size_t bytes = static_cast<size_t>(t->n_rows) * nc * esz;
    // raw_data() already returns the scalar buffer for Kind::Scalar.
    std::memcpy(dst, t->raw_data(), bytes);
    dst += bytes;
  }
  return omle::rt::Tensor::from_raw(dtype, total, nc, std::move(raw));
}

std::vector<omle::rt::Tensor> Batcher::split_rows(
    const omle::rt::Tensor& batched, const std::vector<int>& counts) {
  std::vector<omle::rt::Tensor> result;
  result.reserve(counts.size());
  auto dtype = batched.dtype;
  int nc = batched.n_cols;
  int row = 0;

  for (int count : counts) {
    if (dtype == omle::rt::DataType::String) {
      std::vector<std::string> strs;
      strs.reserve(static_cast<size_t>(count) * nc);
      for (int r = row; r < row + count; ++r)
        for (int c = 0; c < nc; ++c) strs.push_back(batched.str_at(r, c));
      result.push_back(omle::rt::Tensor::strings(count, nc, std::move(strs)));
    } else {
      int esz = omle::rt::dtype_size(dtype);
      size_t off = static_cast<size_t>(row) * nc * esz;
      size_t bytes = static_cast<size_t>(count) * nc * esz;
      const uint8_t* src =
          static_cast<const uint8_t*>(batched.raw_data()) + off;
      std::vector<uint8_t> raw(src, src + bytes);
      result.push_back(
          omle::rt::Tensor::from_raw(dtype, count, nc, std::move(raw)));
    }
    row += count;
  }
  return result;
}

// ── Batcher
// ───────────────────────────────────────────────────────────────────

Batcher::Batcher(std::shared_ptr<omle::rt::Model> model, std::string model_name,
                 int max_batch_size, std::chrono::milliseconds batch_timeout)
    : model_(std::move(model)),
      model_name_(std::move(model_name)),
      max_batch_size_(max_batch_size),
      batch_timeout_(batch_timeout),
      worker_(&Batcher::worker_loop, this) {}

Batcher::~Batcher() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::future<InferResult> Batcher::submit(
    std::unordered_map<std::string, omle::rt::Tensor> inputs,
    std::vector<std::string> output_filter) {
  int n_rows = inputs.empty() ? 1 : inputs.begin()->second.n_rows;

  PendingRequest req;
  req.inputs = std::move(inputs);
  req.output_filter = std::move(output_filter);
  req.n_rows = n_rows;
  auto fut = req.promise.get_future();

  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(std::move(req));
  }
  cv_.notify_one();
  return fut;
}

int Batcher::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return static_cast<int>(queue_.size());
}

void Batcher::worker_loop() {
  while (true) {
    std::vector<PendingRequest> batch;
    {
      std::unique_lock<std::mutex> lk(mu_);

      // Block until at least one item arrives (or shutdown).
      cv_.wait(lk, [this] { return !queue_.empty() || stopping_; });

      if (stopping_) {
        for (auto& r : queue_)
          r.promise.set_value(
              {omle::rt::ErrorCode::InvalidArgument, "server shutting down"});
        queue_.clear();
        break;
      }

      // Wait up to batch_timeout_ for more items to accumulate.
      cv_.wait_for(lk, batch_timeout_, [this] {
        return static_cast<int>(queue_.size()) >= max_batch_size_ || stopping_;
      });

      // Drain up to max_batch_size_ items.
      int n = std::min(static_cast<int>(queue_.size()), max_batch_size_);
      batch.reserve(n);
      for (int i = 0; i < n; ++i) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }
    }

    if (!batch.empty()) flush(batch);
  }
}

void Batcher::flush(std::vector<PendingRequest>& batch) {
  // Collect per-request row counts for output splitting.
  std::vector<int> row_counts;
  row_counts.reserve(batch.size());
  for (auto& r : batch) row_counts.push_back(r.n_rows);

  // Build stacked input map from all requests.
  std::unordered_map<std::string, omle::rt::Tensor> stacked;
  for (auto& [name, _] : batch[0].inputs) {
    std::vector<const omle::rt::Tensor*> ptrs;
    ptrs.reserve(batch.size());
    for (auto& r : batch) {
      auto it = r.inputs.find(name);
      if (it != r.inputs.end()) ptrs.push_back(&it->second);
    }
    if (!ptrs.empty()) stacked[name] = concat_rows(ptrs);
  }

  // Determine output filter: union of all per-request filters.
  // An empty filter in any request means "return all outputs".
  std::vector<std::string> output_filter;
  bool want_all = false;
  for (auto& r : batch) {
    if (r.output_filter.empty()) {
      want_all = true;
      break;
    }
  }
  if (!want_all) {
    std::unordered_set<std::string> seen;
    for (auto& r : batch)
      for (auto& n : r.output_filter)
        if (seen.insert(n).second) output_filter.push_back(n);
  }

  // Record the actual dispatch size.
  Metrics::instance().record_batch_flush(model_name_,
                                         static_cast<int>(batch.size()));

  // Run batched predict.
  auto result = model_->predict(stacked, output_filter);

  if (!result.ok()) {
    for (auto& r : batch) r.promise.set_value(result.status());
    return;
  }

  // Split each output tensor back to per-request slices.
  auto& batch_out = *result;
  for (int i = 0; i < static_cast<int>(batch.size()); ++i) {
    std::unordered_map<std::string, omle::rt::Tensor> req_out;
    for (auto& [oname, otensor] : batch_out) {
      auto slices = split_rows(otensor, row_counts);
      req_out[oname] = std::move(slices[i]);
    }
    batch[i].promise.set_value(std::move(req_out));
  }
}

// ── BatcherRegistry
// ───────────────────────────────────────────────────────────

BatcherRegistry::BatcherRegistry(std::shared_ptr<ModelRegistry> registry,
                                 const ServerConfig& cfg) {
  if (!cfg.batching_enabled) return;

  for (auto& name : registry->model_names()) {
    // Resolve per-model overrides, falling back to global defaults.
    int bsz = cfg.max_batch_size;
    int tmo_ms = cfg.batch_timeout_ms;
    auto it = cfg.model_configs.find(name);
    if (it != cfg.model_configs.end()) {
      if (it->second.max_batch_size > 0) bsz = it->second.max_batch_size;
      if (it->second.batch_timeout_ms >= 0)
        tmo_ms = it->second.batch_timeout_ms;
    }
    if (bsz <= 1) continue;  // batching disabled for this model

    for (auto& ver : registry->versions(name)) {
      auto model = registry->get(name, ver);
      if (!model) continue;
      std::string key = name + "@" + ver;
      batchers_[key] = std::make_unique<Batcher>(
          model, name, bsz, std::chrono::milliseconds(tmo_ms));
    }
  }
}

Batcher* BatcherRegistry::get(const std::string& name,
                              const std::string& version) const {
  // Resolve empty version to "latest" (any key with this name).
  if (version.empty()) {
    // The registry sorts versions lexicographically; find the last one.
    std::string best_key;
    for (auto& [k, _] : batchers_) {
      auto at = k.find('@');
      if (at != std::string::npos && k.substr(0, at) == name) {
        if (best_key.empty() || k > best_key) best_key = k;
      }
    }
    if (best_key.empty()) return nullptr;
    return batchers_.at(best_key).get();
  }
  auto it = batchers_.find(name + "@" + version);
  return it != batchers_.end() ? it->second.get() : nullptr;
}

}  // namespace omle_server

#ifndef OMLE_SERVER_METRICS_H_
#define OMLE_SERVER_METRICS_H_

#include <array>
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace omle_server {

// Thread-safe singleton metric store.  All write paths are lock-free (atomics);
// the mutex is only taken when a new model name is first seen.
class Metrics {
 public:
  static Metrics& instance();

  // Record one completed inference.
  //   model         — model name
  //   success       — false if the runtime returned an error
  //   duration_sec  — wall-clock time from request submit to result ready
  //   batch_size    — actual number of rows flushed in this batch
  // Called once per request (latency + success/error counter).
  void record_request(const std::string& model, bool success,
                      double duration_sec);

  // Called once per batcher flush with the number of requests in that batch.
  void record_batch_flush(const std::string& model, int batch_size);

  void record_timeout(const std::string& model);
  void set_models_loaded(int n);

  // Render all metrics in Prometheus text (exposition) format.
  std::string prometheus_text() const;

 private:
  Metrics() = default;

  // Histogram bucket upper bounds (seconds).
  static constexpr double BOUNDS[] = {0.001, 0.005, 0.010, 0.025, 0.050,
                                      0.100, 0.250, 0.500, 1.000, 5.000};
  static constexpr int N_BOUNDS = 10;
  // One extra slot for +Inf (total count, always equal to requests_ok +
  // requests_err).

  struct ModelMetrics {
    std::atomic<uint64_t> requests_ok{0};
    std::atomic<uint64_t> requests_err{0};
    std::atomic<uint64_t> timeouts{0};
    // Latency histogram — stored per-bucket (non-cumulative for correctness
    // with concurrent updates); cumulated when rendering.
    std::array<std::atomic<uint64_t>, N_BOUNDS + 1> dur_bucket{};
    std::atomic<uint64_t> dur_sum_us{0};  // microsecond sum for average
    // Batch-size histogram (buckets: 1,2,4,8,16,32,64,+Inf)
    static constexpr int BSIZE_BOUNDS[] = {1, 2, 4, 8, 16, 32, 64};
    static constexpr int N_BSIZE = 7;
    std::array<std::atomic<uint64_t>, N_BSIZE + 1> bsz_bucket{};
    std::atomic<uint64_t> bsz_sum{0};

    ModelMetrics() {
      for (auto& b : dur_bucket) b.store(0);
      for (auto& b : bsz_bucket) b.store(0);
    }
    // Non-copyable due to atomics.
    ModelMetrics(const ModelMetrics&) = delete;
    ModelMetrics& operator=(const ModelMetrics&) = delete;
  };

  ModelMetrics& get_or_create(const std::string& model);

  mutable std::shared_mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<ModelMetrics>> per_model_;
  std::atomic<int> models_loaded_{0};
};

}  // namespace omle_server

#endif  // OMLE_SERVER_METRICS_H_

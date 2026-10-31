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
  // Every recorder takes the RESOLVED version, so a series describes one model
  // binary. Merging versions hid the case these metrics exist to catch: a new
  // version being slower or failing more than the one it replaced, invisible in
  // the aggregate while both are being served.
  void record_request(const std::string& model, const std::string& version,
                      bool success, double duration_sec);

  // How long a request sat in the batch queue before dispatch, which is the
  // half of latency the batch window controls. Folded into request_duration it
  // is invisible, so there is no way to tell a slow model from a long wait.
  void record_queue_wait(const std::string& model, const std::string& version,
                         double wait_sec);

  // A request that never reached the model: a body that would not decode, a
  // timeout, an unknown model. It counts toward omle_requests_total so an error
  // rate covers what clients actually experience, and deliberately does NOT
  // observe the latency histogram — the time spent rejecting a malformed body
  // is not inference latency, and mixing the two makes the percentiles lie.
  void record_rejected(const std::string& model, const std::string& version);

  // The label to use when the model name came from the request and named
  // nothing this server has. A per-model series keyed on an unknown name would
  // let any caller mint one series per request, growing this map and the
  // scraper's cardinality without bound; one shared bucket keeps the 404 rate
  // visible without that.
  static constexpr const char* kUnknownModel = "<unknown>";

  // Called once per batcher flush with the number of requests in that batch.
  void record_batch_flush(const std::string& model, const std::string& version,
                          int batch_size);

  void record_timeout(const std::string& model, const std::string& version);
  void set_models_loaded(int n);

  // Render all metrics in Prometheus text (exposition) format.
  std::string prometheus_text() const;

 private:
  Metrics() = default;

  // Histogram bucket upper bounds (seconds).
  // Sub-millisecond bounds are the point of this list. Classical ML inference
  // runs in tens to hundreds of microseconds — a measured 120us per request for
  // a two-feature model — so a histogram starting at 1ms puts every observation
  // in the first bucket and reports p50, p90 and p99 as indistinguishable. The
  // upper bounds stay for the slow cases that actually matter: a large
  // ensemble, a cold page, a saturated pool.
  static constexpr double BOUNDS[] = {
      0.000050, 0.000100, 0.000250, 0.000500, 0.001, 0.005, 0.010,
      0.025,    0.050,    0.100,    0.250,    0.500, 1.000, 5.000};
  static constexpr int N_BOUNDS = 14;
  // One extra slot for +Inf (total count, always equal to requests_ok +
  // requests_err).

  // Queue waits are bounded by the batch window (batch_timeout_ms, single-digit
  // to tens of milliseconds by default), so the useful resolution sits below
  // that. The tail exists to show a window that is set too wide.
  static constexpr double QW_BOUNDS[] = {0.0001, 0.0005, 0.001, 0.005, 0.010,
                                         0.025,  0.050,  0.100, 0.250, 1.000};
  static constexpr int N_QW = 10;

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
    std::array<std::atomic<uint64_t>, N_QW + 1> qw_bucket{};
    std::atomic<uint64_t> qw_sum_us{0};

    ModelMetrics() {
      for (auto& b : dur_bucket) b.store(0);
      for (auto& b : bsz_bucket) b.store(0);
      for (auto& b : qw_bucket) b.store(0);
    }
    // Non-copyable due to atomics.
    ModelMetrics(const ModelMetrics&) = delete;
    ModelMetrics& operator=(const ModelMetrics&) = delete;
  };

  // per_model_ is keyed on model and version joined by a unit separator, a byte
  // that cannot appear in either, so the two are recoverable at render time
  // without a second map or a struct key.
  static std::string series_key(const std::string& model,
                                const std::string& version);
  // "model=\"x\",version=\"1\"" for a key produced above.
  static std::string series_labels(const std::string& key);

  ModelMetrics& get_or_create(const std::string& key);

  mutable std::shared_mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<ModelMetrics>> per_model_;
  std::atomic<int> models_loaded_{0};
};

}  // namespace omle_server

#endif  // OMLE_SERVER_METRICS_H_

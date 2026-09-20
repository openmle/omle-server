#include "omle_server/metrics.h"

#include <iomanip>
#include <sstream>

namespace omle_server {

constexpr double Metrics::BOUNDS[];
constexpr int Metrics::ModelMetrics::BSIZE_BOUNDS[];

// ── Singleton
// ─────────────────────────────────────────────────────────────────

Metrics& Metrics::instance() {
  static Metrics inst;
  return inst;
}

// ── Writing
// ───────────────────────────────────────────────────────────────────

Metrics::ModelMetrics& Metrics::get_or_create(const std::string& model) {
  {
    std::shared_lock rl(mu_);
    auto it = per_model_.find(model);
    if (it != per_model_.end()) return *it->second;
  }
  std::unique_lock wl(mu_);
  auto& ptr = per_model_[model];
  if (!ptr) ptr = std::make_unique<ModelMetrics>();
  return *ptr;
}

void Metrics::record_request(const std::string& model, bool success,
                             double duration_sec) {
  auto& m = get_or_create(model);

  if (success)
    m.requests_ok.fetch_add(1, std::memory_order_relaxed);
  else
    m.requests_err.fetch_add(1, std::memory_order_relaxed);

  // Latency histogram bucket (find first bound >= duration_sec).
  int b = N_BOUNDS;  // +Inf
  for (int i = 0; i < N_BOUNDS; ++i) {
    if (duration_sec <= BOUNDS[i]) {
      b = i;
      break;
    }
  }
  m.dur_bucket[b].fetch_add(1, std::memory_order_relaxed);
  m.dur_sum_us.fetch_add(static_cast<uint64_t>(duration_sec * 1e6),
                         std::memory_order_relaxed);
}

void Metrics::record_batch_flush(const std::string& model, int batch_size) {
  auto& m = get_or_create(model);
  int bsz_b = ModelMetrics::N_BSIZE;  // +Inf
  for (int i = 0; i < ModelMetrics::N_BSIZE; ++i) {
    if (batch_size <= ModelMetrics::BSIZE_BOUNDS[i]) {
      bsz_b = i;
      break;
    }
  }
  m.bsz_bucket[bsz_b].fetch_add(1, std::memory_order_relaxed);
  m.bsz_sum.fetch_add(static_cast<uint64_t>(batch_size),
                      std::memory_order_relaxed);
}

void Metrics::record_timeout(const std::string& model) {
  get_or_create(model).timeouts.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::set_models_loaded(int n) {
  models_loaded_.store(n, std::memory_order_relaxed);
}

// ── Prometheus text rendering
// ─────────────────────────────────────────────────

std::string Metrics::prometheus_text() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(9);

  // Server-level gauge.
  out << "# HELP omle_models_loaded Number of models currently loaded\n"
      << "# TYPE omle_models_loaded gauge\n"
      << "omle_models_loaded " << models_loaded_.load() << "\n\n";

  // Per-model metrics. The HELP/TYPE headers are emitted even with no models
  // recorded yet, so a scraper can discover the metric families before the
  // server has served its first request; the sample loops below emit nothing.
  std::shared_lock rl(mu_);

  out << "# HELP omle_requests_total Total inference requests\n"
      << "# TYPE omle_requests_total counter\n";
  for (auto& [name, m] : per_model_) {
    out << "omle_requests_total{model=\"" << name << "\",status=\"success\"} "
        << m->requests_ok.load() << "\n";
    out << "omle_requests_total{model=\"" << name << "\",status=\"error\"} "
        << m->requests_err.load() << "\n";
  }
  out << "\n";

  out << "# HELP omle_inference_timeouts_total Requests that exceeded "
         "timeout\n"
      << "# TYPE omle_inference_timeouts_total counter\n";
  for (auto& [name, m] : per_model_)
    out << "omle_inference_timeouts_total{model=\"" << name << "\"} "
        << m->timeouts.load() << "\n";
  out << "\n";

  // Latency histogram.
  out << "# HELP omle_request_duration_seconds Inference request latency\n"
      << "# TYPE omle_request_duration_seconds histogram\n";
  for (auto& [name, m] : per_model_) {
    uint64_t cumsum = 0;
    for (int i = 0; i < N_BOUNDS; ++i) {
      cumsum += m->dur_bucket[i].load();
      out << "omle_request_duration_seconds_bucket{model=\"" << name
          << "\",le=\"" << BOUNDS[i] << "\"} " << cumsum << "\n";
    }
    cumsum += m->dur_bucket[N_BOUNDS].load();
    out << "omle_request_duration_seconds_bucket{model=\"" << name
        << "\",le=\"+Inf\"} " << cumsum << "\n";
    double sum_s = static_cast<double>(m->dur_sum_us.load()) / 1e6;
    out << "omle_request_duration_seconds_sum{model=\"" << name << "\"} "
        << sum_s << "\n";
    out << "omle_request_duration_seconds_count{model=\"" << name << "\"} "
        << cumsum << "\n";
  }
  out << "\n";

  // Batch-size histogram.
  out << "# HELP omle_batch_size Batch size actually dispatched to the "
         "model\n"
      << "# TYPE omle_batch_size histogram\n";
  for (auto& [name, m] : per_model_) {
    uint64_t cumsum = 0;
    for (int i = 0; i < ModelMetrics::N_BSIZE; ++i) {
      cumsum += m->bsz_bucket[i].load();
      out << "omle_batch_size_bucket{model=\"" << name << "\",le=\""
          << ModelMetrics::BSIZE_BOUNDS[i] << "\"} " << cumsum << "\n";
    }
    cumsum += m->bsz_bucket[ModelMetrics::N_BSIZE].load();
    out << "omle_batch_size_bucket{model=\"" << name << "\",le=\"+Inf\"} "
        << cumsum << "\n";
    double avg = cumsum > 0 ? static_cast<double>(m->bsz_sum.load()) /
                                  static_cast<double>(cumsum)
                            : 0.0;
    out << "omle_batch_size_sum{model=\"" << name << "\"} " << m->bsz_sum.load()
        << "\n";
    out << "omle_batch_size_count{model=\"" << name << "\"} " << cumsum << "\n";
    (void)avg;
  }
  out << "\n";

  return out.str();
}

}  // namespace omle_server

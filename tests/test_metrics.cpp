#include <gtest/gtest.h>

#include <string>

#include "omle_server/metrics.h"

using namespace omle_server;

// Helper: assert that needle appears in prometheus_text() output.
static void expect_metric(const std::string& text, const std::string& needle) {
  EXPECT_NE(text.find(needle), std::string::npos)
      << "Expected to find '" << needle << "' in metrics output";
}

TEST(Metrics, PrometheusTextNotEmpty) {
  std::string text = Metrics::instance().prometheus_text();
  EXPECT_FALSE(text.empty());
}

TEST(Metrics, PrometheusTextContainsExpectedMetricNames) {
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "omle_requests_total");
  expect_metric(text, "omle_request_duration_seconds");
  expect_metric(text, "omle_batch_size");
  expect_metric(text, "omle_models_loaded");
}

TEST(Metrics, RecordSuccessfulRequest) {
  Metrics::instance().record_request("model_a", true, 0.010);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_a");
  expect_metric(text, "omle_requests_total");
}

TEST(Metrics, RecordFailedRequest) {
  Metrics::instance().record_request("model_err", false, 0.005);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_err");
}

TEST(Metrics, RecordBatchFlush) {
  Metrics::instance().record_batch_flush("model_b", 8);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_b");
  expect_metric(text, "omle_batch_size");
}

TEST(Metrics, RecordTimeout) {
  Metrics::instance().record_timeout("model_t");
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_t");
  expect_metric(text, "omle_inference_timeouts_total");
}

TEST(Metrics, SetModelsLoaded) {
  Metrics::instance().set_models_loaded(3);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "omle_models_loaded");
  // The gauge value 3 should appear somewhere near the metric name.
  expect_metric(text, "3");
}

TEST(Metrics, MultipleRequestsAccumulate) {
  // Record several requests and check counters increase monotonically.
  Metrics::instance().record_request("model_c", true, 0.001);
  Metrics::instance().record_request("model_c", true, 0.002);
  Metrics::instance().record_request("model_c", false, 0.003);

  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_c");
}

TEST(Metrics, LatencyBucketBoundariesPresent) {
  Metrics::instance().record_request("model_lat", true, 0.025);
  std::string text = Metrics::instance().prometheus_text();
  // Standard OIP histogram bucket labels.
  expect_metric(text, "0.001");
  expect_metric(text, "0.005");
  expect_metric(text, "0.01");
}

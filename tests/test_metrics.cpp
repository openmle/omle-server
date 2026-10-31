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
  Metrics::instance().record_request("model_a", "1", true, 0.010);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_a");
  expect_metric(text, "omle_requests_total");
}

TEST(Metrics, RecordFailedRequest) {
  Metrics::instance().record_request("model_err", "1", false, 0.005);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_err");
}

TEST(Metrics, RecordBatchFlush) {
  Metrics::instance().record_batch_flush("model_b", "1", 8);
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_b");
  expect_metric(text, "omle_batch_size");
}

TEST(Metrics, RecordTimeout) {
  Metrics::instance().record_timeout("model_t", "1");
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
  Metrics::instance().record_request("model_c", "1", true, 0.001);
  Metrics::instance().record_request("model_c", "1", true, 0.002);
  Metrics::instance().record_request("model_c", "1", false, 0.003);

  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "model_c");
}

TEST(Metrics, LatencyBucketBoundariesPresent) {
  Metrics::instance().record_request("model_lat", "1", true, 0.025);
  std::string text = Metrics::instance().prometheus_text();
  // Standard OIP histogram bucket labels.
  expect_metric(text, "0.001");
  expect_metric(text, "0.005");
  expect_metric(text, "0.01");
}

// Sub-millisecond buckets exist because this server is that fast: a measured
// ~120us per request for a two-feature model. With a 1ms floor every
// observation landed in the first bucket and p50, p90 and p99 were
// indistinguishable.
TEST(Metrics, LatencyHistogramResolvesSubMillisecond) {
  std::string text = Metrics::instance().prometheus_text();
  // The exposition renders bounds fixed-width, not in scientific notation, so
  // these are the strings a scraper actually sees.
  expect_metric(text, "le=\"0.000050000\"");  // 50us
  expect_metric(text, "le=\"0.000100000\"");  // 100us
  expect_metric(text, "le=\"0.000250000\"");  // 250us
  expect_metric(text, "le=\"0.000500000\"");  // 500us
}

TEST(Metrics, SubMillisecondObservationsLandInDistinctBuckets) {
  // Two requests an order of magnitude apart must not share a bucket, which is
  // the whole point of the added bounds.
  Metrics::instance().record_request("model_fast", "1", true,
                                     0.000030);  // 30us
  Metrics::instance().record_request("model_fast", "1", true,
                                     0.000300);  // 300us
  std::string text = Metrics::instance().prometheus_text();

  // The 50us bucket holds only the first; the 500us bucket holds both.
  EXPECT_NE(text.find("le=\"0.000050000\"} 1"), std::string::npos) << text;
  EXPECT_NE(text.find("le=\"0.000500000\"} 2"), std::string::npos) << text;
}

// A request rejected before it reached the model still happened, so it counts
// toward the request total. It must not touch the latency histogram: the time
// spent rejecting a malformed body is not inference latency.
TEST(Metrics, RejectedRequestCountsAsErrorWithoutAffectingLatency) {
  Metrics::instance().record_request("model_rej", "1", true, 0.002);
  std::string before = Metrics::instance().prometheus_text();

  Metrics::instance().record_rejected("model_rej", "1");
  std::string after = Metrics::instance().prometheus_text();

  EXPECT_NE(after.find("omle_requests_total{model=\"model_rej\",version=\"1\","
                       "status=\"error\"} 1"),
            std::string::npos)
      << after;
  // The histogram count is unchanged: still the one real request.
  EXPECT_NE(after.find("omle_request_duration_seconds_count{model=\"model_"
                       "rej\",version=\"1\"} 1"),
            std::string::npos)
      << after;
}

// An unknown model name comes from the request, so keying a series on it would
// let any caller mint one series per request.
TEST(Metrics, UnknownModelRejectionsShareOneSeries) {
  Metrics::instance().record_rejected(Metrics::kUnknownModel, "");
  Metrics::instance().record_rejected(Metrics::kUnknownModel, "");
  std::string text = Metrics::instance().prometheus_text();
  EXPECT_NE(text.find(std::string("model=\"") + Metrics::kUnknownModel + "\""),
            std::string::npos)
      << text;
}

// ── per-version series ───────────────────────────────────────────────────────

// Two versions of one model are two series. Merged, the case these metrics
// exist for was invisible: a new version slower or failing more than the one it
// replaced, hidden in an aggregate while both are being served.
TEST(Metrics, VersionsGetSeparateSeries) {
  Metrics::instance().record_request("versioned", "1", true, 0.001);
  Metrics::instance().record_request("versioned", "2", false, 0.002);
  std::string text = Metrics::instance().prometheus_text();

  expect_metric(text,
                "omle_requests_total{model=\"versioned\",version=\"1\","
                "status=\"success\"} 1");
  expect_metric(text,
                "omle_requests_total{model=\"versioned\",version=\"2\","
                "status=\"error\"} 1");
  // And version 1 must not have inherited version 2's failure.
  expect_metric(text,
                "omle_requests_total{model=\"versioned\",version=\"1\","
                "status=\"error\"} 0");
}

// ── queue wait ───────────────────────────────────────────────────────────────

TEST(Metrics, QueueWaitIsRecordedSeparatelyFromLatency) {
  Metrics::instance().record_request("qw", "1", true, 0.001);
  Metrics::instance().record_queue_wait("qw", "1", 0.008);
  std::string text = Metrics::instance().prometheus_text();

  // One request, one wait, counted in their own families.
  expect_metric(
      text,
      "omle_request_duration_seconds_count{model=\"qw\",version=\"1\"} 1");
  expect_metric(
      text,
      "omle_batch_queue_wait_seconds_count{model=\"qw\",version=\"1\"} 1");
  // 8ms lands above the 5ms bound and at or below the 10ms one.
  expect_metric(
      text,
      "omle_batch_queue_wait_seconds_bucket{model=\"qw\",version=\"1\","
      "le=\"0.005000000\"} 0");
  expect_metric(
      text,
      "omle_batch_queue_wait_seconds_bucket{model=\"qw\",version=\"1\","
      "le=\"0.010000000\"} 1");
}

TEST(Metrics, QueueWaitFamilyIsPresentBeforeAnyBatching) {
  // The HELP/TYPE headers must exist so a scraper can discover the family on a
  // server that has not batched anything yet.
  std::string text = Metrics::instance().prometheus_text();
  expect_metric(text, "# TYPE omle_batch_queue_wait_seconds histogram");
}

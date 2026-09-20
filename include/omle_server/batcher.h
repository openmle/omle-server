#ifndef OMLE_SERVER_BATCHER_H_
#define OMLE_SERVER_BATCHER_H_

#include <omle/runtime.h>
#include <omle/status.h>
#include <omle/tensor.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "model_registry.h"
#include "server_config.h"

namespace omle_server {

using InferResult =
    omle::rt::StatusOr<std::unordered_map<std::string, omle::rt::Tensor>>;

// Collects individual inference requests into batches.
//
// A dedicated worker thread waits for up to `batch_timeout` after the first
// item arrives; it flushes early when `max_batch_size` items are queued.
// Each submit() returns a future that resolves when the batch completes.
class Batcher {
 public:
  Batcher(std::shared_ptr<omle::rt::Model> model, std::string model_name,
          int max_batch_size, std::chrono::milliseconds batch_timeout);
  ~Batcher();

  // Submit one inference.  `inputs` may have multiple rows; n_rows is
  // recorded so outputs can be split back correctly.
  std::future<InferResult> submit(
      std::unordered_map<std::string, omle::rt::Tensor> inputs,
      std::vector<std::string> output_filter);

  int pending() const;

 private:
  struct PendingRequest {
    std::unordered_map<std::string, omle::rt::Tensor> inputs;
    std::vector<std::string> output_filter;
    int n_rows;
    std::promise<InferResult> promise;
  };

  void worker_loop();
  void flush(std::vector<PendingRequest>& batch);

  // Stack tensors vertically (must share dtype and n_cols).
  static omle::rt::Tensor concat_rows(
      const std::vector<const omle::rt::Tensor*>& ts);

  // Split a batched tensor into per-request slices by row counts.
  static std::vector<omle::rt::Tensor> split_rows(
      const omle::rt::Tensor& batched, const std::vector<int>& counts);

  std::shared_ptr<omle::rt::Model> model_;
  std::string model_name_;
  int max_batch_size_;
  std::chrono::milliseconds batch_timeout_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<PendingRequest> queue_;
  bool stopping_{false};
  std::thread worker_;
};

// Owns one Batcher per loaded model.  Returns nullptr when batching is
// disabled (batching_enabled = false) or the model is not found.
class BatcherRegistry {
 public:
  BatcherRegistry(std::shared_ptr<ModelRegistry> registry,
                  const ServerConfig& cfg);

  Batcher* get(const std::string& name, const std::string& version = "") const;

 private:
  // Key: "name@version" where version is the resolved canonical string.
  std::unordered_map<std::string, std::unique_ptr<Batcher>> batchers_;
};

}  // namespace omle_server

#endif  // OMLE_SERVER_BATCHER_H_

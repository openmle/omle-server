#ifndef OMLE_SERVER_GRPC_INFERENCE_SERVICE_H_
#define OMLE_SERVER_GRPC_INFERENCE_SERVICE_H_

#include <grpcpp/grpcpp.h>

#include <memory>
#include <thread>

#include "omle_server/batcher.h"
#include "omle_server/model_registry.h"
#include "omle_server/server_config.h"
#include "open_inference_grpc.grpc.pb.h"

namespace omle_server {

class InferenceServiceImpl final
    : public inference::GRPCInferenceService::Service {
 public:
  InferenceServiceImpl(std::shared_ptr<ModelRegistry> registry,
                       std::shared_ptr<BatcherRegistry> batchers,
                       const ServerConfig& cfg);

  grpc::Status ServerLive(grpc::ServerContext*,
                          const inference::ServerLiveRequest*,
                          inference::ServerLiveResponse*) override;
  grpc::Status ServerReady(grpc::ServerContext*,
                           const inference::ServerReadyRequest*,
                           inference::ServerReadyResponse*) override;
  grpc::Status ModelReady(grpc::ServerContext*,
                          const inference::ModelReadyRequest*,
                          inference::ModelReadyResponse*) override;
  grpc::Status ServerMetadata(grpc::ServerContext*,
                              const inference::ServerMetadataRequest*,
                              inference::ServerMetadataResponse*) override;
  grpc::Status ModelMetadata(grpc::ServerContext*,
                             const inference::ModelMetadataRequest*,
                             inference::ModelMetadataResponse*) override;
  grpc::Status ModelInfer(grpc::ServerContext*,
                          const inference::ModelInferRequest*,
                          inference::ModelInferResponse*) override;
  grpc::Status ModelStreamInfer(
      grpc::ServerContext*,
      grpc::ServerReaderWriter<inference::ModelInferResponse,
                               inference::ModelInferRequest>*) override;

 private:
  std::shared_ptr<ModelRegistry> registry_;
  std::shared_ptr<BatcherRegistry> batchers_;
  ServerConfig cfg_;
};

std::unique_ptr<grpc::Server> start_grpc_server(
    std::shared_ptr<ModelRegistry> registry,
    std::shared_ptr<BatcherRegistry> batchers, const ServerConfig& cfg);

}  // namespace omle_server

#endif  // OMLE_SERVER_GRPC_INFERENCE_SERVICE_H_

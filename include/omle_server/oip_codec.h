#ifndef OMLE_SERVER_OIP_CODEC_H_
#define OMLE_SERVER_OIP_CODEC_H_

#include <omle/runtime.h>
#include <omle/status.h>
#include <omle/tensor.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "open_inference_grpc.grpc.pb.h"

namespace omle_server {

// Translates between the Open Inference Protocol wire format (JSON / protobuf)
// and omle tensors.
struct OipCodec {
  // ── REST JSON ────────────────────────────────────────────────────────────

  // Parse an OIP /infer JSON body into a named-tensor map.
  // The body string must outlive the returned tensors (views where possible).
  static omle::rt::StatusOr<std::unordered_map<std::string, omle::rt::Tensor>>
  parse_infer_request_json(std::string_view body);

  // Serialize output tensors to an OIP infer-response JSON string.
  static std::string build_infer_response_json(
      const std::string& model_name, const std::string& request_id,
      const std::unordered_map<std::string, omle::rt::Tensor>& outputs,
      const std::vector<omle::rt::OutputSpec>& output_specs);

  // ── gRPC protobuf ────────────────────────────────────────────────────────

  // Decode ModelInferRequest → named-tensor map.
  static omle::rt::StatusOr<std::unordered_map<std::string, omle::rt::Tensor>>
  decode_grpc_request(const inference::ModelInferRequest& req);

  // Encode output tensors → ModelInferResponse (appended to *resp).
  static void encode_grpc_response(
      const std::string& model_name, const std::string& model_version,
      const std::string& request_id,
      const std::unordered_map<std::string, omle::rt::Tensor>& outputs,
      const std::vector<omle::rt::OutputSpec>& output_specs,
      inference::ModelInferResponse* resp);

  // ── dtype helpers ─────────────────────────────────────────────────────────

  static omle::rt::DataType oip_dtype_to_omle(std::string_view s);
  static std::string_view omle_to_oip_dtype(omle::rt::DataType dt);
};

}  // namespace omle_server

#endif  // OMLE_SERVER_OIP_CODEC_H_

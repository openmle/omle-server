#include "omle_server/oip_codec.h"

#include <simdjson.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace omle_server {

// ── dtype mapping
// ─────────────────────────────────────────────────────────────

omle::rt::DataType OipCodec::oip_dtype_to_omle(std::string_view s) {
  if (s == "FP32") return omle::rt::DataType::Float32;
  if (s == "FP64") return omle::rt::DataType::Float64;
  if (s == "INT8") return omle::rt::DataType::Int8;
  if (s == "INT16") return omle::rt::DataType::Int16;
  if (s == "INT32") return omle::rt::DataType::Int32;
  if (s == "INT64") return omle::rt::DataType::Int64;
  if (s == "UINT8") return omle::rt::DataType::UInt8;
  if (s == "UINT16") return omle::rt::DataType::UInt16;
  if (s == "UINT32") return omle::rt::DataType::UInt32;
  if (s == "UINT64") return omle::rt::DataType::UInt64;
  if (s == "BOOL") return omle::rt::DataType::Bool;
  if (s == "STRING") return omle::rt::DataType::String;
  if (s == "BYTES") return omle::rt::DataType::Bytes;
  if (s == "FP16") return omle::rt::DataType::Float16;
  return omle::rt::DataType::Unknown;
}

std::string_view OipCodec::omle_to_oip_dtype(omle::rt::DataType dt) {
  switch (dt) {
    case omle::rt::DataType::Float32:
      return "FP32";
    case omle::rt::DataType::Float64:
      return "FP64";
    case omle::rt::DataType::Int8:
      return "INT8";
    case omle::rt::DataType::Int16:
      return "INT16";
    case omle::rt::DataType::Int32:
      return "INT32";
    case omle::rt::DataType::Int64:
      return "INT64";
    case omle::rt::DataType::UInt8:
      return "UINT8";
    case omle::rt::DataType::UInt16:
      return "UINT16";
    case omle::rt::DataType::UInt32:
      return "UINT32";
    case omle::rt::DataType::UInt64:
      return "UINT64";
    case omle::rt::DataType::Bool:
      return "BOOL";
    case omle::rt::DataType::String:
      return "STRING";
    case omle::rt::DataType::Bytes:
      return "BYTES";
    case omle::rt::DataType::Float16:
      return "FP16";
    default:
      return "BYTES";
  }
}

// ── helpers
// ───────────────────────────────────────────────────────────────────

namespace {

// Resolve a -1 (dynamic) dimension from actual element count.
static void resolve_dynamic_dims(int& rows, int& cols, int total) {
  if (rows == -1 && cols > 0)
    rows = total / cols;
  else if (cols == -1 && rows > 0)
    cols = total / rows;
  else if (rows == -1 && cols == -1) {
    rows = total;
    cols = 1;
  }
}

// Build a dense omle::rt::Tensor from an OIP JSON "data" array and shape.
// rows/cols may be -1 to indicate a dynamic dimension inferred from data count.
omle::rt::StatusOr<omle::rt::Tensor> tensor_from_json_data(
    simdjson::ondemand::array& data_arr, omle::rt::DataType dtype, int rows,
    int cols) {
  if (dtype == omle::rt::DataType::String) {
    std::vector<std::string> strs;
    for (auto v : data_arr) {
      std::string_view sv;
      if (v.get_string().get(sv) != simdjson::SUCCESS)
        return {omle::rt::ErrorCode::InvalidArgument,
                "expected string value in data"};
      strs.emplace_back(sv);
    }
    resolve_dynamic_dims(rows, cols, (int)strs.size());
    return omle::rt::Tensor::strings(rows, cols, std::move(strs));
  }

  int esz = omle::rt::dtype_size(dtype);
  // Collect all elements — required when a dimension is dynamic (-1).
  std::vector<uint8_t> raw;
  if (rows > 0 && cols > 0) raw.reserve(static_cast<size_t>(rows * cols) * esz);

  // simdjson's .get() return codes are intentionally ignored here;
  // malformed values silently default to zero, which is harmless.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-result"
  for (auto v : data_arr) {
    size_t pos = raw.size();
    raw.resize(pos + esz);
    uint8_t* dst = raw.data() + pos;
    switch (dtype) {
      case omle::rt::DataType::Float32: {
        double d;
        v.get_double().get(d);
        float f = (float)d;
        std::memcpy(dst, &f, 4);
        break;
      }
      case omle::rt::DataType::Float64: {
        double d;
        v.get_double().get(d);
        std::memcpy(dst, &d, 8);
        break;
      }
      case omle::rt::DataType::Int8: {
        int64_t x;
        v.get_int64().get(x);
        int8_t c = (int8_t)x;
        std::memcpy(dst, &c, 1);
        break;
      }
      case omle::rt::DataType::Int16: {
        int64_t x;
        v.get_int64().get(x);
        int16_t c = (int16_t)x;
        std::memcpy(dst, &c, 2);
        break;
      }
      case omle::rt::DataType::Int32: {
        int64_t x;
        v.get_int64().get(x);
        int32_t c = (int32_t)x;
        std::memcpy(dst, &c, 4);
        break;
      }
      case omle::rt::DataType::Int64: {
        int64_t x;
        v.get_int64().get(x);
        std::memcpy(dst, &x, 8);
        break;
      }
      case omle::rt::DataType::UInt8: {
        uint64_t x;
        v.get_uint64().get(x);
        uint8_t c = (uint8_t)x;
        std::memcpy(dst, &c, 1);
        break;
      }
      case omle::rt::DataType::UInt16: {
        uint64_t x;
        v.get_uint64().get(x);
        uint16_t c = (uint16_t)x;
        std::memcpy(dst, &c, 2);
        break;
      }
      case omle::rt::DataType::UInt32: {
        uint64_t x;
        v.get_uint64().get(x);
        uint32_t c = (uint32_t)x;
        std::memcpy(dst, &c, 4);
        break;
      }
      case omle::rt::DataType::UInt64: {
        uint64_t x;
        v.get_uint64().get(x);
        std::memcpy(dst, &x, 8);
        break;
      }
      case omle::rt::DataType::Bool: {
        bool b;
        v.get_bool().get(b);
        uint8_t c = b ? 1 : 0;
        std::memcpy(dst, &c, 1);
        break;
      }
      default:
        break;
    }
  }
#pragma clang diagnostic pop

  resolve_dynamic_dims(rows, cols, (int)(raw.size() / esz));
  return omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(raw));
}

// Append the elements of a tensor to a JSON array.
json tensor_to_json_array(const omle::rt::Tensor& t) {
  json arr = json::array();
  if (t.is_string()) {
    for (int r = 0; r < t.n_rows; ++r)
      for (int c = 0; c < t.n_cols; ++c) arr.push_back(t.str_at(r, c));
    return arr;
  }
  for (int r = 0; r < t.n_rows; ++r)
    for (int c = 0; c < t.n_cols; ++c) arr.push_back(t.get(r, c));
  return arr;
}

}  // namespace

// ── REST JSON parse
// ───────────────────────────────────────────────────────────

omle::rt::StatusOr<std::unordered_map<std::string, omle::rt::Tensor>>
OipCodec::parse_infer_request_json(std::string_view body) {
  // simdjson on-demand requires SIMDJSON_PADDING extra bytes at end.
  simdjson::padded_string padded(body.data(), body.size());
  simdjson::ondemand::parser parser;

  simdjson::ondemand::document doc;
  if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
    return {omle::rt::ErrorCode::InvalidArgument, "invalid JSON body"};

  simdjson::ondemand::array inputs_arr;
  if (doc["inputs"].get_array().get(inputs_arr) != simdjson::SUCCESS)
    return {omle::rt::ErrorCode::InvalidArgument, "missing 'inputs' field"};

  std::unordered_map<std::string, omle::rt::Tensor> result;

  for (auto input_val : inputs_arr) {
    simdjson::ondemand::object input;
    if (input_val.get_object().get(input) != simdjson::SUCCESS)
      return {omle::rt::ErrorCode::InvalidArgument,
              "each input must be an object"};

    std::string_view name_sv;
    if (input["name"].get_string().get(name_sv) != simdjson::SUCCESS)
      return {omle::rt::ErrorCode::InvalidArgument, "input missing 'name'"};
    std::string name(name_sv);

    std::string_view dtype_sv;
    if (input["datatype"].get_string().get(dtype_sv) != simdjson::SUCCESS)
      return {omle::rt::ErrorCode::InvalidArgument,
              "input '" + name + "' missing 'datatype'"};
    omle::rt::DataType dtype = oip_dtype_to_omle(dtype_sv);
    if (dtype == omle::rt::DataType::Unknown)
      return {omle::rt::ErrorCode::InvalidArgument,
              "unknown datatype '" + std::string(dtype_sv) + "'"};

    // Parse shape → [rows, cols]
    simdjson::ondemand::array shape_arr;
    if (input["shape"].get_array().get(shape_arr) != simdjson::SUCCESS)
      return {omle::rt::ErrorCode::InvalidArgument,
              "input '" + name + "' missing 'shape'"};

    std::vector<int64_t> shape;
    for (auto sv : shape_arr) {
      int64_t dim;
      if (sv.get_int64().get(dim) != simdjson::SUCCESS)
        return {omle::rt::ErrorCode::InvalidArgument,
                "shape must be integer array"};
      shape.push_back(dim);
    }

    int rows = 1, cols = 1;
    if (shape.size() >= 1) rows = (int)shape[0];
    if (shape.size() >= 2) cols = (int)shape[1];
    // For higher-rank tensors, flatten static dimensions after the first into
    // cols.
    for (size_t i = 2; i < shape.size(); ++i) {
      if (shape[i] != -1) cols = (cols == -1 ? 1 : cols) * (int)shape[i];
    }

    // Check for binary data in "data" field.
    simdjson::ondemand::array data_arr;
    if (input["data"].get_array().get(data_arr) == simdjson::SUCCESS) {
      auto t_or = tensor_from_json_data(data_arr, dtype, rows, cols);
      if (!t_or.ok()) return t_or.status();
      result[name] = std::move(*t_or);
    } else {
      // Empty tensor — will fail at predict time with a meaningful error.
      result[name] = omle::rt::Tensor::dense(dtype, rows, cols);
    }
  }

  return result;
}

// ── REST JSON response
// ────────────────────────────────────────────────────────

std::string OipCodec::build_infer_response_json(
    const std::string& model_name, const std::string& request_id,
    const std::unordered_map<std::string, omle::rt::Tensor>& outputs,
    const std::vector<omle::rt::OutputSpec>& output_specs) {
  json resp;
  resp["model_name"] = model_name;
  if (!request_id.empty()) resp["id"] = request_id;
  resp["outputs"] = json::array();

  // Emit outputs in spec order; fall back to map iteration order.
  auto emit = [&](const std::string& oname, const omle::rt::Tensor& t) {
    json out;
    out["name"] = oname;
    out["datatype"] = omle_to_oip_dtype(t.dtype);
    out["shape"] = json::array({t.n_rows, t.n_cols});
    out["data"] = tensor_to_json_array(t);
    resp["outputs"].push_back(std::move(out));
  };

  if (!output_specs.empty()) {
    for (auto& spec : output_specs) {
      auto it = outputs.find(spec.name);
      if (it != outputs.end()) emit(it->first, it->second);
    }
  } else {
    for (auto& [n, t] : outputs) emit(n, t);
  }

  return resp.dump();
}

// ── gRPC decode
// ───────────────────────────────────────────────────────────────

omle::rt::StatusOr<std::unordered_map<std::string, omle::rt::Tensor>>
OipCodec::decode_grpc_request(const inference::ModelInferRequest& req) {
  std::unordered_map<std::string, omle::rt::Tensor> result;

  const bool has_raw = req.raw_input_contents_size() > 0;

  for (int i = 0; i < req.inputs_size(); ++i) {
    const auto& inp = req.inputs(i);
    const std::string& name = inp.name();

    omle::rt::DataType dtype = oip_dtype_to_omle(inp.datatype());
    if (dtype == omle::rt::DataType::Unknown)
      return {
          omle::rt::ErrorCode::InvalidArgument,
          "unknown datatype '" + inp.datatype() + "' for input '" + name + "'"};

    int rows = 1, cols = 1;
    if (inp.shape_size() >= 1) rows = (int)inp.shape(0);
    if (inp.shape_size() >= 2) cols = (int)inp.shape(1);
    for (int d = 2; d < inp.shape_size(); ++d) {
      if (inp.shape(d) != -1)
        cols = (cols == -1 ? 1 : cols) * (int)inp.shape(d);
    }

    if (has_raw && i < req.raw_input_contents_size()) {
      // Binary fast path: raw bytes provided by caller.
      const std::string& raw = req.raw_input_contents(i);
      std::vector<uint8_t> buf(raw.begin(), raw.end());
      int esz = omle::rt::dtype_size(dtype);
      resolve_dynamic_dims(rows, cols, esz > 0 ? (int)(buf.size() / esz) : 0);
      result[name] =
          omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(buf));
    } else {
      // Typed contents path.
      const auto& c = inp.contents();
      // Compute n from actual content count when a dimension is dynamic.
      int content_count = 0;
      if (dtype == omle::rt::DataType::Float32)
        content_count = c.fp32_contents_size();
      else if (dtype == omle::rt::DataType::Float64)
        content_count = c.fp64_contents_size();
      else if (dtype == omle::rt::DataType::Int32 ||
               dtype == omle::rt::DataType::Int8 ||
               dtype == omle::rt::DataType::Int16)
        content_count = c.int_contents_size();
      else if (dtype == omle::rt::DataType::Int64)
        content_count = c.int64_contents_size();
      else if (dtype == omle::rt::DataType::UInt32 ||
               dtype == omle::rt::DataType::UInt8 ||
               dtype == omle::rt::DataType::UInt16)
        content_count = c.uint_contents_size();
      else if (dtype == omle::rt::DataType::UInt64)
        content_count = c.uint64_contents_size();
      else if (dtype == omle::rt::DataType::String)
        content_count = c.bytes_contents_size();
      else if (dtype == omle::rt::DataType::Bool)
        content_count = c.bool_contents_size();
      resolve_dynamic_dims(rows, cols, content_count);
      int n = rows * cols;
      if (dtype == omle::rt::DataType::Float32) {
        std::vector<float> v(c.fp32_contents().begin(),
                             c.fp32_contents().end());
        v.resize(n, 0.f);
        result[name] = omle::rt::Tensor::from_floats(rows, cols, std::move(v));
      } else if (dtype == omle::rt::DataType::Float64) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * 8);
        int j = 0;
        for (double d : c.fp64_contents()) {
          if (j >= n) break;
          std::memcpy(raw.data() + j * 8, &d, 8);
          ++j;
        }
        result[name] =
            omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(raw));
      } else if (dtype == omle::rt::DataType::Int64) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * 8);
        int j = 0;
        for (int64_t x : c.int64_contents()) {
          if (j >= n) break;
          std::memcpy(raw.data() + j * 8, &x, 8);
          ++j;
        }
        result[name] =
            omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(raw));
      } else if (dtype == omle::rt::DataType::Int32) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * 4);
        int j = 0;
        for (int32_t x : c.int_contents()) {
          if (j >= n) break;
          std::memcpy(raw.data() + j * 4, &x, 4);
          ++j;
        }
        result[name] =
            omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(raw));
      } else if (dtype == omle::rt::DataType::String) {
        std::vector<std::string> strs(c.bytes_contents().begin(),
                                      c.bytes_contents().end());
        strs.resize(n);
        result[name] = omle::rt::Tensor::strings(rows, cols, std::move(strs));
      } else {
        // Generic: copy raw bytes from bytes_contents if provided.
        if (!c.bytes_contents().empty()) {
          const std::string& raw = c.bytes_contents(0);
          std::vector<uint8_t> buf(raw.begin(), raw.end());
          result[name] =
              omle::rt::Tensor::from_raw(dtype, rows, cols, std::move(buf));
        } else {
          result[name] = omle::rt::Tensor::dense(dtype, rows, cols);
        }
      }
    }
  }
  return result;
}

// ── gRPC encode
// ───────────────────────────────────────────────────────────────

void OipCodec::encode_grpc_response(
    const std::string& model_name, const std::string& model_version,
    const std::string& request_id,
    const std::unordered_map<std::string, omle::rt::Tensor>& outputs,
    const std::vector<omle::rt::OutputSpec>& output_specs,
    inference::ModelInferResponse* resp) {
  resp->set_model_name(model_name);
  if (!model_version.empty()) resp->set_model_version(model_version);
  if (!request_id.empty()) resp->set_id(request_id);

  // Emit in spec order where available.
  auto emit = [&](const std::string& oname, const omle::rt::Tensor& t) {
    auto* out = resp->add_outputs();
    out->set_name(oname);
    out->set_datatype(std::string(omle_to_oip_dtype(t.dtype)));
    out->add_shape(t.n_rows);
    out->add_shape(t.n_cols);

    // Use raw_output_contents (binary) for numeric tensors — faster.
    if (!t.is_string() && !t.is_sparse()) {
      // raw_data() already returns the scalar buffer for Kind::Scalar.
      const void* ptr = t.raw_data();
      size_t nbytes = t.data_bytes();
      resp->add_raw_output_contents(
          std::string(static_cast<const char*>(ptr), nbytes));
    } else if (t.is_string()) {
      auto* c = out->mutable_contents();
      for (int r = 0; r < t.n_rows; ++r)
        for (int cc = 0; cc < t.n_cols; ++cc)
          c->add_bytes_contents(t.str_at(r, cc));
    }
  };

  if (!output_specs.empty()) {
    for (auto& spec : output_specs) {
      auto it = outputs.find(spec.name);
      if (it != outputs.end()) emit(it->first, it->second);
    }
  } else {
    for (auto& [n, t] : outputs) emit(n, t);
  }
}

}  // namespace omle_server

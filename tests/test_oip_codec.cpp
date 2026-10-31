#include <gtest/gtest.h>
#include <omle/runtime.h>

#include "omle_server/oip_codec.h"

using namespace omle_server;

// ── dtype round-trip
// ──────────────────────────────────────────────────────────

TEST(OipDtype, KnownStringsMapToCorrectType) {
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("FP32"), omle::rt::DataType::Float32);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("FP64"), omle::rt::DataType::Float64);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("INT8"), omle::rt::DataType::Int8);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("INT16"), omle::rt::DataType::Int16);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("INT32"), omle::rt::DataType::Int32);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("INT64"), omle::rt::DataType::Int64);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("UINT8"), omle::rt::DataType::UInt8);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("UINT16"), omle::rt::DataType::UInt16);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("UINT32"), omle::rt::DataType::UInt32);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("UINT64"), omle::rt::DataType::UInt64);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("BOOL"), omle::rt::DataType::Bool);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("STRING"), omle::rt::DataType::String);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("BYTES"), omle::rt::DataType::Bytes);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("FP16"), omle::rt::DataType::Float16);
}

TEST(OipDtype, UnknownStringReturnsUnknown) {
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("FLOAT32"),
            omle::rt::DataType::Unknown);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle(""), omle::rt::DataType::Unknown);
  EXPECT_EQ(OipCodec::oip_dtype_to_omle("INVALID"),
            omle::rt::DataType::Unknown);
}

TEST(OipDtype, RoundTripAllTypes) {
  const std::vector<omle::rt::DataType> types = {
      omle::rt::DataType::Float32, omle::rt::DataType::Float64,
      omle::rt::DataType::Int8,    omle::rt::DataType::Int16,
      omle::rt::DataType::Int32,   omle::rt::DataType::Int64,
      omle::rt::DataType::UInt8,   omle::rt::DataType::UInt16,
      omle::rt::DataType::UInt32,  omle::rt::DataType::UInt64,
      omle::rt::DataType::Bool,    omle::rt::DataType::String,
      omle::rt::DataType::Bytes,   omle::rt::DataType::Float16,
  };
  for (auto dt : types) {
    auto oip_str = OipCodec::omle_to_oip_dtype(dt);
    EXPECT_EQ(OipCodec::oip_dtype_to_omle(oip_str), dt)
        << "Round-trip failed for dtype " << static_cast<int>(dt);
  }
}

// ── parse_infer_request_json
// ──────────────────────────────────────────────────

TEST(ParseInferRequest, ValidFP32Input) {
  std::string body = R"({
        "inputs": [{
            "name": "features",
            "datatype": "FP32",
            "shape": [2, 3],
            "data": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
        }]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  ASSERT_TRUE(result.ok()) << result.message();
  ASSERT_EQ(result->size(), 1u);
  auto& t = (*result)["features"];
  EXPECT_EQ(t.n_rows, 2);
  EXPECT_EQ(t.n_cols, 3);
  EXPECT_EQ(t.dtype, omle::rt::DataType::Float32);
}

TEST(ParseInferRequest, ValidFP64Input) {
  std::string body = R"({
        "inputs": [{
            "name": "X",
            "datatype": "FP64",
            "shape": [1, 2],
            "data": [1.5, 2.5]
        }]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  ASSERT_TRUE(result.ok()) << result.message();
  auto& t = (*result)["X"];
  EXPECT_EQ(t.n_rows, 1);
  EXPECT_EQ(t.n_cols, 2);
  EXPECT_EQ(t.dtype, omle::rt::DataType::Float64);
  EXPECT_NEAR(t.get(0, 0), 1.5, 1e-9);
  EXPECT_NEAR(t.get(0, 1), 2.5, 1e-9);
}

TEST(ParseInferRequest, ValidStringInput) {
  std::string body = R"({
        "inputs": [{
            "name": "cats",
            "datatype": "STRING",
            "shape": [3, 1],
            "data": ["a", "b", "c"]
        }]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  ASSERT_TRUE(result.ok()) << result.message();
  auto& t = (*result)["cats"];
  EXPECT_EQ(t.n_rows, 3);
  EXPECT_EQ(t.n_cols, 1);
  EXPECT_TRUE(t.is_string());
  EXPECT_EQ(t.str_at(0, 0), "a");
  EXPECT_EQ(t.str_at(2, 0), "c");
}

TEST(ParseInferRequest, MultipleInputs) {
  std::string body = R"({
        "inputs": [
            {"name": "a", "datatype": "FP32", "shape": [1, 1], "data": [1.0]},
            {"name": "b", "datatype": "INT64", "shape": [1, 1], "data": [42]}
        ]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  ASSERT_TRUE(result.ok()) << result.message();
  EXPECT_EQ(result->size(), 2u);
  EXPECT_EQ(result->count("a"), 1u);
  EXPECT_EQ(result->count("b"), 1u);
}

TEST(ParseInferRequest, DynamicBatchDimension) {
  // shape [-1, 2] with 6 values should resolve to 3 rows, 2 cols
  std::string body = R"({
        "inputs": [{
            "name": "X",
            "datatype": "FP32",
            "shape": [-1, 2],
            "data": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
        }]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  ASSERT_TRUE(result.ok()) << result.message();
  auto& t = (*result)["X"];
  EXPECT_EQ(t.n_rows, 3);
  EXPECT_EQ(t.n_cols, 2);
}

TEST(ParseInferRequest, MissingInputsField) {
  auto result = OipCodec::parse_infer_request_json(R"({"model_name": "foo"})");
  EXPECT_FALSE(result.ok());
}

TEST(ParseInferRequest, InvalidJson) {
  auto result = OipCodec::parse_infer_request_json("not json {{");
  EXPECT_FALSE(result.ok());
}

TEST(ParseInferRequest, MissingName) {
  std::string body = R"({
        "inputs": [{"datatype": "FP32", "shape": [1, 1], "data": [1.0]}]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  EXPECT_FALSE(result.ok());
}

TEST(ParseInferRequest, UnknownDatatype) {
  std::string body = R"({
        "inputs": [{"name": "x", "datatype": "FLOAT32", "shape": [1, 1], "data": [1.0]}]
    })";
  auto result = OipCodec::parse_infer_request_json(body);
  EXPECT_FALSE(result.ok());
}

// ── build_infer_response_json
// ─────────────────────────────────────────────────

TEST(BuildInferResponse, SingleOutputIncludesModelNameAndData) {
  std::unordered_map<std::string, omle::rt::Tensor> outputs;
  std::vector<float> vals = {0.7f, 0.3f};
  outputs["proba"] = omle::rt::Tensor::from_floats(1, 2, vals);

  std::string json =
      OipCodec::build_infer_response_json("my_model", "req-1", outputs, {});

  EXPECT_NE(json.find("my_model"), std::string::npos);
  EXPECT_NE(json.find("req-1"), std::string::npos);
  EXPECT_NE(json.find("proba"), std::string::npos);
  EXPECT_NE(json.find("FP32"), std::string::npos);
}

TEST(BuildInferResponse, EmptyRequestIdNotIncluded) {
  std::unordered_map<std::string, omle::rt::Tensor> outputs;
  outputs["out"] = omle::rt::Tensor::from_floats(1, 1, {1.0f});

  std::string json = OipCodec::build_infer_response_json("m", "", outputs, {});
  EXPECT_EQ(json.find("\"id\""), std::string::npos);
}

TEST(BuildInferResponse, OutputSpecOrderRespected) {
  std::unordered_map<std::string, omle::rt::Tensor> outputs;
  outputs["b"] = omle::rt::Tensor::from_floats(1, 1, {2.0f});
  outputs["a"] = omle::rt::Tensor::from_floats(1, 1, {1.0f});

  omle::rt::OutputSpec spec_a;
  spec_a.name = "a";
  omle::rt::OutputSpec spec_b;
  spec_b.name = "b";

  std::string json =
      OipCodec::build_infer_response_json("m", "", outputs, {spec_a, spec_b});
  auto pos_a = json.find("\"a\"");
  auto pos_b = json.find("\"b\"");
  EXPECT_LT(pos_a, pos_b) << "spec order not preserved";
}

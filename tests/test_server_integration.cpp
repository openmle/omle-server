// Integration tests for omle-server REST and gRPC endpoints.
//
// Starts the server binary (SERVER_BINARY_PATH) pointing at a temporary model
// directory populated with a copy of test_model_2f.omle.
// REST calls use a minimal synchronous HTTP/1.1 client over POSIX sockets.
// gRPC calls use the generated C++ synchronous stubs.

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// POSIX (fork/exec/sockets)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// gRPC
#include <grpcpp/grpcpp.h>

#include "open_inference_grpc.grpc.pb.h"

// JSON parsing for REST responses
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Compile-time paths set by CMake ──────────────────────────────────────────

#ifndef SERVER_BINARY_PATH
#define SERVER_BINARY_PATH ""
#endif

#ifndef TEST_MODEL_SRC_PATH
#define TEST_MODEL_SRC_PATH ""
#endif

// Ports chosen to avoid conflicts with the default 8080/8081.
static constexpr int REST_PORT = 28080;
static constexpr int GRPC_PORT = 28081;

static const std::string MODEL_NAME = "test_model_2f";

// ── Minimal synchronous HTTP/1.1 client ──────────────────────────────────────

struct HttpResp {
  int status = 0;
  std::string body;
};

static HttpResp http_request(const std::string& method, const std::string& path,
                             const std::string& req_body = "") {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};

  // 5-second receive timeout.
  struct timeval tv{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(REST_PORT));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }

  std::string req = method + " " + path +
                    " HTTP/1.1\r\n"
                    "Host: 127.0.0.1:" +
                    std::to_string(REST_PORT) +
                    "\r\n"
                    "Connection: close\r\n";
  if (!req_body.empty()) {
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(req_body.size()) + "\r\n";
  }
  req += "\r\n" + req_body;

  ::send(fd, req.c_str(), req.size(), 0);

  std::string raw;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
    raw.append(buf, static_cast<size_t>(n));
  ::close(fd);

  if (raw.size() < 12) return {};

  HttpResp resp;
  try {
    resp.status = std::stoi(raw.substr(9, 3));
  } catch (...) {
  }

  auto sep = raw.find("\r\n\r\n");
  if (sep != std::string::npos) resp.body = raw.substr(sep + 4);
  return resp;
}

static HttpResp http_get(const std::string& path) {
  return http_request("GET", path);
}

static HttpResp http_post(const std::string& path, const std::string& body) {
  return http_request("POST", path, body);
}

// ── Server lifecycle
// ──────────────────────────────────────────────────────────

static pid_t g_server_pid = -1;
static fs::path g_model_dir;
static bool g_server_up = false;

static bool poll_port(int port, double timeout_sec) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::duration<double>(timeout_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (ok) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

class ServerEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    const std::string bin = SERVER_BINARY_PATH;
    if (bin.empty() || !fs::exists(bin)) {
      GTEST_SKIP() << "Server binary not found: '" << bin
                   << "' — build with cmake .. first";
      return;
    }

    // Create temp model dir and copy the test model.
    g_model_dir = fs::temp_directory_path() / "omle_integ_test";
    fs::create_directories(g_model_dir);

    // Hard failure, not a silent skip. Without the model the server still
    // starts and answers /v2/health/*, so the suite used to report 24 failing
    // tests with 503s and no mention of the file that was never copied.
    const std::string src = TEST_MODEL_SRC_PATH;
    ASSERT_FALSE(src.empty()) << "TEST_MODEL_SRC_PATH is empty — the CMake "
                                 "cache variable TEST_MODEL_SRC is unset";
    ASSERT_TRUE(fs::exists(src))
        << "Test model not found: " << src
        << "\nThe integration test needs it in the model directory; every "
           "model-dependent test returns 503 without it.";
    fs::copy_file(src, g_model_dir / (MODEL_NAME + ".omle"),
                  fs::copy_options::overwrite_existing);

    // Fork + exec the server.
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork() failed: " << strerror(errno);

    if (pid == 0) {
      // Child: set env vars, suppress output, exec.
      setenv("OMLE_MODEL_DIR", g_model_dir.c_str(), 1);
      setenv("OMLE_REST_PORT", std::to_string(REST_PORT).c_str(), 1);
      setenv("OMLE_GRPC_PORT", std::to_string(GRPC_PORT).c_str(), 1);
      int null_fd = ::open("/dev/null", O_WRONLY);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      ::execl(bin.c_str(), bin.c_str(), nullptr);
      ::_exit(1);
    }

    g_server_pid = pid;
    if (!poll_port(REST_PORT, 20.0)) {
      ::kill(g_server_pid, SIGKILL);
      ::waitpid(g_server_pid, nullptr, 0);
      g_server_pid = -1;
      GTEST_SKIP() << "Server did not become ready within 20 s";
      return;
    }

    g_server_up = true;
  }

  void TearDown() override {
    if (g_server_pid > 0) {
      ::kill(g_server_pid, SIGTERM);
      ::waitpid(g_server_pid, nullptr, 0);
      g_server_pid = -1;
    }
    if (!g_model_dir.empty()) {
      fs::remove_all(g_model_dir);
      g_model_dir.clear();
    }
    g_server_up = false;
  }
};

// Custom main so we can register the environment.
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new ServerEnvironment);
  return RUN_ALL_TESTS();
}

// ── Helper: skip test if server failed to start
// ───────────────────────────────

#define REQUIRE_SERVER()                                    \
  do {                                                      \
    if (!g_server_up) GTEST_SKIP() << "server not running"; \
  } while (0)

// ── REST: Health
// ──────────────────────────────────────────────────────────────

TEST(RestHealth, ServerLive) {
  REQUIRE_SERVER();
  auto r = http_get("/v2/health/live");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(json::parse(r.body), json::object());
}

TEST(RestHealth, ServerReady) {
  REQUIRE_SERVER();
  auto r = http_get("/v2/health/ready");
  EXPECT_EQ(r.status, 200);
}

// ── REST: Server metadata
// ─────────────────────────────────────────────────────

TEST(RestServerMetadata, HasRequiredFields) {
  REQUIRE_SERVER();
  auto r = http_get("/v2");
  ASSERT_EQ(r.status, 200);
  auto j = json::parse(r.body);
  EXPECT_TRUE(j.contains("name"));
  EXPECT_TRUE(j.contains("version"));
  EXPECT_TRUE(j.contains("extensions"));
}

// ── REST: Model readiness
// ─────────────────────────────────────────────────────

TEST(RestModelReady, KnownModel) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_get("/v2/models/" + MODEL_NAME + "/ready").status, 200);
}

TEST(RestModelReady, UnknownModel) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_get("/v2/models/no_such_model/ready").status, 503);
}

TEST(RestModelReady, VersionedKnown) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_get("/v2/models/" + MODEL_NAME + "/versions/1/ready").status,
            200);
}

TEST(RestModelReady, VersionedWrong) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_get("/v2/models/" + MODEL_NAME + "/versions/99/ready").status,
            503);
}

// ── REST: Model metadata
// ──────────────────────────────────────────────────────

TEST(RestModelMetadata, KnownModel) {
  REQUIRE_SERVER();
  auto r = http_get("/v2/models/" + MODEL_NAME);
  ASSERT_EQ(r.status, 200);
  auto j = json::parse(r.body);
  EXPECT_EQ(j["name"], MODEL_NAME);
  ASSERT_TRUE(j.contains("inputs") && !j["inputs"].empty());
  // Matches test_model_2f.omle's declared input, which is FP64 — the same
  // value the Python metadata tests assert. The two suites disagreed here
  // until the fixture was regenerated.
  EXPECT_EQ(j["inputs"][0]["datatype"], "FP64");
  EXPECT_TRUE(j.contains("outputs"));
}

TEST(RestModelMetadata, UnknownModel) {
  REQUIRE_SERVER();
  auto r = http_get("/v2/models/no_such_model");
  EXPECT_EQ(r.status, 404);
  EXPECT_TRUE(json::parse(r.body).contains("error"));
}

TEST(RestModelMetadata, VersionedModel) {
  REQUIRE_SERVER();
  auto r = http_get("/v2/models/" + MODEL_NAME + "/versions/1");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(json::parse(r.body)["name"], MODEL_NAME);
}

// ── REST: Inference
// ───────────────────────────────────────────────────────────

static std::string infer_body(const std::string& name, const std::string& dtype,
                              std::initializer_list<int> shape,
                              std::initializer_list<double> data) {
  json j = {{"inputs",
             {{{"name", name},
               {"datatype", dtype},
               {"shape", std::vector<int>(shape)},
               {"data", std::vector<double>(data)}}}}};
  return j.dump();
}

TEST(RestInfer, SingleRow) {
  REQUIRE_SERVER();
  auto r = http_post("/v2/models/" + MODEL_NAME + "/infer",
                     infer_body("X", "FP64", {1, 2}, {1.0, 2.0}));
  ASSERT_EQ(r.status, 200);
  auto j = json::parse(r.body);
  EXPECT_EQ(j["model_name"], MODEL_NAME);
  ASSERT_FALSE(j["outputs"].empty());
  EXPECT_EQ(j["outputs"][0]["shape"][0], 1);
}

TEST(RestInfer, BatchRows) {
  REQUIRE_SERVER();
  auto r = http_post(
      "/v2/models/" + MODEL_NAME + "/infer",
      infer_body("X", "FP64", {3, 2}, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(json::parse(r.body)["outputs"][0]["shape"][0], 3);
}

TEST(RestInfer, WithRequestId) {
  REQUIRE_SERVER();
  json payload = {{"id", "cpp-req-1"},
                  {"inputs",
                   {{{"name", "X"},
                     {"datatype", "FP64"},
                     {"shape", {1, 2}},
                     {"data", {0.0, 1.0}}}}}};
  auto r = http_post("/v2/models/" + MODEL_NAME + "/infer", payload.dump());
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(json::parse(r.body)["id"], "cpp-req-1");
}

TEST(RestInfer, DynamicBatchDim) {
  REQUIRE_SERVER();
  auto r = http_post("/v2/models/" + MODEL_NAME + "/infer",
                     infer_body("X", "FP64", {-1, 2}, {1.0, 2.0, 3.0, 4.0}));
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(json::parse(r.body)["outputs"][0]["shape"][0], 2);
}

TEST(RestInfer, VersionedEndpoint) {
  REQUIRE_SERVER();
  auto r = http_post("/v2/models/" + MODEL_NAME + "/versions/1/infer",
                     infer_body("X", "FP64", {1, 2}, {1.0, 0.5}));
  ASSERT_EQ(r.status, 200);
  EXPECT_FALSE(json::parse(r.body)["outputs"].empty());
}

TEST(RestInfer, UnknownModelReturns404) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_post("/v2/models/no_such/infer",
                      infer_body("X", "FP64", {1, 2}, {0.0, 0.0}))
                .status,
            404);
}

TEST(RestInfer, BadJsonReturns400) {
  REQUIRE_SERVER();
  EXPECT_EQ(
      http_post("/v2/models/" + MODEL_NAME + "/infer", "{{bad json").status,
      400);
}

TEST(RestInfer, MissingInputsReturns400) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_post("/v2/models/" + MODEL_NAME + "/infer",
                      json({{"model_name", MODEL_NAME}}).dump())
                .status,
            400);
}

TEST(RestInfer, UnknownDatatypeReturns400) {
  REQUIRE_SERVER();
  EXPECT_EQ(http_post("/v2/models/" + MODEL_NAME + "/infer",
                      infer_body("X", "FLOAT32", {1, 2}, {0.0, 0.0}))
                .status,
            400);
}

// ── REST: Prometheus /metrics
// ─────────────────────────────────────────────────

TEST(RestMetrics, EndpointReturns200) {
  REQUIRE_SERVER();
  auto r = http_get("/metrics");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("omle_models_loaded"), std::string::npos);
  EXPECT_NE(r.body.find("omle_requests_total"), std::string::npos);
}

TEST(RestMetrics, UpdatedAfterInfer) {
  REQUIRE_SERVER();
  http_post("/v2/models/" + MODEL_NAME + "/infer",
            infer_body("X", "FP64", {1, 2}, {2.0, 3.0}));
  auto r = http_get("/metrics");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find(MODEL_NAME), std::string::npos);
}

// ── gRPC: shared stub
// ─────────────────────────────────────────────────────────

static std::shared_ptr<grpc::Channel> g_channel;
static std::unique_ptr<inference::GRPCInferenceService::Stub> g_stub;

class GrpcTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(GRPC_PORT),
                                    grpc::InsecureChannelCredentials());
    g_stub = inference::GRPCInferenceService::NewStub(g_channel);
  }

  grpc::ClientContext ctx_;  // fresh per test (ClientContext is single-use)
};

// ── gRPC: ServerLive / ServerReady
// ────────────────────────────────────────────

TEST_F(GrpcTest, ServerLive) {
  REQUIRE_SERVER();
  inference::ServerLiveResponse resp;
  auto s = g_stub->ServerLive(&ctx_, {}, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_TRUE(resp.live());
}

TEST_F(GrpcTest, ServerReady) {
  REQUIRE_SERVER();
  inference::ServerReadyResponse resp;
  auto s = g_stub->ServerReady(&ctx_, {}, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_TRUE(resp.ready());
}

// ── gRPC: ModelReady
// ──────────────────────────────────────────────────────────

TEST_F(GrpcTest, ModelReadyKnown) {
  REQUIRE_SERVER();
  inference::ModelReadyRequest req;
  req.set_name(MODEL_NAME);
  inference::ModelReadyResponse resp;
  ASSERT_TRUE(g_stub->ModelReady(&ctx_, req, &resp).ok());
  EXPECT_TRUE(resp.ready());
}

TEST_F(GrpcTest, ModelReadyVersioned) {
  REQUIRE_SERVER();
  inference::ModelReadyRequest req;
  req.set_name(MODEL_NAME);
  req.set_version("1");
  inference::ModelReadyResponse resp;
  ASSERT_TRUE(g_stub->ModelReady(&ctx_, req, &resp).ok());
  EXPECT_TRUE(resp.ready());
}

TEST_F(GrpcTest, ModelReadyUnknown) {
  REQUIRE_SERVER();
  inference::ModelReadyRequest req;
  req.set_name("no_such");
  inference::ModelReadyResponse resp;
  ASSERT_TRUE(g_stub->ModelReady(&ctx_, req, &resp).ok());
  EXPECT_FALSE(resp.ready());
}

// ── gRPC: ServerMetadata / ModelMetadata ─────────────────────────────────────

TEST_F(GrpcTest, ServerMetadata) {
  REQUIRE_SERVER();
  inference::ServerMetadataResponse resp;
  auto s = g_stub->ServerMetadata(&ctx_, {}, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_FALSE(resp.name().empty());
  EXPECT_FALSE(resp.version().empty());
}

TEST_F(GrpcTest, ModelMetadataKnown) {
  REQUIRE_SERVER();
  inference::ModelMetadataRequest req;
  req.set_name(MODEL_NAME);
  inference::ModelMetadataResponse resp;
  auto s = g_stub->ModelMetadata(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(resp.name(), MODEL_NAME);
  ASSERT_GT(resp.inputs_size(), 0);
  EXPECT_EQ(resp.inputs(0).datatype(), "FP64");  // test_model_2f.omle's input
  // Shape should contain 2 (number of features).
  bool has2 = false;
  for (int i = 0; i < resp.inputs(0).shape_size(); ++i)
    if (resp.inputs(0).shape(i) == 2) has2 = true;
  EXPECT_TRUE(has2);
}

TEST_F(GrpcTest, ModelMetadataVersioned) {
  REQUIRE_SERVER();
  inference::ModelMetadataRequest req;
  req.set_name(MODEL_NAME);
  req.set_version("1");
  inference::ModelMetadataResponse resp;
  auto s = g_stub->ModelMetadata(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(resp.name(), MODEL_NAME);
}

TEST_F(GrpcTest, ModelMetadataUnknown) {
  REQUIRE_SERVER();
  inference::ModelMetadataRequest req;
  req.set_name("no_such");
  inference::ModelMetadataResponse resp;
  auto s = g_stub->ModelMetadata(&ctx_, req, &resp);
  EXPECT_EQ(s.error_code(), grpc::NOT_FOUND);
}

// ── gRPC: ModelInfer
// ──────────────────────────────────────────────────────────

static inference::ModelInferRequest make_fp64_req(
    const std::string& model_name, std::initializer_list<double> values,
    int n_rows, int n_cols) {
  inference::ModelInferRequest req;
  req.set_model_name(model_name);

  auto* inp = req.add_inputs();
  inp->set_name("X");
  inp->set_datatype("FP64");
  inp->add_shape(n_rows);
  inp->add_shape(n_cols);

  // Encode as raw little-endian doubles.
  std::string raw(values.size() * 8, '\0');
  size_t i = 0;
  for (double v : values) {
    std::memcpy(&raw[i * 8], &v, 8);
    ++i;
  }
  req.add_raw_input_contents(std::move(raw));

  return req;
}

TEST_F(GrpcTest, InferSingleRow) {
  REQUIRE_SERVER();
  auto req = make_fp64_req(MODEL_NAME, {1.0, 2.0}, 1, 2);
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(resp.model_name(), MODEL_NAME);
  ASSERT_GT(resp.outputs_size(), 0);
  EXPECT_EQ(resp.outputs(0).shape(0), 1);
}

TEST_F(GrpcTest, InferBatch) {
  REQUIRE_SERVER();
  auto req = make_fp64_req(MODEL_NAME, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}, 3, 2);
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(resp.outputs(0).shape(0), 3);
}

TEST_F(GrpcTest, InferWithRequestId) {
  REQUIRE_SERVER();
  auto req = make_fp64_req(MODEL_NAME, {0.0, 1.0}, 1, 2);
  req.set_id("grpc-cpp-1");
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(resp.id(), "grpc-cpp-1");
}

TEST_F(GrpcTest, InferTypedContents) {
  REQUIRE_SERVER();
  inference::ModelInferRequest req;
  req.set_model_name(MODEL_NAME);
  auto* inp = req.add_inputs();
  inp->set_name("X");
  inp->set_datatype("FP64");
  inp->add_shape(1);
  inp->add_shape(2);
  inp->mutable_contents()->add_fp64_contents(1.5);
  inp->mutable_contents()->add_fp64_contents(2.5);
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  ASSERT_TRUE(s.ok()) << s.error_message();
  ASSERT_GT(resp.outputs_size(), 0);
}

TEST_F(GrpcTest, InferUnknownModelReturnsNotFound) {
  REQUIRE_SERVER();
  auto req = make_fp64_req("no_such", {1.0, 2.0}, 1, 2);
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  EXPECT_EQ(s.error_code(), grpc::NOT_FOUND);
}

TEST_F(GrpcTest, InferUnknownDatatypeReturnsError) {
  REQUIRE_SERVER();
  inference::ModelInferRequest req;
  req.set_model_name(MODEL_NAME);
  auto* inp = req.add_inputs();
  inp->set_name("X");
  inp->set_datatype("FLOAT32");  // not a valid OIP dtype
  inp->add_shape(1);
  inp->add_shape(2);
  inference::ModelInferResponse resp;
  auto s = g_stub->ModelInfer(&ctx_, req, &resp);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.error_code() == grpc::INVALID_ARGUMENT ||
              s.error_code() == grpc::INTERNAL);
}

// ── gRPC: ModelStreamInfer
// ────────────────────────────────────────────────────

TEST_F(GrpcTest, StreamInferSingle) {
  REQUIRE_SERVER();
  auto stream = g_stub->ModelStreamInfer(&ctx_);
  ASSERT_TRUE(stream->Write(make_fp64_req(MODEL_NAME, {1.0, 2.0}, 1, 2)));
  stream->WritesDone();

  inference::ModelInferResponse resp;
  ASSERT_TRUE(stream->Read(&resp));
  EXPECT_EQ(resp.model_name(), MODEL_NAME);
  EXPECT_GT(resp.outputs_size(), 0);
  EXPECT_FALSE(stream->Read(&resp));  // stream ended
  EXPECT_TRUE(stream->Finish().ok());
}

TEST_F(GrpcTest, StreamInferMultiple) {
  REQUIRE_SERVER();
  auto stream = g_stub->ModelStreamInfer(&ctx_);
  for (double v : {1.0, 2.0, 3.0})
    stream->Write(make_fp64_req(MODEL_NAME, {v, v + 1.0}, 1, 2));
  stream->WritesDone();

  int count = 0;
  inference::ModelInferResponse resp;
  while (stream->Read(&resp)) {
    EXPECT_EQ(resp.model_name(), MODEL_NAME);
    EXPECT_GT(resp.outputs_size(), 0);
    ++count;
  }
  EXPECT_EQ(count, 3);
  EXPECT_TRUE(stream->Finish().ok());
}

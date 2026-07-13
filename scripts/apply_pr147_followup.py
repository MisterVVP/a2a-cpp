#!/usr/bin/env python3

from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file_path = Path(path)
    content = file_path.read_text(encoding="utf-8")
    if old not in content:
        raise RuntimeError(f"Expected block not found in {path}:\n{old}")
    file_path.write_text(content.replace(old, new, 1), encoding="utf-8")


def insert_before(path: str, anchor: str, addition: str) -> None:
    replace_once(path, anchor, addition + anchor)


# Restore the shared easy handle for unary requests. Only long-lived streams need
# independent handles.
replace_once(
    "src/http/http_client.cpp",
    """  CurlEasyHandle stream_handle(curl_easy_init());
  if (stream_handle == nullptr) {
    return core::Error::Internal(std::string(kCurlInitFailureMessage));
  }
  CURL* const handle = stream_handle.get();

""",
    """  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->handle == nullptr) {
    return core::Error::Internal(std::string(kCurlInitFailureMessage));
  }
  CURL* const handle = state_->handle;
  curl_easy_reset(handle);

""",
)

# Ensure the default libcurl path still emits and validates metadata when the
# response has no body and therefore never enters WriteStreamBody.
replace_once(
    "src/http/http_client.cpp",
    """  const CURLcode code = curl_easy_perform(handle);
  if (code != CURLE_OK) {
    if (stream_context.error.has_value()) {
      return stream_context.error.value();
    }
    if (is_cancelled()) {
      return core::Error::Network("HTTP stream was cancelled").WithTransport("http");
    }
    return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }
  long response_code = kHttpResponseCodeUnset;
""",
    """  const CURLcode code = curl_easy_perform(handle);
  if (code != CURLE_OK) {
    if (stream_context.error.has_value()) {
      return stream_context.error.value();
    }
    if (is_cancelled()) {
      return core::Error::Network("HTTP stream was cancelled").WithTransport("http");
    }
    return core::Error::Network(BuildCurlErrorMessage(kRequestFailureMessage, code, error_buffer.data()));
  }
  if (!stream_context.metadata_checked) {
    const auto metadata = ValidateStreamMetadata(&stream_context);
    if (!metadata.ok()) {
      return metadata.error();
    }
    stream_context.metadata_checked = true;
  }
  long response_code = kHttpResponseCodeUnset;
""",
)

replace_once(
    "src/http/http_client.cpp",
    """  (void)request;
  (void)on_chunk;
  (void)is_cancelled;
""",
    """  (void)request;
  (void)on_metadata;
  (void)on_chunk;
  (void)is_cancelled;
""",
)

# Injected/custom requesters are allowed to omit the early metadata callback.
# Validate their returned response before finishing an empty stream.
replace_once(
    "src/client/http_json_transport.cpp",
    """    if (!stream_response.ok()) {
      NotifyErrorAndStop(*state, observer, stream_response.error());
      return;
    }

    const auto finish = parser.Finish([&observer](const SseEvent& event) { return DispatchSseEvent(event, observer); });
""",
    """    if (!stream_response.ok()) {
      NotifyErrorAndStop(*state, observer, stream_response.error());
      return;
    }

    if (!metadata_validated) {
      const auto metadata = validate_metadata(stream_response.value());
      if (!metadata.ok()) {
        NotifyErrorAndStop(*state, observer, metadata.error());
        return;
      }
    }

    const auto finish = parser.Finish([&observer](const SseEvent& event) { return DispatchSseEvent(event, observer); });
""",
)

replace_once(
    "src/client/json_rpc_transport.cpp",
    """  if (!stream_response.ok()) {
    NotifyErrorAndStop(*state, observer, stream_response.error());
    return;
  }
  const auto finish = parser.Finish([&observer, &request_id, &response_metadata](const SseEvent& event) {
""",
    """  if (!stream_response.ok()) {
    NotifyErrorAndStop(*state, observer, stream_response.error());
    return;
  }
  if (!metadata_validated) {
    const auto metadata = validate_metadata(stream_response.value());
    if (!metadata.ok()) {
      NotifyErrorAndStop(*state, observer, metadata.error());
      return;
    }
  }
  const auto finish = parser.Finish([&observer, &request_id, &response_metadata](const SseEvent& event) {
""",
)

# Default HTTP client regression coverage for header-only/empty SSE responses.
replace_once(
    "tests/unit/http_client_test.cpp",
    """constexpr std::string_view kSseHeaders =
    "HTTP/1.1 200 OK\\r\\nA2A-Version: 1.0\\r\\nContent-Type: text/event-stream\\r\\nConnection: close\\r\\n\\r\\n";
""",
    """constexpr std::string_view kSseHeaders =
    "HTTP/1.1 200 OK\\r\\nA2A-Version: 1.0\\r\\nContent-Type: text/event-stream\\r\\nConnection: close\\r\\n\\r\\n";
constexpr std::string_view kEmptySseResponse =
    "HTTP/1.1 200 OK\\r\\nA2A-Version: 1.0\\r\\nContent-Type: text/event-stream\\r\\nContent-Length: "
    "0\\r\\nConnection: close\\r\\n\\r\\n";
""",
)

insert_before(
    "tests/unit/http_client_test.cpp",
    "TEST(SharedHttpClientTest, ConcurrentStreamsDoNotSerializeBehindSharedEasyHandle) {\n",
    """TEST(SharedHttpClientTest, EmptyStreamStillDeliversMetadata) {
  LoopbackHttpServer server{std::string(kEmptySseResponse)};
  a2a::http::Client client;
  a2a::http::Request request;
  request.method = "GET";
  request.url = BuildLoopbackUrl(server.port(), a2a::core::http::kHttpScheme, "/stream");
  request.timeout = std::chrono::milliseconds(kStreamTimeoutMs);
  request.http_version = std::string(kHttpVersion11);

  bool metadata_received = false;
  bool chunk_received = false;
  const auto response = client.StreamRequest(
      request,
      [&metadata_received](const a2a::http::Response& metadata) -> a2a::core::Result<void> {
        metadata_received = true;
        EXPECT_EQ(metadata.status_code, kHttpOk);
        return {};
      },
      [&chunk_received](std::string_view) -> a2a::core::Result<void> {
        chunk_received = true;
        return {};
      },
      [] { return false; });

  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_TRUE(metadata_received);
  EXPECT_FALSE(chunk_received);
  EXPECT_EQ(response.value().status_code, kHttpOk);
}

""",
)

# HTTP+JSON regression coverage for custom requesters that return an empty
# response without invoking on_metadata.
insert_before(
    "tests/integration/http_json_streaming_integration_test.cpp",
    "TEST(HttpJsonStreamingIntegrationTest, SubscribeTaskWithoutIdReturnsValidationError) {\n",
    """TEST(HttpJsonStreamingIntegrationTest, EmptyResponseWithoutMetadataCallbackStillCompletes) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
  EXPECT_TRUE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, ReturnedUnsupportedVersionIsValidatedForEmptyStream) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "2.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kUnsupportedVersion);
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, ReturnedMissingContentTypeIsValidatedForEmptyStream) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_FALSE(observer.completed);
}

""",
)

# JSON-RPC regression coverage for the same injected-requester fallback.
insert_before(
    "tests/unit/json_rpc_transport_test.cpp",
    "TEST(JsonRpcTransportUnitTest, StreamingMismatchedResponseIdReportsErrorOnce) {\n",
    """TEST(JsonRpcTransportUnitTest, EmptyResponseWithoutMetadataCallbackStillCompletes) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_TRUE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedUnsupportedVersion) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "2.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kUnsupportedVersion);
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedNonSuccessStatus) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpBadGateway,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(observer.errors.front().http_status().value_or(0), kHttpBadGateway);
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedMissingContentType) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kRemoteProtocol);
}

""",
)

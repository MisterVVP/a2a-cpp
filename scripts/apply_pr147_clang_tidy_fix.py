#!/usr/bin/env python3
from pathlib import Path

path = Path("src/client/http_json_transport.cpp")
text = path.read_text(encoding="utf-8")
text = text.replace("#include <thread>\n", "#include <thread>\n#include <utility>\n", 1)

insert_at = "core::Result<HttpRequest> BuildStreamingRequest("
helper = r'''struct HttpSseSession final {
  HttpStreamRequester requester;
  HttpRequest request;
  std::shared_ptr<StreamHandle::State> state;
  StreamObserver* observer = nullptr;
  std::string method;
  std::string endpoint;
  SseParser parser;
  HttpClientResponse response_metadata;
  bool metadata_validated = false;

  core::Result<void> ValidateMetadata(const HttpClientResponse& response) {
    response_metadata = response;
    const auto version_check = ValidateResponseVersion(response_metadata);
    if (!version_check.ok()) {
      return version_check.error();
    }
    if (response_metadata.status_code < kHttpOkMin || response_metadata.status_code > kHttpOkMax) {
      return BuildHttpError(method, endpoint, response_metadata);
    }
    if (!HasSseContentType(response_metadata.headers)) {
      return core::Error::RemoteProtocol("HTTP stream response must use text/event-stream")
          .WithTransport("http")
          .WithHttpStatus(response_metadata.status_code);
    }
    metadata_validated = true;
    return {};
  }

  core::Result<void> HandleChunk(std::string_view chunk) {
    if (state->cancel_requested.load()) {
      return {};
    }
    if (!metadata_validated) {
      return core::Error::RemoteProtocol("HTTP stream metadata must be validated before body chunks")
          .WithTransport("http");
    }
    return parser.Feed(chunk, [this](const SseEvent& event) { return DispatchSseEvent(event, *observer); });
  }

  void Run() {
    const auto response = requester(
        request, [this](const HttpClientResponse& metadata) { return ValidateMetadata(metadata); },
        [this](std::string_view chunk) { return HandleChunk(chunk); },
        [this] { return state->cancel_requested.load(); });
    if (state->cancel_requested.load()) {
      MarkInactive(*state);
      return;
    }
    if (!response.ok()) {
      NotifyErrorAndStop(*state, *observer, response.error());
      return;
    }
    if (!metadata_validated) {
      const auto metadata = ValidateMetadata(response.value());
      if (!metadata.ok()) {
        NotifyErrorAndStop(*state, *observer, metadata.error());
        return;
      }
    }
    const auto finish = parser.Finish([this](const SseEvent& event) { return DispatchSseEvent(event, *observer); });
    if (!finish.ok()) {
      NotifyErrorAndStop(*state, *observer, finish.error());
      return;
    }
    observer->OnCompleted();
    MarkInactive(*state);
  }
};

'''
if helper not in text:
    text = text.replace(insert_at, helper + insert_at, 1)

start = text.index("core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::StartSseStream(")
end = text.index("\n}\n\n}  // namespace a2a::client", start) + 3
replacement = r'''core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::StartSseStream(HttpOperation operation, std::string body,
                                                                              StreamObserver& observer,
                                                                              const CallOptions& options) const {
  if (stream_requester_ == nullptr) {
    return core::Error::Internal("HTTP stream requester is not configured");
  }
  auto request = BuildStreamingRequest(resolved_interface_, operation, std::move(body), options, default_timeout_);
  if (!request.ok()) {
    return request.error();
  }

  auto state = std::make_shared<StreamHandle::State>();
  auto session = std::make_shared<HttpSseSession>(HttpSseSession{.requester = stream_requester_,
                                                                 .request = std::move(request.value()),
                                                                 .state = state,
                                                                 .observer = &observer,
                                                                 .method = std::string(operation.method),
                                                                 .endpoint = std::string(operation.endpoint)});
  auto worker = StreamHandle::WorkerThread([session = std::move(session)] { session->Run(); });
  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}
'''
text = text[:start] + replacement + text[end:]
path.write_text(text, encoding="utf-8")

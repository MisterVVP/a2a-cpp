#include "tutorial.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/protojson.h"
#include "a2a/core/response_builders.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/http_adapter.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "model.h"

namespace job_tutorial {
namespace {
constexpr int kBacklog = 16;
constexpr std::chrono::milliseconds kPoll{50};
constexpr std::chrono::milliseconds kRequestTimeout{240000};
constexpr std::string_view kResume = "resume";
constexpr std::string_view kJob = "job_description";
constexpr std::string_view kAnalysisArtifact = "candidate-fit-analysis";
constexpr std::string_view kDraftArtifact = "application-draft";
volatile std::sig_atomic_t g_running = 1;
void Stop(int /*unused*/) { g_running = 0; }

class SocketTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit SocketTransport(int socket) : socket_(socket) {}
  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const auto count = recv(socket_, buffer, size, 0);
    if (count < 0) {
      return a2a::core::Error::Internal("socket read failed");
    }
    return static_cast<std::size_t>(count);
  }
  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    const auto count = send(socket_, buffer, size, 0);
    if (count < 0) {
      return a2a::core::Error::Internal("socket write failed");
    }
    return static_cast<std::size_t>(count);
  }

 private:
  int socket_;
};

google::protobuf::Value StringList(std::initializer_list<std::string_view> items) {
  google::protobuf::Value value;
  for (const auto item : items) {
    value.mutable_list_value()->add_values()->set_string_value(std::string(item));
  }
  return value;
}
const google::protobuf::Struct* Input(const lf::a2a::v1::SendMessageRequest& request) {
  if (!request.has_message()) {
    return nullptr;
  }
  for (const auto& part : request.message().parts()) {
    if (part.has_data() && part.data().has_struct_value()) {
      return &part.data().struct_value();
    }
  }
  return nullptr;
}
lf::a2a::v1::Task CompletedTask(std::string_view text, const google::protobuf::Value& data) {
  lf::a2a::v1::Task task;
  task.set_id("tutorial-task");
  task.set_context_id("tutorial-context");
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  a2a::core::ResponseBuilders::AddArtifactsWithPrimary(
      &task,
      a2a::core::ResponseBuilders::StructuredDataArtifact(
          data, {.artifact_id = std::string(kAnalysisArtifact), .name = "Structured analysis"}),
      {a2a::core::ResponseBuilders::TextArtifact(
          text, {.artifact_id = std::string(kDraftArtifact), .name = "Application draft"})});
  return task;
}
google::protobuf::Value Analyze(std::string_view resume, std::string_view job) {
  const auto has = [&](std::string_view word) {
    return resume.find(word) != std::string_view::npos && job.find(word) != std::string_view::npos;
  };
  google::protobuf::Value value;
  auto* fields = value.mutable_struct_value()->mutable_fields();
  (*fields)["match_summary"].set_string_value(
      "Good fit for backend delivery; one stated requirement needs clarification.");
  std::vector<std::string_view> strengths;
  if (has("C++")) {
    strengths.emplace_back("Modern C++ delivery");
  }
  if (has("distributed")) {
    strengths.emplace_back("Distributed systems experience");
  }
  if (has("Docker")) {
    strengths.emplace_back("Container delivery");
  }
  auto* strength_values = (*fields)["strengths"].mutable_list_value();
  for (auto item : strengths) {
    strength_values->add_values()->set_string_value(std::string(item));
  }
  (*fields)["gaps"] = StringList({"Kubernetes experience is not stated in the resume"});
  (*fields)["important_job_requirements"] = StringList({"Modern C++", "Distributed systems", "Docker", "Kubernetes"});
  (*fields)["resume_evidence"] =
      StringList({"Built C++ services for distributed telemetry", "Shipped services in Docker containers"});
  (*fields)["suggested_cv_emphasis"] =
      StringList({"C++20 service ownership", "Distributed telemetry outcomes", "Docker-based delivery"});
  return value;
}
std::string Draft(const google::protobuf::Value& analysis) {
  (void)analysis;
  return "Dear hiring team,\n\nI am applying for the backend engineering role. My resume describes building C++ "
         "services for distributed telemetry and shipping them in Docker containers. These experiences align with your "
         "modern C++ and distributed-systems needs. Kubernetes experience is not stated in my resume, so I would "
         "welcome a discussion about that requirement rather than imply experience I have not provided.\n";
}

std::string JobAnalysisPrompt(std::string_view resume, std::string_view job) {
  std::ostringstream prompt;
  prompt << "You are the Profile Analyst in a job-application workflow. Analyze only the evidence supplied below. "
            "Do not invent candidate experience. Return ONLY one JSON object, with no Markdown, using exactly these "
            "fields: match_summary (string), strengths (array of strings), gaps (array of strings), "
            "important_job_requirements (array of strings), resume_evidence (array of strings), "
            "suggested_cv_emphasis (array of strings).\n\nRESUME:\n"
         << resume << "\n\nJOB DESCRIPTION:\n"
         << job;
  return prompt.str();
}

std::string ApplicationDraftPrompt(std::string_view resume, std::string_view job, std::string_view analysis_json) {
  std::ostringstream prompt;
  prompt
      << "Write a concise job-application message. Use only facts present in the resume and the specialist analysis. "
         "Do not fabricate experience or hide identified gaps. Return only the application message, without "
         "analysis or Markdown headings.\n\nRESUME:\n"
      << resume << "\n\nJOB DESCRIPTION:\n"
      << job << "\n\nSPECIALIST ANALYSIS JSON:\n"
      << analysis_json;
  return prompt.str();
}

std::string_view JsonObject(std::string_view generated) {
  const auto begin = generated.find('{');
  const auto end = generated.rfind('}');
  if (begin == std::string_view::npos || end == std::string_view::npos || begin > end) {
    return {};
  }
  return generated.substr(begin, end - begin + 1);
}

bool HasAnalysisField(const google::protobuf::Struct& analysis, std::string_view name,
                      google::protobuf::Value::KindCase kind) {
  const auto found = analysis.fields().find(std::string(name));
  return found != analysis.fields().end() && found->second.kind_case() == kind;
}

a2a::core::Result<google::protobuf::Value> ParseJobAnalysis(std::string_view generated) {
  const auto json = JsonObject(generated);
  if (json.empty()) {
    return a2a::core::Error::Validation("profile analyst model did not return a JSON object");
  }
  google::protobuf::Struct analysis;
  auto status = a2a::core::JsonToMessage(json, &analysis);
  if (!status.ok()) {
    return a2a::core::Error::Validation("profile analyst model returned malformed JSON");
  }
  if (!HasAnalysisField(analysis, "match_summary", google::protobuf::Value::kStringValue) ||
      !HasAnalysisField(analysis, "strengths", google::protobuf::Value::kListValue) ||
      !HasAnalysisField(analysis, "gaps", google::protobuf::Value::kListValue) ||
      !HasAnalysisField(analysis, "important_job_requirements", google::protobuf::Value::kListValue) ||
      !HasAnalysisField(analysis, "resume_evidence", google::protobuf::Value::kListValue) ||
      !HasAnalysisField(analysis, "suggested_cv_emphasis", google::protobuf::Value::kListValue)) {
    return a2a::core::Error::Validation("profile analyst model response does not match the required analysis schema");
  }
  google::protobuf::Value value;
  *value.mutable_struct_value() = std::move(analysis);
  return value;
}

class Executor final : public a2a::server::AgentExecutor {
 public:
  Executor(bool coordinator, std::string specialist_url, std::unique_ptr<TextModel> model)
      : coordinator_(coordinator), specialist_url_(std::move(specialist_url)), model_(std::move(model)) {}
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& /*context*/) override {
    const auto* input = Input(request);
    if (input == nullptr) {
      return a2a::core::Error::Validation("structured resume and job_description are required");
    }
    const auto resume = input->fields().find(kResume);
    const auto job = input->fields().find(kJob);
    if (resume == input->fields().end() || job == input->fields().end()) {
      return a2a::core::Error::Validation("resume and job_description are required");
    }
    if (!coordinator_) {
      return Specialist(resume->second.string_value(), job->second.string_value());
    }
    auto delegated = Send(specialist_url_, request);
    if (!delegated.ok()) {
      return delegated.error();
    }
    if (!delegated.value().has_task() || delegated.value().task().artifacts().empty()) {
      return a2a::core::Error::Internal("profile analyst returned no analysis artifact");
    }
    const auto& data = delegated.value().task().artifacts(0).parts(0).data();
    auto analysis_json = a2a::core::MessageToJson(data);
    if (!analysis_json.ok()) {
      return analysis_json.error();
    }
    std::string draft = Draft(data);
    auto generated = model_->Generate(
        ApplicationDraftPrompt(resume->second.string_value(), job->second.string_value(), analysis_json.value()));
    if (!generated.ok()) {
      return generated.error();
    }
    if (!generated.value().empty()) {
      draft = std::move(generated.value());
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = CompletedTask(draft, data);
    return response;
  }
  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, a2a::server::RequestContext& /*context*/) override {
    return a2a::core::Error::Validation("streaming is not supported");
  }
  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& /*request*/,
                                               a2a::server::RequestContext& /*context*/) override {
    return a2a::core::Error::Validation("tasks are not persisted");
  }
  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& /*request*/,
                                                              a2a::server::RequestContext& /*context*/) override {
    return a2a::server::ListTasksResponse{};
  }
  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& /*request*/,
                                                  a2a::server::RequestContext& /*context*/) override {
    return a2a::core::Error::Validation("tasks are synchronous");
  }

 private:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> Specialist(std::string_view resume, std::string_view job) {
    google::protobuf::Value analysis = Analyze(resume, job);
    auto generated = model_->Generate(JobAnalysisPrompt(resume, job));
    if (!generated.ok()) {
      return generated.error();
    }
    if (!generated.value().empty()) {
      auto parsed = ParseJobAnalysis(generated.value());
      if (!parsed.ok()) {
        return parsed.error();
      }
      analysis = std::move(parsed.value());
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = CompletedTask("Profile analysis complete", analysis);
    return response;
  }
  bool coordinator_;
  std::string specialist_url_;
  std::unique_ptr<TextModel> model_;
};

int Listen(std::string_view host, int port) {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    return -1;
  }
  int reuse = 1;
  setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  const std::string host_text(host);
  if (inet_pton(AF_INET, host_text.c_str(), &address.sin_addr) != 1 ||
      bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(socket_fd, kBacklog) != 0) {
    close(socket_fd);
    return -1;
  }
  fcntl(socket_fd, F_SETFL, fcntl(socket_fd, F_GETFL, 0) | O_NONBLOCK);
  return socket_fd;
}
}  // namespace

a2a::core::Result<std::string> ReadFile(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) {
    return a2a::core::Error::Validation("cannot read input file: " + std::string(path));
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (contents.str().empty()) {
    return a2a::core::Error::Validation("input file is empty: " + std::string(path));
  }
  return contents.str();
}
lf::a2a::v1::SendMessageRequest JobRequest(std::string_view resume, std::string_view job) {
  lf::a2a::v1::SendMessageRequest request;
  auto* message = request.mutable_message();
  message->set_message_id("job-application-request");
  message->set_role(lf::a2a::v1::ROLE_USER);
  auto* fields = message->add_parts()->mutable_data()->mutable_struct_value()->mutable_fields();
  (*fields)[kResume].set_string_value(std::string(resume));
  (*fields)[kJob].set_string_value(std::string(job));
  return request;
}
a2a::core::Result<lf::a2a::v1::SendMessageResponse> Send(std::string_view base_url,
                                                         const lf::a2a::v1::SendMessageRequest& request) {
  auto discovery = a2a::client::DiscoveryClient::CreateDefault();
  auto card = discovery.Fetch(base_url);
  if (!card.ok()) {
    return card.error();
  }
  auto resolved =
      a2a::client::AgentCardResolver::SelectPreferredInterface(card.value(), a2a::client::PreferredTransport::kRest);
  if (!resolved.ok()) {
    return resolved.error();
  }
  a2a::client::A2AClient client(
      a2a::client::HttpJsonTransport::CreateDefault(std::move(resolved.value()), kRequestTimeout));
  return client.SendMessage(request);
}
namespace {
void RenderAnalysis(const google::protobuf::Struct& analysis, std::ostringstream* output) {
  const auto& fields = analysis.fields();
  const auto summary = fields.find("match_summary");
  if (summary != fields.end()) {
    *output << "Match summary: " << summary->second.string_value() << '\n';
  }
  for (const auto* const name : {"strengths", "gaps", "suggested_cv_emphasis"}) {
    const auto found = fields.find(name);
    if (found == fields.end()) {
      continue;
    }
    *output << name << ":\n";
    for (const auto& value : found->second.list_value().values()) {
      *output << "- " << value.string_value() << '\n';
    }
  }
}
}  // namespace

std::string Render(const lf::a2a::v1::SendMessageResponse& response) {
  if (!response.has_task()) {
    return {};
  }
  std::ostringstream output;
  for (const auto& artifact : response.task().artifacts()) {
    output << "## " << artifact.name() << '\n';
    for (const auto& part : artifact.parts()) {
      if (part.has_text()) {
        output << part.text() << '\n';
      } else if (part.has_data()) {
        RenderAnalysis(part.data().struct_value(), &output);
      }
    }
  }
  return output.str();
}
int RunAgentServer(std::string_view endpoint, std::string_view public_url, bool coordinator,
                   std::string_view specialist_url) {
  auto parsed = a2a::server::ParseHostPortEndpoint(endpoint);
  if (!parsed.ok()) {
    std::cerr << parsed.error().message() << '\n';
    return 1;
  }
  auto config = LoadModelConfig(coordinator ? "COORDINATOR" : "SPECIALIST");
  if (!config.ok()) {
    std::cerr << config.error().message() << '\n';
    return 1;
  }
  auto model = CreateModel(config.value());
  if (!model.ok()) {
    std::cerr << model.error().message() << '\n';
    return 1;
  }
  Executor executor(coordinator, std::string(specialist_url), std::move(model.value()));
  a2a::server::Dispatcher dispatcher(&executor);
  auto card =
      a2a::core::AgentCardBuilder::RestPreset(coordinator ? "Application Coordinator" : "Profile Analyst", public_url)
          .Build();
  auto* skill = card.add_skills();
  skill->set_id(coordinator ? "prepare_job_application" : "analyze_candidate_fit");
  skill->set_name(coordinator ? "Prepare Job Application" : "Analyze Candidate Fit");
  skill->set_description("Structured job application assistance");
  skill->add_tags("tutorial");
  a2a::server::RestServerTransport server(&dispatcher, std::move(card),
                                          {.rest_api_base_path = std::string(kRestPath),
                                           .require_version_header = true,
                                           .include_legacy_transport_fields = false});
  const int listener = Listen(parsed.value().host, parsed.value().port);
  if (listener < 0) {
    std::cerr << "unable to listen on " << endpoint << '\n';
    return 1;
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  std::cout << (coordinator ? "application_coordinator" : "profile_analyst") << " ready at " << public_url << '\n';
  while (g_running != 0) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      std::this_thread::sleep_for(kPoll);
      continue;
    }
    SocketTransport transport(client);
    a2a::server::HttpAdapter adapter;
    auto request = adapter.ReadRequest(transport, "localhost");
    if (request.ok()) {
      auto response = server.Handle(request.value());
      if (response.ok()) {
        (void)a2a::server::HttpAdapter::WriteResponse(transport, response.value());
      }
    }
    close(client);
  }
  close(listener);
  return 0;
}
}  // namespace job_tutorial

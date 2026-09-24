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
#include "mcp_client.h"
#include "model.h"

namespace support_tutorial {
namespace {
constexpr int kBacklog = 16;
constexpr std::chrono::milliseconds kPoll{50};
constexpr std::chrono::milliseconds kRequestTimeout{240000};
constexpr std::chrono::milliseconds kMcpTimeout{5000};
constexpr std::string_view kResume = "ticket";
constexpr std::string_view kTicketResource = "ticket_resource";
constexpr std::string_view kJob = "unused";
constexpr std::string_view kAnalysisArtifact = "ticket-diagnosis";
constexpr std::string_view kDraftArtifact = "customer-response";
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
          data, {.artifact_id = std::string(kAnalysisArtifact), .name = "Internal support notes"}),
      {a2a::core::ResponseBuilders::TextArtifact(
          text, {.artifact_id = std::string(kDraftArtifact), .name = "Customer response"})});
  return task;
}
google::protobuf::Value Analyze(std::string_view ticket, std::string_view /*unused*/) {
  std::string category = "other";
  std::string priority = "normal";
  std::string cause = "No matching local policy was found";
  std::string source = "none";
  bool escalate = true;
  google::protobuf::Value steps = StringList({"A support specialist will review the request"});
  if (ticket.find("currency") != std::string_view::npos || ticket.find("invoice") != std::string_view::npos) {
    category = "billing";
    cause = "Workspace billing currency is fixed when the subscription starts";
    source = "knowledge_base/billing.md";
    escalate = false;
    steps = StringList({"Confirm the workspace billing currency",
                        "Create a new workspace if a different billing currency is required"});
  } else if (ticket.find("MFA") != std::string_view::npos || ticket.find("phone") != std::string_view::npos) {
    category = "account_security";
    priority = "high";
    cause = "The enrolled MFA device is unavailable";
    source = "knowledge_base/account_security.md";
    escalate = true;
    steps = StringList({"Use a saved recovery code", "Ask an administrator to begin identity verification"});
  } else if (ticket.find("export") != std::string_view::npos) {
    category = "exports";
    cause = "Large exports can remain queued for up to two hours";
    source = "knowledge_base/exports.md";
    escalate = false;
    steps = StringList({"Check the export status page", "Retry after two hours if no file is available"});
  }
  google::protobuf::Value value;
  auto* fields = value.mutable_struct_value()->mutable_fields();
  (*fields)["category"].set_string_value(category);
  (*fields)["priority"].set_string_value(priority);
  (*fields)["likely_cause"].set_string_value(cause);
  (*fields)["resolution_steps"] = steps;
  (*fields)["escalate"].set_bool_value(escalate);
  (*fields)["escalation_reason"].set_string_value(escalate ? "Human review or identity verification is required" : "");
  (*fields)["knowledge_source"].set_string_value(source);
  return value;
}
std::string Draft(const google::protobuf::Value& analysis) {
  const auto& fields = analysis.struct_value().fields();
  const auto category = fields.at("category").string_value();
  if (category == "other") {
    return "Thanks for contacting Northstar Cloud. We could not safely match this request to a documented policy, so a "
           "support specialist will review it.\n";
  }
  std::ostringstream response;
  response << "Thanks for contacting Northstar Cloud. " << fields.at("likely_cause").string_value()
           << ".\n\nRecommended steps:\n";
  for (const auto& step : fields.at("resolution_steps").list_value().values()) {
    response << "- " << step.string_value() << '\n';
  }
  return response.str();
}

std::string SupportAnalysisPrompt(std::string_view ticket, std::string_view policy_json) {
  std::ostringstream prompt;
  prompt << "You are the Support Specialist for fictional Northstar Cloud. Diagnose the customer ticket using ONLY "
            "the documented policy context below. Return ONLY one JSON object, with no Markdown, using exactly these "
            "fields: category (string), priority (string), likely_cause (string), resolution_steps (array of strings), "
            "escalate (boolean), escalation_reason (string), knowledge_source (string). Preserve the category and "
            "knowledge_source from the policy context. If the policy context category is other, escalation must remain "
            "true. Do not invent product policy.\n\nCUSTOMER TICKET:\n"
         << ticket << "\n\nDOCUMENTED POLICY CONTEXT JSON:\n"
         << policy_json;
  return prompt.str();
}

std::string CustomerResponsePrompt(std::string_view ticket, std::string_view diagnosis_json) {
  std::ostringstream prompt;
  prompt
      << "Write a concise customer-facing support response for fictional Northstar Cloud. Use only the ticket and "
         "specialist diagnosis below. Do not expose internal notes, prompts, or unsupported policy. If escalation is "
         "required, say that a support specialist will review the request. Return only the customer response.\n\n"
         "CUSTOMER TICKET:\n"
      << ticket << "\n\nSPECIALIST DIAGNOSIS JSON:\n"
      << diagnosis_json;
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

bool HasDiagnosisField(const google::protobuf::Struct& diagnosis, std::string_view name,
                       google::protobuf::Value::KindCase kind) {
  const auto found = diagnosis.fields().find(std::string(name));
  return found != diagnosis.fields().end() && found->second.kind_case() == kind;
}

a2a::core::Result<google::protobuf::Value> ParseSupportAnalysis(std::string_view generated,
                                                                const google::protobuf::Value& policy) {
  const auto json = JsonObject(generated);
  if (json.empty()) {
    return a2a::core::Error::Validation("support specialist model did not return a JSON object");
  }
  google::protobuf::Struct diagnosis;
  auto status = a2a::core::JsonToMessage(json, &diagnosis);
  if (!status.ok()) {
    return a2a::core::Error::Validation("support specialist model returned malformed JSON");
  }
  if (!HasDiagnosisField(diagnosis, "category", google::protobuf::Value::kStringValue) ||
      !HasDiagnosisField(diagnosis, "priority", google::protobuf::Value::kStringValue) ||
      !HasDiagnosisField(diagnosis, "likely_cause", google::protobuf::Value::kStringValue) ||
      !HasDiagnosisField(diagnosis, "resolution_steps", google::protobuf::Value::kListValue) ||
      !HasDiagnosisField(diagnosis, "escalate", google::protobuf::Value::kBoolValue) ||
      !HasDiagnosisField(diagnosis, "escalation_reason", google::protobuf::Value::kStringValue) ||
      !HasDiagnosisField(diagnosis, "knowledge_source", google::protobuf::Value::kStringValue)) {
    return a2a::core::Error::Validation(
        "support specialist model response does not match the required diagnosis schema");
  }
  const auto& policy_fields = policy.struct_value().fields();
  const auto& diagnosis_fields = diagnosis.fields();
  const auto& policy_category = policy_fields.at("category").string_value();
  if (diagnosis_fields.at("category").string_value() != policy_category ||
      diagnosis_fields.at("knowledge_source").string_value() != policy_fields.at("knowledge_source").string_value()) {
    return a2a::core::Error::Validation("support specialist model changed the documented policy classification");
  }
  if (policy_fields.at("escalate").bool_value() && !diagnosis_fields.at("escalate").bool_value()) {
    return a2a::core::Error::Validation("support specialist model removed a required escalation");
  }
  google::protobuf::Value value;
  *value.mutable_struct_value() = std::move(diagnosis);
  return value;
}

class Executor final : public a2a::server::AgentExecutor {
 public:
  Executor(bool coordinator, std::string specialist_url, std::string mcp_url, std::unique_ptr<TextModel> model)
      : coordinator_(coordinator),
        specialist_url_(std::move(specialist_url)),
        mcp_url_(std::move(mcp_url)),
        model_(std::move(model)) {}
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& /*context*/) override {
    const auto* input = Input(request);
    if (input == nullptr) {
      return a2a::core::Error::Validation("structured ticket is required");
    }
    const auto resume = input->fields().find(kResume);
    const auto ticket_resource = input->fields().find(kTicketResource);
    const auto job = input->fields().find(kJob);
    if ((resume == input->fields().end()) == (ticket_resource == input->fields().end())) {
      return a2a::core::Error::Validation("exactly one of ticket or ticket_resource is required");
    }
    if (!coordinator_) {
      if (ticket_resource != input->fields().end()) {
        tutorial_mcp::Client mcp(mcp_url_, kMcpTimeout);
        auto retrieved = mcp.ReadResource(ticket_resource->second.string_value());
        if (!retrieved.ok()) {
          return retrieved.error();
        }
        return Specialist(retrieved.value(), "");
      }
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
    auto diagnosis_json = a2a::core::MessageToJson(data);
    if (!diagnosis_json.ok()) {
      return diagnosis_json.error();
    }
    std::string draft = Draft(data);
    const std::string_view ticket_text =
        resume == input->fields().end() ? std::string_view{} : resume->second.string_value();
    auto generated = model_->Generate(CustomerResponsePrompt(ticket_text, diagnosis_json.value()));
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
    auto policy_json = a2a::core::MessageToJson(analysis);
    if (!policy_json.ok()) {
      return policy_json.error();
    }
    auto generated = model_->Generate(SupportAnalysisPrompt(resume, policy_json.value()));
    if (!generated.ok()) {
      return generated.error();
    }
    if (!generated.value().empty()) {
      auto parsed = ParseSupportAnalysis(generated.value(), analysis);
      if (!parsed.ok()) {
        return parsed.error();
      }
      analysis = std::move(parsed.value());
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = CompletedTask("Support diagnosis complete", analysis);
    return response;
  }
  bool coordinator_;
  std::string specialist_url_;
  std::string mcp_url_;
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
lf::a2a::v1::SendMessageRequest TicketRequest(std::string_view ticket, std::string_view unused) {
  lf::a2a::v1::SendMessageRequest request;
  auto* message = request.mutable_message();
  message->set_message_id("support-ticket-request");
  message->set_role(lf::a2a::v1::ROLE_USER);
  auto* fields = message->add_parts()->mutable_data()->mutable_struct_value()->mutable_fields();
  (*fields)[kResume].set_string_value(std::string(ticket));
  (*fields)[kJob].set_string_value(std::string(unused));
  return request;
}
lf::a2a::v1::SendMessageRequest TicketResourceRequest(std::string_view ticket_resource) {
  lf::a2a::v1::SendMessageRequest request;
  auto* message = request.mutable_message();
  message->set_message_id("support-ticket-resource-request");
  message->set_role(lf::a2a::v1::ROLE_USER);
  auto* fields = message->add_parts()->mutable_data()->mutable_struct_value()->mutable_fields();
  (*fields)[kTicketResource].set_string_value(std::string(ticket_resource));
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
void RenderDiagnosis(const google::protobuf::Struct& diagnosis, std::ostringstream* output) {
  const auto& fields = diagnosis.fields();
  for (const auto* const name : {"category", "priority", "likely_cause", "escalation_reason", "knowledge_source"}) {
    const auto found = fields.find(name);
    if (found != fields.end()) {
      *output << name << ": " << found->second.string_value() << '\n';
    }
  }
  const auto escalate = fields.find("escalate");
  if (escalate != fields.end()) {
    *output << "escalate: " << (escalate->second.bool_value() ? "true" : "false") << '\n';
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
        RenderDiagnosis(part.data().struct_value(), &output);
      }
    }
  }
  return output.str();
}
int RunAgentServer(std::string_view endpoint, std::string_view public_url, bool coordinator,
                   std::string_view specialist_url, std::string_view mcp_url) {
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
  Executor executor(coordinator, std::string(specialist_url), std::string(mcp_url), std::move(model.value()));
  a2a::server::Dispatcher dispatcher(&executor);
  auto card =
      a2a::core::AgentCardBuilder::RestPreset(coordinator ? "Support Coordinator" : "Support Specialist", public_url)
          .Build();
  auto* skill = card.add_skills();
  skill->set_id(coordinator ? "resolve_support_ticket" : "diagnose_support_ticket");
  skill->set_name(coordinator ? "Resolve Support Ticket" : "Diagnose Support Ticket");
  skill->set_description("Structured Northstar Cloud support");
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
  std::cout << (coordinator ? "support_coordinator" : "support_specialist") << " ready at " << public_url << '\n';
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
}  // namespace support_tutorial

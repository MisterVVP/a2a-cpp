// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "example_support.h"
#include "support/security/fixtures.h"
#include "support/security/observed_task_store.h"

namespace {
using namespace a2a::server;
using namespace a2a::tests::security;
constexpr std::string_view kAlice = "alice";
constexpr std::string_view kBob = "bob";
constexpr std::string_view kTaskId = "security-owned-task";
constexpr std::string_view kOtherTaskId = "security-other-task";
constexpr std::string_view kThirdTaskId = "security-third-task";
constexpr std::string_view kMissingTaskId = "security-missing-task";
constexpr std::string_view kAuthorizationHeader = "Authorization";
constexpr std::string_view kProxyAuthorizationHeader = "Proxy-Authorization";
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kLegacyStreamingPrefix = "task-stream-";
constexpr std::string_view kBearerKey = "bearer_token";
constexpr std::string_view kMessageId = "security-message";
constexpr std::string_view kText = "security text";
constexpr std::size_t kOwnedTaskCount = 2;
constexpr auto kPollTimeout = std::chrono::milliseconds(1);

class Policy final : public SecurityPolicy {
 public:
  a2a::core::Result<std::string> Authenticate(const RequestContext& context) const override {
    ++authentications;
    const auto token = context.auth_metadata.find(std::string(kBearerKey));
    if (token == context.auth_metadata.end() || token->second != kAliceToken) {
      return a2a::core::protocol_errors::Unauthenticated();
    }
    return std::string(kAlice);
  }
  bool AuthorizeOperation(const DispatchRequest& /*request*/, const RequestContext& /*context*/) const override {
    return operations_allowed;
  }
  bool CanAccessTask(const lf::a2a::v1::Task& task, const RequestContext& context) const override {
    return resources_allowed && task.context_id() == context.principal;
  }
  std::optional<ListTasksRequest> ScopeTaskList(const ListTasksRequest& query,
                                                const RequestContext& context) const override {
    if (!scope_lists) {
      return query;
    }
    if (!query.context_id.empty() && query.context_id != context.principal) {
      return std::nullopt;
    }
    auto scoped = query;
    scoped.context_id = context.principal;
    scoped.history_length = 0;
    scoped.include_artifacts = false;
    return scoped;
  }
  bool scope_lists = false;
  mutable std::atomic_size_t authentications = 0;
  bool operations_allowed = true;
  bool resources_allowed = true;
};

lf::a2a::v1::Task Task(std::string_view id, std::string_view owner) {
  lf::a2a::v1::Task task;
  task.set_id(std::string(id));
  task.set_context_id(std::string(owner));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  return task;
}
lf::a2a::v1::SendMessageRequest Message(std::string_view id = kMessageId) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(id));
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_context_id(std::string(kAlice));
  request.mutable_message()->add_parts()->set_text(std::string(kText));
  return request;
}
std::vector<DispatchRequest> ResourceOperations(std::string_view id) {
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(id));
  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(std::string(id));
  auto send = Message();
  send.mutable_message()->set_task_id(std::string(id));
  lf::a2a::v1::TaskPushNotificationConfig push;
  push.set_task_id(std::string(id));
  lf::a2a::v1::GetTaskPushNotificationConfigRequest push_get;
  push_get.set_task_id(std::string(id));
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest push_list;
  push_list.set_task_id(std::string(id));
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest push_delete;
  push_delete.set_task_id(std::string(id));
  return {{.operation = DispatcherOperation::kGetTask, .payload = get},
          {.operation = DispatcherOperation::kSubscribeTask, .payload = get},
          {.operation = DispatcherOperation::kCancelTask, .payload = cancel},
          {.operation = DispatcherOperation::kSendMessage, .payload = send},
          {.operation = DispatcherOperation::kSendStreamingMessage, .payload = send},
          {.operation = DispatcherOperation::kCreateTaskPushNotificationConfig, .payload = push},
          {.operation = DispatcherOperation::kGetTaskPushNotificationConfig, .payload = push_get},
          {.operation = DispatcherOperation::kListTaskPushNotificationConfigs, .payload = push_list},
          {.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig, .payload = push_delete}};
}

class SdkSecurityTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(store.CreateOrUpdate(Task(kTaskId, kAlice)).ok());
    ASSERT_TRUE(store.CreateOrUpdate(Task(kOtherTaskId, kBob)).ok());
    ASSERT_TRUE(store.CreateOrUpdate(Task(kThirdTaskId, kAlice)).ok());
    dispatcher.SetSecurityPolicy(policy, &store);
    context.auth_metadata[std::string(kBearerKey)] = kAliceToken;
  }
  void CheckHiddenResource(const DispatchRequest& denied, const DispatchRequest& missing) {
    auto hidden = dispatcher.Dispatch(denied, context);
    auto absent = dispatcher.Dispatch(missing, context);
    ASSERT_FALSE(hidden.ok());
    ASSERT_FALSE(absent.ok());
    EXPECT_EQ(hidden.error().protocol_code(), a2a::core::protocol_codes::kTaskNotFound);
    EXPECT_EQ(hidden.error().protocol_code(), absent.error().protocol_code());
    EXPECT_EQ(hidden.error().http_status(), absent.error().http_status());
  }
  ListTasksResponse ListPage(const ListTasksRequest& query) {
    auto result = dispatcher.Dispatch({.operation = DispatcherOperation::kListTasks, .payload = query}, context);
    EXPECT_TRUE(result.ok());
    if (!result.ok()) {
      return {};
    }
    return std::get<ListTasksResponse>(result.value().payload());
  }
  static void CheckOwnedPage(const ListTasksResponse& page) {
    ASSERT_EQ(page.tasks.size(), 1U);
    EXPECT_EQ(page.total_size, kOwnedTaskCount);
    EXPECT_EQ(page.tasks.front().context_id(), kAlice);
  }
  InMemoryTaskStore store;
  a2a::examples::ExampleExecutor executor{
      {.task_store = &store, .push_store = nullptr, .push_delivery = nullptr, .task_id_generator = {}}};
  Dispatcher dispatcher{&executor};
  std::shared_ptr<Policy> policy = std::make_shared<Policy>();
  RequestContext context;
};

TEST(ServerSecurityMetadataTest, ProxyCredentialsCannotAuthenticateTheAgentRequest) {
  const auto proxy = std::string(kBearerPrefix).append(kBobToken);
  const auto caller = std::string(kBearerPrefix).append(kAliceToken);
  auto metadata = ExtractAuthMetadata({{std::string(kProxyAuthorizationHeader), proxy}});
  EXPECT_FALSE(metadata.contains(std::string(kBearerKey)));
  metadata = ExtractAuthMetadata(
      {{std::string(kProxyAuthorizationHeader), proxy}, {std::string(kAuthorizationHeader), caller}});
  EXPECT_EQ(metadata.at(std::string(kBearerKey)), kAliceToken);
}

TEST_F(SdkSecurityTest, EveryOperationAuthenticatesBeforeDispatchAndClearsStaleIdentity) {
  auto requests = ResourceOperations(kTaskId);
  requests.push_back({.operation = DispatcherOperation::kListTasks, .payload = ListTasksRequest{}});
  requests.push_back(
      {.operation = DispatcherOperation::kGetExtendedAgentCard, .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}});
  context.auth_metadata.clear();
  context.principal = kAlice;
  for (const auto& request : requests) {
    auto result = dispatcher.Dispatch(request, context);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().http_status(), a2a::core::http::kStatusUnauthorized);
    EXPECT_TRUE(context.principal.empty());
  }
  EXPECT_EQ(policy->authentications.load(), requests.size());
}
TEST_F(SdkSecurityTest, UnauthorizedResourcesMatchMissingResourcesOnEveryApplicableOperation) {
  const auto denied = ResourceOperations(kOtherTaskId);
  const auto missing = ResourceOperations(kMissingTaskId);
  for (std::size_t index = 0; index < denied.size(); ++index) {
    CheckHiddenResource(denied[index], missing[index]);
  }
}
TEST_F(SdkSecurityTest, ListFiltersBeforeCountsProjectionsAndPagination) {
  ListTasksRequest query;
  query.page_size = 1;
  query.history_length = 0;
  const auto first = ListPage(query);
  CheckOwnedPage(first);
  ASSERT_FALSE(first.next_page_token.empty());
  query.page_token = first.next_page_token;
  const auto last = ListPage(query);
  CheckOwnedPage(last);
  ASSERT_FALSE(first.tasks.empty());
  ASSERT_FALSE(last.tasks.empty());
  EXPECT_NE(first.tasks.front().id(), last.tasks.front().id());
  EXPECT_TRUE(last.next_page_token.empty());
}
TEST_F(SdkSecurityTest, NewStreamCreationUsesSdkIdentityWithoutCollidingWithAnotherCaller) {
  const auto foreign_id = std::string(kLegacyStreamingPrefix).append(kMessageId);
  ASSERT_TRUE(store.CreateOrUpdate(Task(foreign_id, kBob)).ok());
  auto result =
      dispatcher.Dispatch({.operation = DispatcherOperation::kSendStreamingMessage, .payload = Message()}, context);
  ASSERT_TRUE(result.ok());
  auto& stream = std::get<std::unique_ptr<ServerStreamSession>>(result.value().payload());
  auto first = stream->Next();
  ASSERT_TRUE(first.ok());
  const auto& event = first.value();
  ASSERT_TRUE(event.has_value());
  const auto value = event.value_or(lf::a2a::v1::StreamResponse{});
  EXPECT_EQ(value.status_update().context_id(), kAlice);
  EXPECT_NE(value.status_update().task_id(), foreign_id);
}

TEST_F(SdkSecurityTest, OperationDenialsAndRevokedStreamsAreEnforced) {
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskId));
  auto subscribed = dispatcher.Dispatch({.operation = DispatcherOperation::kSubscribeTask, .payload = get}, context);
  ASSERT_TRUE(subscribed.ok());
  auto& stream = std::get<std::unique_ptr<ServerStreamSession>>(subscribed.value().payload());
  EXPECT_TRUE(stream->SupportsTimedNext());
  EXPECT_TRUE(stream->Next().ok());
  policy->resources_allowed = false;
  auto revoked = stream->NextFor(kPollTimeout);
  ASSERT_FALSE(revoked.ok());
  EXPECT_EQ(revoked.error().protocol_code(), a2a::core::protocol_codes::kTaskNotFound);
  policy->resources_allowed = true;
  policy->operations_allowed = false;
  auto denied = dispatcher.Dispatch({.operation = DispatcherOperation::kGetTask, .payload = get}, context);
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().http_status(), a2a::core::http::kStatusForbidden);
}

constexpr std::size_t kLargeTaskCount = (kMaxListTasksPageSize * 2U) + 1U;
constexpr std::size_t kExpectedLargePages = 3;
constexpr std::string_view kListTaskPrefix = "bounded-list-task-";

void PopulateListTasks(ObservedTaskStore& store) {
  for (std::size_t index = 0; index < kLargeTaskCount; ++index) {
    std::string id(kListTaskPrefix);
    id.append(std::to_string(index));
    auto task = Task(id, kAlice);
    *task.add_history() = Message().message();
    task.add_artifacts()->set_artifact_id(id);
    ASSERT_TRUE(store.CreateOrUpdate(task).ok());
  }
  ASSERT_TRUE(store.CreateOrUpdate(Task(kOtherTaskId, kBob)).ok());
}

void CheckSelectedPage(const ListTasksResponse& page, std::size_t page_size) {
  EXPECT_EQ(page.total_size, kLargeTaskCount);
  ASSERT_EQ(page.tasks.size(), page_size);
  EXPECT_FALSE(page.tasks.front().history().empty());
  EXPECT_FALSE(page.tasks.front().artifacts().empty());
}

void CheckScopedQuery(const ListTasksRequest& query) {
  EXPECT_EQ(query.page_size, kMaxListTasksPageSize);
  EXPECT_EQ(query.context_id, kAlice);
  EXPECT_EQ(query.history_length, 0);
  EXPECT_FALSE(query.include_artifacts);
}

void CheckScopedStoreQueries(const std::vector<ListTasksRequest>& queries) {
  ASSERT_EQ(queries.size(), kExpectedLargePages);
  for (const auto& query : queries) {
    CheckScopedQuery(query);
  }
}

void CheckEmptyForeignScope(DispatchSecurity& security, const RequestContext& context, ListTasksRequest request) {
  request.context_id = kBob;
  EXPECT_FALSE(security.ListTasks(request, context).ok());
  request.page_token.clear();
  auto foreign = security.ListTasks(request, context);
  ASSERT_TRUE(foreign.ok());
  EXPECT_EQ(std::get<ListTasksResponse>(foreign.value().payload()).total_size, 0);
}

TEST(SdkScopedListingTest, StoreQueriesAreBoundedAndHeavyProjectionsAreLoadedForSelectedPage) {
  ObservedTaskStore store;
  PopulateListTasks(store);
  auto policy = std::make_shared<Policy>();
  policy->scope_lists = true;
  DispatchSecurity security(policy, &store);
  RequestContext context;
  context.principal = kAlice;
  ListTasksRequest request;
  request.page_size = 1;
  request.page_token = std::to_string(kMaxListTasksPageSize);
  request.include_artifacts = true;
  auto result = security.ListTasks(request, context);
  ASSERT_TRUE(result.ok());
  const auto& page = std::get<ListTasksResponse>(result.value().payload());
  CheckSelectedPage(page, request.page_size);
  CheckScopedStoreQueries(store.list_queries);
  CheckEmptyForeignScope(security, context, request);
}

}  // namespace

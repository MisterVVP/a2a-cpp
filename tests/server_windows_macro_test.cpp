// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <type_traits>

// Exercise the object-like Win32 alias on every platform, including Linux CI.
#define SendMessage A2aWindowsSendMessage
#include "a2a/server/agent_executor.h"
#ifdef SendMessage
#error AgentExecutor must clear the SendMessage macro before declaring protocol methods
#endif
static_assert(std::is_member_function_pointer_v<decltype(&a2a::server::AgentExecutor::SendMessage)>);

// Dependencies may have been included earlier, before a Windows header.
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/transport_mux.h"
#define SendMessage A2aWindowsSendMessage
#include "a2a/server/server_bindings.h"
#ifdef SendMessage
#error ServerBindings must not leak the SendMessage macro to application executors
#endif
static_assert(std::is_member_function_pointer_v<decltype(&a2a::server::AgentExecutor::SendMessage)>);

#if defined(A2A_HAS_ASYNC_SERVER) && A2A_HAS_ASYNC_SERVER
#define SendMessage A2aWindowsSendMessage
#include "a2a/server/engine/coroutine_agent_executor.h"
#ifdef SendMessage
#error CoroutineAgentExecutor must preserve the protocol method name
#endif
static_assert(std::is_member_function_pointer_v<decltype(&a2a::server::engine::CoroutineAgentExecutor::SendMessage)>);

#define SendMessage A2aWindowsSendMessage
#include "a2a/server/engine/blocking_executor_adapter.h"
#ifdef SendMessage
#error BlockingExecutorAdapter must handle a macro introduced after its base headers
#endif
static_assert(std::is_member_function_pointer_v<decltype(&a2a::server::engine::BlockingExecutorAdapter::SendMessage)>);
#endif

int main() {}

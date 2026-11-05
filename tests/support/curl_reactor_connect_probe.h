// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

// Temporary, process-local experiment. Never linked into ordinary SDK builds.
#include "curl_reactor_stage_trace.h"

#ifdef _WIN32
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace a2a::http::stage_trace {

inline constexpr const char* kCurlModule = "libcurl.dll";
inline constexpr const char* kKernelModule = "kernel32.dll";
inline constexpr const char* kSocketModule = "ws2_32.dll";
inline constexpr const char* kYieldFunction = "SleepEx";
inline constexpr const char* kSocketErrorFunction = "getsockopt";
inline constexpr const char* kPeerFunction = "getpeername";
inline constexpr const char* kLocalFunction = "getsockname";
inline constexpr const char* kSendFunction = "send";
inline constexpr const char* kSkipYieldVariable = "A2A_STAGE_SKIP_CONNECT_YIELD";
inline constexpr const char* kFastCheckVariable = "A2A_STAGE_FAST_CONNECT_CHECK";
inline constexpr const char* kProbeFailure = "cannot install process-local curl connection probe";
inline constexpr std::size_t kFlagBufferSize = 2U;

using YieldFunction = DWORD(WINAPI*)(DWORD, BOOL);
using SocketErrorFunction = int(WSAAPI*)(SOCKET, int, int, char*, int*);
using SocketNameFunction = int(WSAAPI*)(SOCKET, sockaddr*, int*);
using SocketSendFunction = int(WSAAPI*)(SOCKET, const char*, int, int);
inline YieldFunction original_yield = nullptr;
inline SocketErrorFunction original_socket_error = nullptr;
inline SocketNameFunction original_peer = nullptr;
inline SocketNameFunction original_local = nullptr;
inline SocketSendFunction original_send = nullptr;
inline bool skip_connect_yield = false;
inline bool fast_connect_check = false;
inline std::atomic_size_t confirmed_failure_checks{0U};

inline bool WritableWithoutException(SOCKET socket) {
  fd_set writable{};
  fd_set failed{};
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(socket, &writable);
  FD_SET(socket, &failed);
  timeval immediate{};
  return select(0, nullptr, &writable, &failed, &immediate) > 0 && FD_ISSET(socket, &writable) != 0 &&
         FD_ISSET(socket, &failed) == 0;
}

inline DWORD WINAPI ProbeYield(DWORD milliseconds, BOOL alertable) {
  const Span span(Kind::kYieldBegin, Kind::kYieldEnd, nullptr, milliseconds);
  if (skip_connect_yield && milliseconds == 0U && alertable == FALSE) {
    return 0U;
  }
  return original_yield(milliseconds, alertable);
}

inline int WSAAPI ProbePeer(SOCKET socket, sockaddr* address, int* length) {
  const Span span(Kind::kPeerBegin, Kind::kPeerEnd, nullptr, static_cast<std::int64_t>(socket));
  return original_peer(socket, address, length);
}

inline int WSAAPI ProbeLocal(SOCKET socket, sockaddr* address, int* length) {
  const Span span(Kind::kLocalBegin, Kind::kLocalEnd, nullptr, static_cast<std::int64_t>(socket));
  return original_local(socket, address, length);
}

inline int WSAAPI ProbeSend(SOCKET socket, const char* bytes, int length, int flags) {
  const Span span(Kind::kSendBegin, Kind::kSendEnd, nullptr, static_cast<std::int64_t>(socket));
  return original_send(socket, bytes, length, flags);
}

inline int WSAAPI ProbeSocketError(SOCKET socket, int level, int option, char* value, int* length) {
  if (level == SOL_SOCKET && option == SO_ERROR) {
    const Span span(Kind::kSocketErrorBegin, Kind::kSocketErrorEnd, nullptr, static_cast<std::int64_t>(socket));
    const bool valid_output = value != nullptr && length != nullptr && *length >= static_cast<int>(sizeof(int));
    if (fast_connect_check && valid_output && WritableWithoutException(socket)) {
      constexpr int kNoSocketError = 0;
      std::memcpy(value, &kNoSocketError, sizeof(kNoSocketError));
      *length = sizeof(kNoSocketError);
      Emit(Kind::kFastConnected, nullptr, static_cast<std::int64_t>(socket));
      return 0;
    }
    const int result = original_socket_error(socket, level, option, value, length);
    int socket_error = 0;
    if (result == 0 && valid_output) {
      std::memcpy(&socket_error, value, sizeof(socket_error));
    }
    if (result != 0 || socket_error != 0) {
      confirmed_failure_checks.fetch_add(1U, std::memory_order_relaxed);
    }
    return result;
  }
  return original_socket_error(socket, level, option, value, length);
}

inline ULONG_PTR* FindImportSlot(HMODULE module, FARPROC target) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    throw std::runtime_error(kProbeFailure);
  }
  const auto* image = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (image->Signature != IMAGE_NT_SIGNATURE) {
    throw std::runtime_error(kProbeFailure);
  }
  const auto& directory = image->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
  const auto descriptors = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
  for (std::size_t index = 0U; index < descriptors && imports[index].Name != 0U; ++index) {
    const auto offset = imports[index].FirstThunk;
    if (offset >= image->OptionalHeader.SizeOfImage) {
      throw std::runtime_error(kProbeFailure);
    }
    auto* entries = reinterpret_cast<IMAGE_THUNK_DATA*>(base + offset);
    const auto capacity = (image->OptionalHeader.SizeOfImage - offset) / sizeof(IMAGE_THUNK_DATA);
    for (std::size_t entry = 0U; entry < capacity && entries[entry].u1.Function != 0U; ++entry) {
      if (entries[entry].u1.Function == std::bit_cast<ULONG_PTR>(target)) {
        return &entries[entry].u1.Function;
      }
    }
  }
  throw std::runtime_error(kProbeFailure);
}

class ImportHook final {
 public:
  ImportHook() = default;
  ImportHook(const ImportHook&) = delete;
  ImportHook& operator=(const ImportHook&) = delete;
  ~ImportHook() {
    if (slot_ != nullptr) {
      (void)Store(original_);
    }
  }

  void Install(HMODULE module, FARPROC original, FARPROC replacement) {
    slot_ = FindImportSlot(module, original);
    original_ = std::bit_cast<ULONG_PTR>(original);
    if (!Store(std::bit_cast<ULONG_PTR>(replacement))) {
      throw std::runtime_error(kProbeFailure);
    }
  }

 private:
  bool Store(ULONG_PTR value) noexcept {
    DWORD protection = 0U;
    if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection) == 0) {
      return false;
    }
    *slot_ = value;
    DWORD ignored = 0U;
    return VirtualProtect(slot_, sizeof(*slot_), protection, &ignored) != 0;
  }

  ULONG_PTR* slot_ = nullptr;
  ULONG_PTR original_ = 0U;
};

template <typename Function>
void InstallObservation(ImportHook& hook, HMODULE module, const char* name, Function replacement, Function& original) {
  const auto address = GetProcAddress(GetModuleHandleA(kSocketModule), name);
  if (address == nullptr) {
    throw std::runtime_error(kProbeFailure);
  }
  original = std::bit_cast<Function>(address);
  hook.Install(module, address, std::bit_cast<FARPROC>(replacement));
}

class ConnectProbe final {
 public:
  ConnectProbe() {
    HMODULE module = GetModuleHandleA(kCurlModule);
    if (module == nullptr) {
      // The private MinGW portability harness links libcurl statically.
      module = GetModuleHandleA(nullptr);
    }
    const auto yield = GetProcAddress(GetModuleHandleA(kKernelModule), kYieldFunction);
    const auto socket_error = GetProcAddress(GetModuleHandleA(kSocketModule), kSocketErrorFunction);
    if (module == nullptr || yield == nullptr || socket_error == nullptr) {
      throw std::runtime_error(kProbeFailure);
    }
    original_yield = std::bit_cast<YieldFunction>(yield);
    original_socket_error = std::bit_cast<SocketErrorFunction>(socket_error);
    std::array<char, kFlagBufferSize> flag{};
    skip_connect_yield =
        GetEnvironmentVariableA(kSkipYieldVariable, flag.data(), static_cast<DWORD>(flag.size())) == 1U &&
        flag.front() == '1';
    flag.fill('\0');
    fast_connect_check =
        GetEnvironmentVariableA(kFastCheckVariable, flag.data(), static_cast<DWORD>(flag.size())) == 1U &&
        flag.front() == '1';
    yield_.Install(module, yield, std::bit_cast<FARPROC>(&ProbeYield));
    socket_error_.Install(module, socket_error, std::bit_cast<FARPROC>(&ProbeSocketError));
    InstallObservation(peer_, module, kPeerFunction, &ProbePeer, original_peer);
    InstallObservation(local_, module, kLocalFunction, &ProbeLocal, original_local);
    InstallObservation(send_, module, kSendFunction, &ProbeSend, original_send);
  }

 private:
  ImportHook yield_;
  ImportHook socket_error_;
  ImportHook peer_;
  ImportHook local_;
  ImportHook send_;
};

}  // namespace a2a::http::stage_trace
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

// Temporary, process-local experiment. Never linked into ordinary SDK builds.
#include "curl_reactor_stage_trace.h"

#ifdef _WIN32
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace a2a::http::stage_trace {

inline constexpr const char* kCurlModule = "libcurl.dll";
inline constexpr const char* kKernelModule = "kernel32.dll";
inline constexpr const char* kSocketModule = "ws2_32.dll";
inline constexpr const char* kYieldFunction = "SleepEx";
inline constexpr const char* kSocketErrorFunction = "getsockopt";
inline constexpr const char* kSkipYieldVariable = "A2A_STAGE_SKIP_CONNECT_YIELD";
inline constexpr const char* kProbeFailure = "cannot install process-local curl connection probe";
inline constexpr std::size_t kFlagBufferSize = 2U;

using YieldFunction = DWORD(WINAPI*)(DWORD, BOOL);
using SocketErrorFunction = int(WSAAPI*)(SOCKET, int, int, char*, int*);
inline YieldFunction original_yield = nullptr;
inline SocketErrorFunction original_socket_error = nullptr;
inline bool skip_connect_yield = false;

inline DWORD WINAPI ProbeYield(DWORD milliseconds, BOOL alertable) {
  const Span span(Kind::kYieldBegin, Kind::kYieldEnd, nullptr, milliseconds);
  if (skip_connect_yield && milliseconds == 0U && alertable == FALSE) {
    return 0U;
  }
  return original_yield(milliseconds, alertable);
}

inline int WSAAPI ProbeSocketError(SOCKET socket, int level, int option, char* value, int* length) {
  if (level == SOL_SOCKET && option == SO_ERROR) {
    const Span span(Kind::kSocketErrorBegin, Kind::kSocketErrorEnd, nullptr, static_cast<std::int64_t>(socket));
    return original_socket_error(socket, level, option, value, length);
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
    yield_.Install(module, yield, std::bit_cast<FARPROC>(&ProbeYield));
    socket_error_.Install(module, socket_error, std::bit_cast<FARPROC>(&ProbeSocketError));
  }

 private:
  ImportHook yield_;
  ImportHook socket_error_;
};

}  // namespace a2a::http::stage_trace
#endif

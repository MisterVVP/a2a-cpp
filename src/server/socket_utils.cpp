// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/socket_utils.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace a2a::server {

void CloseSocketCrossPlatform(int fd) noexcept {
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

}  // namespace a2a::server

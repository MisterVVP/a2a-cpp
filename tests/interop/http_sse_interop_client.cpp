// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json
# Optional Boost.Asio dependency

The a2a-cpp SDK remains licensed under Apache-2.0.

When built with `A2A_ENABLE_ASIO=ON`, the optional `a2a::asio` target uses
Boost.Asio and its Boost header dependencies, licensed under the Boost Software
License 1.0. These third-party components retain their original copyrights and
license. They are supplied by the dependency manager rather than vendored here.

The Boost license text is reproduced in [LICENSES/BSL-1.0.txt](LICENSES/BSL-1.0.txt)
and installed with Asio-enabled SDK builds. Preserve the applicable upstream
copyright and license notices when redistributing Boost source or headers.
This document describes the new optional dependency; it is not an exhaustive
inventory of the SDK's existing third-party dependencies.

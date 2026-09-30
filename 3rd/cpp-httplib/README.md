# cpp-httplib

Header-only HTTP/HTTPS client + server library by Yuji Hirose.

## Source

- Upstream repo: https://github.com/yhirose/cpp-httplib
- Version: **v0.58.0**
- License: MIT (Copyright (c) 2026 Yuji Hirose)
- Header: `httplib.h` (single file, ~23k lines)

## Integration

DevDesk uses cpp-httplib for `src/platform/http_server.{h,cpp}` (see spec §7.2).
The file is included as:

```cpp
#define CPPHTTPLIB_OPENSSL_SUPPORT  // optional - enables HTTPS via OpenSSL
#include <httplib.h>
```

## CMake

Header-only - no extra build step. Just add include dir:

```cmake
target_include_directories(<target> PRIVATE 3rd/cpp-httplib)
```

## OpenSSL

`CPPHTTPLIB_OPENSSL_SUPPORT` enables HTTPS client/server via `OpenSSL::SSL` /
`OpenSSL::Crypto`. DevDesk's `Dependencies.cmake` already calls
`find_package(OpenSSL REQUIRED)`. On macOS, Homebrew ships
`/opt/homebrew/lib/{libssl.dylib,libcrypto.dylib}` (auto-discovered).

## Threading notes

`httplib::Server` runs its listener loop on the calling thread. DevDesk wraps
it in `std::thread` (`Impl::worker`) and joins on `stop()`. The library uses
internal `std::thread` pool for request handlers by default.

## Update procedure

```bash
# 1. Bump version (also bump CPPHTTPLIB_VERSION note above)
curl -L -o 3rd/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/vX.Y.Z/httplib.h

# 2. Rebuild
cmake --build build

# 3. Run tests
build/tests/devdesk_tests
```
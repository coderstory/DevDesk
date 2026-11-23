#include "web_dev_entry.h"

#ifndef NDEBUG
#include <cstdlib>
#include <cstring>
#endif

namespace module::terminal::web {

#ifndef NDEBUG

namespace {
const char* envOrNull(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}
} // namespace

DevEntryConfig readDevEntryConfig() {
    DevEntryConfig cfg;
    if (const char* autoStart = envOrNull("DEVDESK_WEB_AUTOSTART")) {
        cfg.autostart = std::strcmp(autoStart, "1") == 0;
    }
    if (const char* port = envOrNull("DEVDESK_WEB_PORT")) {
        const int parsed = std::atoi(port);
        if (parsed > 0 && parsed <= 65535) cfg.port = parsed;
    }
    cfg.testHooks = envOrNull("DEVDESK_WEB_TEST_HOOKS") != nullptr;
    return cfg;
}

bool devEntryAvailable() { return true; }

#else

DevEntryConfig readDevEntryConfig() { return DevEntryConfig{}; }

bool devEntryAvailable() { return false; }

#endif

} // namespace module::terminal::web

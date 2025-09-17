// tests/test_tui_log.cpp
#include <gtest/gtest.h>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#ifndef BITCHAT_SOURCE_DIR
#  error "BITCHAT_SOURCE_DIR must be defined by CMake to the project source root"
#endif

struct Check {
    const char* label;
    const char* rel_path;
    const char* pattern;   // regex for the message text
    bool        allow_prev_line_macro = true;
};

static std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static bool has_system_macro_near(const std::string& content,
                                  const std::regex& msg_re,
                                  bool allow_prev) {
    std::istringstream iss(content);
    std::string line, prev;
    while (std::getline(iss, line)) {
        if (std::regex_search(line, msg_re)) {
            auto pos = line.find("LOG_SYSTEM(");
            if (pos != std::string::npos) return true;
            if (allow_prev && prev.find("LOG_SYSTEM(") != std::string::npos) return true;
            return false;
        }
        prev = line;
    }
    return false; // message not found => fail (we depend on it)
}

// Check if logs used by TUI are at SYSTEM level in the source files
// (to ensure they are always visible in TUI mode).
TEST(TuiLogs, SystemLevelLogs) {
    const std::vector<Check> checks = {
        {"RECV", "src/app/chat_service.cpp", R"(\[RECV\])"},
        {"CTRL HELLO in", "src/app/chat_service.cpp", R"(\[CTRL\]\s+HELLO in:)"},
        {"CTRL HELLO out","src/app/chat_service.cpp", R"(\[CTRL\]\s+HELLO out:)"},
        {"KEX complete", "src/app/chat_service.cpp", R"(\[KEX\]\s+complete)"},
        {"KEX failures", "src/app/chat_service.cpp", R"(\[KEX\].*(install failed|no/invalid PSK))"},
        // This is the one that used to be LOG_WARN, test enforces SYSTEM:
        {"SEC AEAD decrypt failed", "src/app/chat_service.cpp", R"(\[SEC\].*AEAD decrypt failed)"},
        {"Notifications ready", "src/transport/bluez_transport.cpp", R"(Notifications enabled; ready)"},
        {"StartDiscovery OK","src/transport/bluez_transport.cpp", R"(StartDiscovery OK)"},
        {"StopDiscovery OK", "src/transport/bluez_transport.cpp", R"(StopDiscovery OK)"},
        {"Device connected", "src/transport/bluez_helper.inc", R"(Device connected:)"},
        {"Connected property true","src/transport/bluez_helper.inc", R"(Connected property became true)"},
        {"Disconnected", "src/transport/bluez_helper.inc", R"(Disconnected\s*\()"},
        {"InterfacesRemoved", "src/transport/bluez_helper.inc", R"(InterfacesRemoved -> cleared device)"},
        {"LE adv registered", "src/transport/bluez_helper.inc", R"(LE advertisement registered successfully)"},
        {"found peer addr", "src/transport/bluez_helper.inc", R"(found .* addr=)"}
    };

    for (const auto& c : checks) {
        const std::string path = std::string(BITCHAT_SOURCE_DIR) + "/" + c.rel_path;
        const std::string content = read_file(path);
        ASSERT_FALSE(content.empty()) << "Missing file: " << path;
        const bool ok = has_system_macro_near(content, std::regex(c.pattern), c.allow_prev_line_macro);
        EXPECT_TRUE(ok) << "Log for [" << c.label << "] is not LOG_SYSTEM near message in " << path
                        << " (pattern: " << c.pattern << ")";
    }
}

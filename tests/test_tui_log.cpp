#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

#ifndef BITCHAT_SOURCE_DIR
#error "BITCHAT_SOURCE_DIR must be defined by CMake to the project source root"
#endif

struct Check
{
    const char              *label;
    const char              *rel_path;
    std::vector<std::string> needles;  // all substrings must appear in the SAME line
    bool                     allow_prev_line_macro = true;  // sometimes macro is on prev line
};

static std::string read_file(const std::string &path)
{
    std::ifstream      ifs(path);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static bool line_has_all(const std::string &line, const std::vector<std::string> &needles)
{
    for (const auto &n : needles)
    {
        if (line.find(n) == std::string::npos)
            return false;
    }
    return true;
}

static bool has_system_macro_near(const std::string              &content,
                                  const std::vector<std::string> &needles,
                                  bool                            allow_prev)
{
    std::istringstream iss(content);
    std::string        line, prev;
    while (std::getline(iss, line))
    {
        if (line_has_all(line, needles))
        {
            const bool on_same = line.find("LOG_SYSTEM(") != std::string::npos;
            const bool on_prev = allow_prev && prev.find("LOG_SYSTEM(") != std::string::npos;
            return on_same || on_prev;
        }
        prev = line;
    }
    // message not found => fail
    return false;
}

TEST(TuiLogs, AllSystemLevel)
{
    const std::vector<Check> checks = {
        // clang-format off
        {"RECV", "src/app/chat_service.cpp", {"[RECV]"}},
        {"CTRL HELLO in", "src/app/chat_service.cpp", {"[CTRL]", "HELLO in:"}},
        {"CTRL HELLO out", "src/app/chat_service.cpp", {"[CTRL]", "HELLO out:"}},
        {"KEX complete", "src/app/chat_service.cpp", {"[KEX]", "complete"}},
        {"KEX install failed", "src/app/chat_service.cpp", {"[KEX]", "install failed"}},
        {"KEX invalid PSK", "src/app/chat_service.cpp", {"[KEX]", "no/invalid PSK"}},
        // Enforce SYSTEM (used to be WARN)
        {"SEC AEAD decrypt failed", "src/app/chat_service.cpp", {"[SEC]", "AEAD decrypt failed"}},
        // role = central
        {"Notifications ready", "src/transport/bluez_transport_central.cpp", {"Notifications enabled; ready"}},
        {"StartDiscovery OK", "src/transport/bluez_transport_central.cpp", {"StartDiscovery OK"}},
        {"StopDiscovery OK", "src/transport/bluez_transport_central.cpp", {"StopDiscovery OK"}},

        {"Device connected", "src/transport/bluez_helper.inc", {"Device connected:"}},
        {"Connected property true", "src/transport/bluez_helper.inc", {"Connected property became true"}},
        {"Disconnected", "src/transport/bluez_helper.inc", {"Disconnected", "("}},
        {"InterfacesRemoved", "src/transport/bluez_helper.inc", {"InterfacesRemoved -> cleared device"}},
        {"LE adv registered", "src/transport/bluez_helper.inc", {"LE advertisement registered successfully"}},
        {"found peer addr", "src/transport/bluez_helper.inc", {"found ", " addr="}}
        // clang-format on
    };

    for (const auto &c : checks)
    {
        const std::string path    = std::string(BITCHAT_SOURCE_DIR) + "/" + c.rel_path;
        const std::string content = read_file(path);
        ASSERT_FALSE(content.empty()) << "Missing file: " << path;
        const bool ok = has_system_macro_near(content, c.needles, c.allow_prev_line_macro);
        if (!ok)
        {
            std::ostringstream err;
            err << "Log for [" << c.label << "] is not LOG_SYSTEM near message in " << path
                << " (needles: ";
            for (size_t i = 0; i < c.needles.size(); ++i)
            {
                if (i)
                    err << ", ";
                err << '"' << c.needles[i] << '"';
            }
            err << ")";
            ADD_FAILURE() << err.str();
        }
    }
}

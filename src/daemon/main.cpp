#include <atomic>
#include <string>
#include "ctl/ipc.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

static std::atomic<bool> g_tail_enabled{false};

static void on_line(const std::string &line)
{
    if (line == "QUIT")
    {
        LOG_INFO("Received QUIT command, exiting...");
        return;
    }
    if (line == "TAIL on")
    {
        g_tail_enabled.store(true);
        LOG_INFO("TAIL Enabled");
        return;
    }
    if (line == "TAIL off")
    {
        g_tail_enabled.store(false);
        LOG_INFO("TAIL Disabled");
        return;
    }
    LOG_INFO("CMD: %s", line.c_str());
}

int main()
{
    bitchat::set_log_level(bitchat::Level::Info);
    std::string sock = ipc::expand_user(std::string(constants::kCtlSock));
    bool        ok   = ipc::start_server(sock, on_line);
    return ok ? 0 : 1;
}

#include <string>
#include "ctl/ipc.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

static void on_line(const std::string &line)
{
    LOG_INFO("CMD: %s", line.c_str());
}

int main()
{
    bitchat::set_log_level(bitchat::Level::Info);
    std::string sock = ipc::expand_user(std::string(constants::kCtlSock));
    bool        ok   = ipc::start_server(sock, on_line);
    return ok ? 0 : 1;
}

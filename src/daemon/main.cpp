#include <atomic>
#include <string>
#include "ctl/ipc.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "transport/loopback_transport.hpp"

static transport::LoopbackTransport g_tx;
static aead::PskAead                g_aead;
static app::ChatService             g_chat{g_tx, g_aead, /*mtu=*/100};

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
    if (line.rfind("SEND ", 0) == 0)  // sending msg
    {
        g_chat.send_text(std::string_view{line}.substr(5));
        return;
    }
}

int main()
{
    bitchat::set_log_level(bitchat::Level::Info);
    // start chat service
    g_chat.start();

    // start daemon IPC server
    std::string sock = ipc::expand_user(std::string(constants::kCtlSock));
    bool        ok   = ipc::start_server(sock, on_line);
    return ok ? 0 : 1;
}

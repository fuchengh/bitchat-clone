#include <atomic>
#include <cassert>
#include <string>

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "ctl/ipc.hpp"
#include "transport/loopback_transport.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

static transport::LoopbackTransport   g_tx;
static std::unique_ptr<aead::PskAead> g_aead;
static app::ChatService              *g_chat;

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
        assert(g_chat != nullptr);
        g_chat->send_text(std::string_view{line}.substr(5));
        return;
    }
}

int main()
{
    if (auto s = aead::SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK_HEX"))
    {
        LOG_DEBUG("Using libsodium");
        g_aead = std::make_unique<aead::SodiumPskAead>(*s);
    }
    else
    {
        LOG_DEBUG("Using noop-pskaead");
        g_aead = std::make_unique<aead::NoopPskAead>();
    }

    bitchat::set_log_level(bitchat::Level::Info);
    // start chat service
    app::ChatService chat_service{g_tx, *g_aead, /*mtu*/ 100};
    g_chat = &chat_service;

    // start daemon IPC server
    std::string sock = ipc::expand_user(std::string(constants::kCtlSock));
    bool        ok   = ipc::start_server(sock, on_line);
    return ok ? 0 : 1;
}

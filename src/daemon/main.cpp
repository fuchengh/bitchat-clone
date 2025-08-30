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

static void on_line(const std::string &line)
{
    if (line == "QUIT")
    {
        LOG_INFO("Received QUIT command, exiting...");
        return;
    }
    if (line == "TAIL on")
    {
        if (g_chat)
            g_chat->set_tail(true);
        LOG_INFO("TAIL Enabled");
        return;
    }
    if (line == "TAIL off")
    {
        if (g_chat)
            g_chat->set_tail(false);
        LOG_INFO("TAIL Disabled");
        return;
    }
    if (line.rfind("SEND ", 0) == 0)  // sending msg
    {
        auto msg = line.substr(5);
        LOG_INFO("CMD: SEND %s", msg.c_str());
        if (g_chat)
            g_chat->send_text(std::string_view{msg});
        else
            LOG_WARN("ChatService not ready");
        return;
    }
}

int main()
{
    // AEAD: prefer Sodium key if env is set, else Noop
    if (auto s = aead::SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK_HEX"))
    {
        LOG_INFO("Using SodiumPskAead (key from BITCHAT_PSK_HEX)");
        g_aead = std::make_unique<aead::SodiumPskAead>(*s);
    }
    else
    {
        LOG_WARN("Using NoopPskAead (encryption disabled)");
        g_aead = std::make_unique<aead::NoopPskAead>();
    }

    // chat service
    app::ChatService chat(g_tx, *g_aead, /*mtu_payload*/ 100);
    if (!chat.start())
    {
        LOG_ERROR("ChatService start failed");
        return 1;
    }
    g_chat = &chat;

    // IPC server
    const std::string sock = ipc::expand_user(std::string(constants::kCtlSock));
    if (!ipc::start_server(sock, &on_line))
    {
        LOG_ERROR("start_server failed");
        return 1;
    }
    return 0;
}

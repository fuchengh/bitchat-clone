#include <cassert>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "ctl/ipc.hpp"
#include "transport/bluez_transport.hpp"
#include "transport/loopback_transport.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

static std::unique_ptr<aead::PskAead> g_aead;
static app::ChatService              *g_chat;
static transport::ITransport         *g_tx = nullptr;

// ---------------- helpers ----------------
static bool is_valid_mac(const std::string &mac)
{
    if (mac.size() != 17)
        return false;
    for (size_t i = 0; i < mac.size(); ++i)
    {
        if ((i % 3) == 2)
        {
            if (mac[i] != ':')
                return false;
        }
        else
        {
            unsigned char c = static_cast<unsigned char>(mac[i]);
            if (!std::isxdigit(c))
                return false;
        }
    }
    return true;
}

static std::string normalize_mac(std::string mac)
{
    std::transform(mac.begin(), mac.end(), mac.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return mac;
}

std::unique_ptr<transport::ITransport> make_transport_from_env()
{
    const char *t = std::getenv("BITCHAT_TRANSPORT");
    if (t && std::strcmp(t, "bluez") == 0)
    {
        transport::BluezConfig cfg;
        // role: default Peripheral
        if (const char *r = std::getenv("BITCHAT_ROLE"))
        {
            // case-insensitive
            std::string rs = r;
            std::transform(rs.begin(), rs.end(), rs.begin(), ::tolower);
            if (rs == "central")
                cfg.role = transport::Role::Central;
            else
                cfg.role = transport::Role::Peripheral;
        }
        if (const char *a = std::getenv("BITCHAT_ADAPTER"))
            cfg.adapter = a;
        if (const char *p = std::getenv("BITCHAT_PEER"))
            cfg.peer_addr = normalize_mac(std::string{p});
        return std::make_unique<transport::BluezTransport>(std::move(cfg));
    }
    // default - loopback
    return std::make_unique<transport::LoopbackTransport>();
}

#define DEBUG_ON 0

static void on_line(const std::string &line)
{
#if DEBUG_ON
    LOG_DEBUG("IPC line: %s", line.c_str());
#endif
    using transport::BluezTransport;
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
        // trim head/tail spaces
        auto l = msg.find_first_not_of(" \t\r");
        auto r = msg.find_last_not_of(" \t\r");
        if (l == std::string::npos)
        {
            LOG_WARN("CMD: SEND ignored (empty payload)");
            return;
        }
        msg = msg.substr(l, r - l + 1);

        LOG_INFO("CMD: SEND %s", msg.c_str());
        if (g_chat)
            g_chat->send_text(std::string_view{msg});
        else
            LOG_WARN("ChatService not ready");
        return;
    }
    if (line == "PEERS")
    {
        auto *bt = dynamic_cast<BluezTransport *>(g_tx);
        if (!bt)
        {
            LOG_SYSTEM("[PEERS] not supported on this transport");
            return;
        }
        bt->request_candidate_refresh();  // non-blocking poke

        auto peers = bt->list_peers();
        // optionally filter noisy RSSI==0 entries (toggle via env)
        bool keep_zero = false;
        if (const char *e = std::getenv("BITCHAT_KEEP_ZERO_RSSI"))
            keep_zero = (*e == '1');
        if (!keep_zero)
        {
            peers.erase(std::remove_if(peers.begin(), peers.end(),
                                       [](const transport::PeerInfo &p) { return p.rssi == 0; }),
                        peers.end());
        }

        if (peers.empty())
        {
            LOG_SYSTEM("[PEERS] no peers found");
            return;
        }
        for (const auto &p : peers)
        {
            LOG_SYSTEM("[PEER] %s rssi=%d", p.addr.c_str(), (int)p.rssi);
        }
        return;
    }
    if (line.rfind("CONNECT", 0) == 0)
    {
        auto *bt = dynamic_cast<BluezTransport *>(g_tx);
        if (!bt)
        {
            LOG_SYSTEM("[CONNECT] not supported on this transport");
            return;
        }
        std::string mac = line.size() >= 8 ? line.substr(8) : std::string{};
        while (!mac.empty() && std::isspace(static_cast<unsigned char>(mac.front())))
            mac.erase(mac.begin());
        // empty => clear target (disconnect/stop trying)
        if (!mac.empty())
        {
            mac = normalize_mac(mac);
            if (!is_valid_mac(mac))
            {
                LOG_WARN("[CONNECT] invalid MAC address: %s", mac.c_str());
                return;
            }
        }

        if (bt->handover_to(mac))
            LOG_SYSTEM("[CONNECT] switching to %s", mac.c_str());
        else
            LOG_WARN("[CONNECT] failed to switch to %s", mac.c_str());
        return;
    }
    if (line == "DISCONNECT")
    {
        auto *bt = dynamic_cast<BluezTransport *>(g_tx);
        if (!bt)
        {
            LOG_SYSTEM("[DISCONNECT] not supported on this transport");
            return;
        }
        // reuse handover_to("") to disconnect and clear target
        if (bt->handover_to(std::string{}))
            LOG_SYSTEM("[DISCONNECT] link dropped and target cleared");
        else
            LOG_WARN("[DISCONNECT] failed");
        return;
    }
}

int main()
{
    // log level from env var
    const char *log_level = std::getenv("BITCHAT_LOG_LEVEL");
    if (log_level)
    {
        bitchat::set_log_level_by_name(log_level);
    }

    const char *env_transport = std::getenv("BITCHAT_TRANSPORT");
    const char *env_role      = std::getenv("BITCHAT_ROLE");
    const char *env_adapter   = std::getenv("BITCHAT_ADAPTER");
    const char *env_peer      = std::getenv("BITCHAT_PEER");
    LOG_SYSTEM("Config: transport=%s role=%s adapter=%s peer=%s",
               env_transport ? env_transport : "loopback", env_role ? env_role : "peripheral",
               env_adapter ? env_adapter : "hci0", env_peer ? env_peer : "(none)");

    // Bind transport from env var
    auto tx = make_transport_from_env();
    g_tx    = tx.get();

    // AEAD: prefer Sodium key if env is set, else Noop
    if (auto s = aead::SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK"))
    {
        LOG_INFO("Using SodiumPskAead (key from BITCHAT_PSK)");
        g_aead = std::make_unique<aead::SodiumPskAead>(*s);
    }
    else
    {
        LOG_WARN("Using NoopPskAead (encryption disabled)");
        g_aead = std::make_unique<aead::NoopPskAead>();
    }

    // chat service: allow env override for mtu payload
    // so ChatService/transport are in sync
    size_t mtu_payload = 100;
    if (const char *e = std::getenv("BITCHAT_MTU_PAYLOAD"))
    {
        char         *p = nullptr;
        unsigned long v = std::strtoul(e, &p, 10);
        if (p && *p == '\0' && v >= 20 && v <= 244)
        {
            mtu_payload = static_cast<size_t>(v);
            LOG_INFO("Using mtu_payload=%zu (from BITCHAT_MTU_PAYLOAD)", mtu_payload);
        }
        else
        {
            LOG_WARN("Ignoring invalid BITCHAT_MTU_PAYLOAD='%s' (expect 20..244)", e);
        }
    }

    app::ChatService chat(*tx, *g_aead, mtu_payload);
    if (!chat.start())
    {
        LOG_ERROR("ChatService start failed");
        return 1;
    }
    g_chat = &chat;

    // IPC server
    std::string sock = ipc::expand_user(constants::ctl_sock_path());
    if (const char *e = std::getenv("BITCHAT_CTL_SOCK"))
    {
        if (*e)
            sock = ipc::expand_user(e);
    }

    if (!ipc::start_server(sock, &on_line))
    {
        LOG_ERROR("start_server failed");
        return 1;
    }
    return 0;
}

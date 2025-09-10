#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "proto/ctrl.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

namespace app
{

constexpr std::uint8_t AAD[] = {'B', 'C', '1'};

inline bool get_na32(std::array<uint8_t, 32> &out)
{
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    size_t got = 0;
    while (got < out.size())
    {
        ssize_t n = ::read(fd, out.data() + got, out.size() - got);
        if (n <= 0)
        {
            ::close(fd);
            return false;
        }
        got += size_t(n);
    }
    ::close(fd);
    return true;
}

ChatService::ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload)
    : tx_(t), aead_(aead), mtu_payload_(mtu_payload)
{
}

bool ChatService::start()
{
    // in case there are any background hello-threads
    stop();

    // Default to loopback
    // If BITCHAT_TRANSPORT=bluez we pass BLE UUIDs and role
    auto env_or = [](const char *key, const char *defv) -> std::string {
        const char *v = std::getenv(key);
        return v ? std::string(v) : std::string(defv);
    };

    const std::string which = env_or("BITCHAT_TRANSPORT", "loopback");

    transport::Settings s{};
    s.mtu_payload = mtu_payload_;

    if (which == "bluez")
    {
        // Role for BlueZ: "central" or "peripheral" (default peripheral)
        s.role = env_or("BITCHAT_ROLE", "peripheral");
        // Fixed UUIDs per project spec.
        s.svc_uuid = constants::SVC_UUID;
        s.tx_uuid  = constants::TX_UUID;  // Notify
        s.rx_uuid  = constants::RX_UUID;  // Write w/ response
    }
    else
    {
        // Loopback for testing/dev
        s.role = "loopback";
    }

    bool ok = tx_.start(s, [this](const transport::Frame &f) { this->on_rx(f); });
    if (!ok)
        return false;

    // Prepare user ID and send hello packet
    if (const char *u = std::getenv("BITCHAT_USER_ID"))
        local_user_ = u;
    else
        local_user_.clear();
    if (local_user_.size() > 64)
        local_user_.resize(64);
    local_caps_ = std::getenv("BITCHAT_PSK") ? ctrl::CAP_AEAD_PSK_SUPPORTED : 0;

    hello_stop_.store(false);
    hello_sent_ = false;
    get_na32(na32_);

    hello_thr_ = std::thread([this] {
        bool last_ready = false;
        while (!hello_stop_.load())
        {
            const bool ready = tx_.link_ready();

            // new link edge: refresh Na32 & clear sent flag
            if (ready && !last_ready)
            {
                get_na32(na32_);
                hello_sent_ = false;
            }

            if (ready && !hello_sent_)
            {
                auto bytes = ctrl::encode_hello(local_user_, local_caps_, na32_.data());
                if (tx_.send(bytes))
                {
                    hello_sent_ = true;
                    LOG_INFO("[CTRL] HELLO out: user='%s' caps=0x%08x na32=%02x%02x...",
                             local_user_.c_str(), local_caps_, (unsigned)na32_[0],
                             (unsigned)na32_[1]);
                }
            }
            if (!ready)
                hello_sent_ = false;  // keep false while link is down
            last_ready = ready;

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    return true;
}

void ChatService::stop()
{
    hello_stop_.store(true);
    if (hello_thr_.joinable())
        hello_thr_.join();
    tx_.stop();
}

bool ChatService::send_text(std::string_view msg)
{
    // 1) Encrypt
    std::vector<std::uint8_t> sealed_text;
    if (!aead_.seal(msg, AAD, sizeof(AAD), sealed_text))
    {
        LOG_ERROR("send_text: AEAD seal failed");
        return false;
    }

    // 2) Make chunk
    const std::size_t frame_mtu   = mtu_payload_;
    const std::size_t payload_mtu = (frame_mtu > frag::HDR_SIZE) ? (frame_mtu - frag::HDR_SIZE)
                                                                 : 0;
    auto              chunks = frag::make_chunks(next_id_.fetch_add(1), sealed_text, payload_mtu);
    if (!sealed_text.empty() && chunks.empty())
    {
        LOG_ERROR("send_text: make_chunks failed");
        return false;
    }

    // 3) Send
    for (const auto &ch : chunks)
    {
        auto frame = frag::serialize(ch);
        if (frame.empty() && ch.hdr.len != 0)
        {
            LOG_ERROR("send_text: serialize failed");
            return false;
        }
        if (!tx_.send(frame))
        {
            LOG_ERROR("send_text: transport.send failed");
            return false;
        }
    }
    return true;
}

void ChatService::on_rx(const transport::Frame &f)
{
    // CTRL_HELLO
    if (f.size() >= 2 && f[0] == ctrl::MSG_CTRL_HELLO && f[1] == ctrl::HELLO_VER)
    {
        ctrl::Hello h{};
        if (!ctrl::parse_hello(f.data(), f.size(), h))
        {
            LOG_WARN("[CTRL] HELLO malformed -> dropping...");
            return;
        }
        if (!h.user_id.empty())
            peer_user_ = h.user_id;
        if (h.has_caps)
            peer_caps_ = h.caps;
        if (h.has_na32)
            peer_na32_ = h.na32;

        // Let TUI to update user id
        LOG_INFO("[CTRL] HELLO in: user='%s' caps=0x%08x na32=%02x%02x...",
                 peer_user_.empty() ? "<none>" : peer_user_.c_str(), peer_caps_, peer_na32_[0],
                 peer_na32_[1]);
        return;
    }

    if (!tail_enabled_.load(std::memory_order_relaxed))
        return;

    // parse -> reassemble -> aead.open -> print
    auto c = frag::parse(f);
    if (!c)
    {
        LOG_WARN("on_rx: dropping invalid frame");
        return;
    }
    // Feed to reassembler
    auto full = rx_.feed(*c);
    if (!full)  // not complete yet
        return;

    std::vector<std::uint8_t> plain;
    if (!aead_.open(*full, AAD, sizeof(AAD), plain))
    {
        LOG_WARN("on_rx: AEAD open failed");
        return;
    }
    const char       *p = reinterpret_cast<const char *>(plain.data());
    const std::size_t n = plain.size();
    LOG_INFO("[RECV] %.*s", n, p);
}

}  // namespace app

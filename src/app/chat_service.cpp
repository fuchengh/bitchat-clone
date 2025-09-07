#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "app/chat_service.hpp"
#include "proto/frag.hpp"
#include "transport/bluez_transport.hpp"
#include "transport/itransport.hpp"
#include "util/constants.hpp"
#include "util/log.hpp"

namespace app
{

constexpr std::uint8_t AAD[] = {'B', 'C', '1'};

ChatService::ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload)
    : tx_(t), aead_(aead), mtu_payload_(mtu_payload)
{
}

bool ChatService::start()
{
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

    return tx_.start(s, [this](const transport::Frame &f) { this->on_rx(f); });
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

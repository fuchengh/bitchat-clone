#include <cstring>
#include <iostream>

#include "app/chat_service.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"
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
    transport::Settings s{
        .role        = "loopback",
        .svc_uuid    = {},
        .tx_uuid     = {},
        .rx_uuid     = {},
        .mtu_payload = mtu_payload_,
    };

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
    const std::uint32_t msg_id = next_id_.fetch_add(1, std::memory_order_relaxed);

    auto chunks = frag::make_chunks(msg_id, sealed_text, mtu_payload_);
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

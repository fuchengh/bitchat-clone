#include <cstring>
#include <iostream>

#include "app/chat_service.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"
#include "util/log.hpp"

namespace app
{

ChatService::ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload)
    : tx_(t), aead_(aead), mtu_payload_(mtu_payload)
{
}

bool ChatService::start()
{
    //TODO: change role and mtu_payload
    transport::Settings s{};
    s.role        = "loopback";
    s.mtu_payload = 0;

    return tx_.start(s, [this](const transport::Frame &f) { this->on_rx(f); });
}

bool ChatService::send_text(std::string_view msg)
{
    // TODO: plain -> [aead.seal] -> frag::make_chunks -> transport.send
    std::vector<std::uint8_t> bytes(msg.begin(), msg.end());
    const std::size_t         mtu = (mtu_payload_ == 0)
                                        ? frag::MAX_PAYLOAD
                                        : std::min<std::size_t>(mtu_payload_, frag::MAX_PAYLOAD);

    auto chunks = frag::make_chunks(msg_id_++, bytes, mtu);
    if (!bytes.empty() && chunks.empty())
    {
        LOG_ERROR("send_text: make_chunks failed");
        return false;
    }

    for (const auto &ch : chunks)
    {
        auto frame = frag::serialize(ch);
        if (frame.empty())
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
    // TODO: parse -> reassemble -> aead.open -> print
    auto c = frag::parse(f);
    if (!c)
    {
        LOG_WARN("on_rx: dropping invalid frame");
        return;
    }
    // Feed to reassembler
    auto full = rx_.feed(*c);
    if (!full)
        return;

    // TODO: replace with AEAD.open.
    const char *p = reinterpret_cast<const char *>(full->data());
    const int   n = static_cast<int>(full->size());
    LOG_INFO("[RECV] %.*s", n, p);
}

}  // namespace app

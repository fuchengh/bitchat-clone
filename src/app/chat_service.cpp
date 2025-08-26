#include <cstring>
#include <iostream>

#include "app/chat_service.hpp"
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
    // TODO: plain -> [aead.seal -> frag::make_chunks] -> transport.send
    transport::Frame frame(msg.size());
    std::memcpy(frame.data(), msg.data(), msg.size());

    return tx_.send(frame);
}

void ChatService::on_rx(const transport::Frame &f)
{
    // TODO: parse -> reassemble -> aead.open -> print
    const char *p = reinterpret_cast<const char *>(f.data());
    const int   n = static_cast<int>(f.size());
    LOG_INFO("[RECV] %.*s", n, p);  // length-aware, safe even if there are NULs
}

}  // namespace app

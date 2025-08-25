#include <iostream>
#include "app/chat_service.hpp"

namespace app
{

ChatService::ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload)
    : t_(t), aead_(aead), mtu_payload_(mtu_payload)
{
}

bool ChatService::send_text(std::string_view msg)
{
    // TODO: plain -> aead.seal -> frag::make_chunks -> transport.send
    (void)msg;
    return true;
}

void ChatService::on_frame(const transport::Frame &f)
{
    // TODO: parse -> reassemble -> aead.open -> print
    (void)f;
}

}  // namespace app

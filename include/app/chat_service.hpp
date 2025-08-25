#pragma once
#include <atomic>
#include "crypto/psk_aead.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"

namespace app
{

class ChatService
{
  public:
    ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload);

    bool send_text(std::string_view msg);
    void on_frame(const transport::Frame &f);

    void set_tail(bool on) { tail_ = on; }

  private:
    [[maybe_unused]] transport::ITransport     &t_;
    [[maybe_unused]] aead::PskAead             &aead_;
    [[maybe_unused]] frag::Reassembler          rx_;
    [[maybe_unused]] std::size_t                mtu_payload_{100};
    [[maybe_unused]] std::atomic<std::uint32_t> next_msg_id_{1};
    [[maybe_unused]] std::atomic<std::uint64_t> send_ctr_{0}, recv_ctr_{0};
    bool                       tail_{true};
};

}  // namespace app

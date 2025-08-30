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

    bool start();
    bool send_text(std::string_view msg);
    void on_rx(const transport::Frame &f);

    void set_tail(bool on) { tail_ = on; }

  private:
    transport::ITransport                      &tx_;
    aead::PskAead                              &aead_;
    frag::Reassembler                           rx_;
    std::size_t                                 mtu_payload_{100};
    std::atomic<std::uint32_t>                  msg_id_{1};
    [[maybe_unused]] std::atomic<std::uint64_t> send_ctr_{0}, recv_ctr_{0};
    bool                       tail_{true};
};

}  // namespace app

#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>

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

    void set_tail(bool on) { tail_enabled_.store(on, std::memory_order_relaxed); }

  private:
    transport::ITransport                      &tx_;
    aead::PskAead                              &aead_;
    frag::Reassembler                           rx_;
    std::size_t                                 mtu_payload_{100};
    std::atomic<std::uint32_t>                  next_id_{1};
    std::atomic<bool>                           tail_enabled_{true};
};

}  // namespace app

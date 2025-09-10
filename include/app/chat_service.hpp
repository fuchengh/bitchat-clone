#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>

#include "crypto/psk_aead.hpp"
#include "proto/frag.hpp"
#include "transport/itransport.hpp"

namespace app
{

class ChatService
{
  public:
    ChatService(transport::ITransport &t, aead::PskAead &aead, std::size_t mtu_payload);
    ~ChatService()
    {
        hello_stop_.store(true);
        if (hello_thr_.joinable())
            hello_thr_.join();
    }

    bool start();
    void stop();
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
    // hello packet
    std::thread             hello_thr_;
    std::atomic_bool        hello_stop_{true};
    bool                    hello_sent_{false};
    std::string             local_user_;
    uint32_t                local_caps_{0};
    std::array<uint8_t, 32> na32_{};
    // peer state for TUI
    std::string             peer_user_;
    uint32_t                peer_caps_{0};
    std::array<uint8_t, 32> peer_na32_{};
};

}  // namespace app

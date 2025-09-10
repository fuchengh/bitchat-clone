#pragma once
#include <cstddef>

#include "transport/itransport.hpp"

namespace transport {

class LoopbackTransport final : public ITransport {
public:
  bool start(const Settings& s, OnFrame on_rx) override;
  bool send(const Frame& one_chunk) override;  // one BLE write-sized chunk
  void stop() override;
  bool link_ready() const override;

private:
  OnFrame     on_rx_{};
  std::size_t mtu_{0};
  bool        started_{false};
};

} // namespace transport

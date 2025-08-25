#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>

#include "transport/itransport.hpp"

namespace transport {

class LoopbackTransport final : public ITransport {
public:
  bool start(const Settings& s, OnFrame on_rx) override;
  bool send(const Frame& one_chunk) override;  // one BLE write-sized chunk
  void stop() override;

private:
  OnFrame     on_rx_{};
  std::size_t mtu_{0};
  bool        started_{false};
};

} // namespace transport

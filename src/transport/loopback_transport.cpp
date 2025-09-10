#include "transport/loopback_transport.hpp"

namespace transport
{
// LoopbackTransport: a fake link to test the pipeline (CLI → daemon → transport) without BLE.
bool LoopbackTransport::start(const Settings &s, OnFrame on_rx)
{
    on_rx_   = std::move(on_rx);
    mtu_     = s.mtu_payload;
    started_ = true;
    return true;
}

bool LoopbackTransport::send(const Frame &one_chunk)
{
    if (!started_ || !on_rx_)
        return false;
    if (mtu_ != 0 && one_chunk.size() > mtu_)
        return false;
    on_rx_(one_chunk);
    return true;
}

void LoopbackTransport::stop()
{
    started_ = false;
    on_rx_   = nullptr;
}

bool LoopbackTransport::link_ready() const
{
    return true;
}

}  // namespace transport
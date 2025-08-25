#include "transport/itransport.hpp"
// TODO: Implement loopback transport (send() should call on_rx).
namespace transport
{
struct LoopbackTransport : ITransport
{
    OnFrame rx_;
    bool    start(const Settings &, OnFrame cb) override
    {
        rx_ = cb;
        return true;
    }
    bool send(const Frame &f) override
    {
        if (rx_)
            rx_(f);
        return true;
    }
    void stop() override {}
};
}  // namespace transport

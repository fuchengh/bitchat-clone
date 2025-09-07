
#include "transport/bluez_transport.hpp"
#include "transport/itransport.hpp"
#include "util/log.hpp"

namespace transport
{
struct BluezTransport::Impl
{
    // TOOD: add sd-bus handles, object paths, watchers...
};

BluezTransport::BluezTransport(BluezConfig cfg) : cfg_(std::move(cfg)) {}

BluezTransport::~BluezTransport()
{
    stop();
}

bool BluezTransport::start(const Settings &s, OnFrame cb)
{
    if (running_.load(std::memory_order_relaxed))
        return true;  // idempotent

    settings_ = s;
    on_frame_ = std::move(cb);
    if (!impl_)
        impl_ = std::make_unique<Impl>();

    running_.store(true, std::memory_order_relaxed);

    LOG_INFO("[BLUEZ] stub start: role=%s adapter=%s mtu_payload=%zu svc=%s tx=%s rx=%s%s%s",
             (cfg_.role == Role::Central ? "central" : "peripheral"), cfg_.adapter.c_str(),
             settings_.mtu_payload, cfg_.svc_uuid.c_str(), cfg_.tx_uuid.c_str(),
             cfg_.rx_uuid.c_str(), cfg_.peer_addr ? " peer=" : "",
             cfg_.peer_addr ? cfg_.peer_addr->c_str() : "");
    // TODO
    return true;
}

bool BluezTransport::send(const Frame & /*frame*/)
{
    if (!running_.load(std::memory_order_relaxed))
        return false;
    // Stub only: later we will map Frame -> GATT Write.
    LOG_INFO("[BLUEZ] stub send (frame)");
    return true;
}

void BluezTransport::stop()
{
    if (!running_.exchange(false, std::memory_order_relaxed))
        return;
    LOG_INFO("[BLUEZ] stub stop");
    impl_.reset();
}

}  // namespace transport

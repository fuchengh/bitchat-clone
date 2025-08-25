#include "transport/itransport.hpp"
// TODO: Implement BlueZ D-Bus GATT (tx/rx) in next step.
namespace transport
{
struct BluezTransport : ITransport
{
    bool start(const Settings &, OnFrame) override { return false; }
    bool send(const Frame &) override { return false; }
    void stop() override {}
};
}  // namespace transport

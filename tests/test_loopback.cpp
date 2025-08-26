#include <gtest/gtest.h>

#include "transport/itransport.hpp"
#include "transport/loopback_transport.hpp"

using namespace transport;

TEST(Loopback, EchoesFrame)
{
    LoopbackTransport t;
    Frame             captured;

    Settings s{};
    s.role        = "loopback";
    s.mtu_payload = 100;

    ASSERT_TRUE(t.start(s, [&](const Frame &f) { captured = f; }));

    Frame f = {1, 2, 3, 4, 5};
    EXPECT_TRUE(t.send(f));
    EXPECT_EQ(captured, f);

    t.stop();
}

TEST(Loopback, SendFailsWhenNotStarted)
{
    LoopbackTransport t;
    Frame             f = {0x42};
    EXPECT_FALSE(t.send(f));
}

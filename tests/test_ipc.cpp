#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <thread>

#include "ctl/ipc.hpp"

TEST(IPC, TestExpandUser)
{
    const char *path      = std::getenv("HOME");
    const char *test_home = "/tmp/ut-home";
    setenv("HOME", test_home, 1);

    EXPECT_EQ(ipc::expand_user("~"), test_home);
    EXPECT_EQ(ipc::expand_user("~/x/y"), std::string(test_home) + "/x/y");
    EXPECT_EQ(ipc::expand_user("/abs/path"), "/abs/path");
    EXPECT_EQ(ipc::expand_user("relative/~/path"), "relative/~/path");

    if (path)
        setenv("HOME", path, 1);
}

TEST(IPC, TestExpandUserNoHomeEnv)
{
    const char *path = std::getenv("HOME");
    unsetenv("HOME");
    EXPECT_EQ(ipc::expand_user("~"), "~");
    EXPECT_EQ(ipc::expand_user("~/x"), "~/x");
    if (path)
        setenv("HOME", path, 1);
}

TEST(IPC, TestStartServerAndSendLine)
{
    // temporary socket path
    std::string sock = "/tmp/bitchat-ipc-ut-" + std::to_string(getpid()) + ".sock";

    // run server (blocks until QUIT)
    std::thread th([&] { ipc::start_server(sock, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // check if send_line executes without error
    ASSERT_TRUE(ipc::send_line(sock, "QUIT\n"));
    th.join();
    // server should unlink the socket
    EXPECT_FALSE(access(sock.c_str(), F_OK) == 0);
}

// tests/test_cli.cpp
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include "ctl/ipc.hpp"

using namespace std::chrono_literals;

namespace test_cli
{
std::mutex               g_mu;
std::vector<std::string> g_lines_to_send;

static void on_line_cb(const std::string &line)
{
    std::lock_guard<std::mutex> lockguard(g_mu);
    g_lines_to_send.push_back(line);
}

static std::string temp_sock_path()
{
    const char *tmp  = std::getenv("TMPDIR");
    std::string base = (tmp && *tmp) ? tmp : "/tmp";
    return base + "/bitchat-cli-test-" + std::to_string(::getpid()) + ".sock";
}

static int run_cli(const std::string &sock, const std::string &args)
{
    // ctest runs from the build directory; binaries live in ./bin
    std::string cmd = "./bin/bitchatctl --sock " + sock + " " + args;
    int         rc  = std::system(cmd.c_str());
    if (rc == -1)
        return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
}  // namespace

TEST(CLI, TestCliFunctionalality)
{
    test_cli::g_lines_to_send.clear();
    const auto sock = test_cli::temp_sock_path();

    std::atomic<bool> server_done{false};
    std::thread       th([&] {
        (void)ipc::start_server(sock, test_cli::on_line_cb);
        server_done.store(true);
    });

    // Wait for server to bind the socket
    for (int i = 0; i < 100; ++i)
    {
        if (access(sock.c_str(), F_OK) == 0)
            break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(access(sock.c_str(), F_OK) == 0) << "socket not created: " << sock;

    // Exercise CLI argument parsing + IPC
    EXPECT_EQ(test_cli::run_cli(sock, "send \"hello world!\""), 0);
    EXPECT_EQ(test_cli::run_cli(sock, "tail on"), 0);
    EXPECT_EQ(test_cli::run_cli(sock, "quit"), 0);

    th.join();
    ASSERT_TRUE(server_done.load());

    // Validate the lines the daemon saw
    {
        std::lock_guard<std::mutex> lockguard(test_cli::g_mu);
        ASSERT_GE(test_cli::g_lines_to_send.size(), 3u);
        EXPECT_EQ(test_cli::g_lines_to_send[0], "SEND hello world!");
        EXPECT_EQ(test_cli::g_lines_to_send[1], "TAIL on");
        EXPECT_EQ(test_cli::g_lines_to_send.back(), "QUIT");
    }

    // start_server should have cleaned up the socket file
    EXPECT_FALSE(access(sock.c_str(), F_OK) == 0);
}
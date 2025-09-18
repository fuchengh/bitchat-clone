// tests/test_env.cpp
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "util/constants.hpp"
#include "util/log.hpp"

// ENV guard
struct EnvGuard
{
    std::string key, old_val;
    bool        had = false;
    explicit EnvGuard(const char *k) : key(k)
    {
        const char *v = std::getenv(k);
        if (v)
        {
            had     = true;
            old_val = v;
        }
    }
    void set(const std::string &v) const { ::setenv(key.c_str(), v.c_str(), 1); }
    void unset() const { ::unsetenv(key.c_str()); }
    ~EnvGuard()
    {
        if (had)
            ::setenv(key.c_str(), old_val.c_str(), 1);
        else
            ::unsetenv(key.c_str());
    }
};

TEST(Env_CtlSockPath, FromEnv)
{
    EnvGuard          g("BITCHAT_CTL_SOCK");
    const std::string want = "/tmp/bitchat-test.sock";
    g.set(want);

    // should NOT log "Listening on ..." when env is set
    testing::internal::CaptureStderr();
    std::string got = constants::ctl_sock_path();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(got, want);
    EXPECT_TRUE(err.find("Listening on") == std::string::npos);
}

TEST(Env_CtlSockPath, DefaultFromHomeAndLogs)
{
    EnvGuard g_sock("BITCHAT_CTL_SOCK");
    g_sock.unset();

    EnvGuard              g_home("HOME");
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "bitchat-home";
    std::filesystem::create_directories(tmp);
    g_home.set(tmp.string());

    testing::internal::CaptureStderr();
    std::string got = constants::ctl_sock_path();
    std::string err = testing::internal::GetCapturedStderr();

    std::string want = (tmp / ".cache/bitchat-clone/ctl.sock").string();
    EXPECT_EQ(got, want);
    EXPECT_NE(err.find("Listening on " + want), std::string::npos);
}

TEST(LogLevel, FiltersByThreshold)
{
    using namespace bitchat;

    // ERROR-only: WARN should be suppressed, ERROR should appear
    set_log_level_by_name("ERROR");
    testing::internal::CaptureStderr();
    LOG_WARN("should_not_print_warn");
    std::string out1 = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(out1.find("should_not_print_warn") == std::string::npos);

    testing::internal::CaptureStderr();
    LOG_ERROR("should_print_error");
    std::string out2 = testing::internal::GetCapturedStderr();
    EXPECT_NE(out2.find("should_print_error"), std::string::npos);

    // DEBUG: DEBUG should appear
    set_log_level_by_name("DEBUG");
    testing::internal::CaptureStderr();
    LOG_DEBUG("debug_visible");
    std::string out3 = testing::internal::GetCapturedStderr();
    EXPECT_NE(out3.find("debug_visible"), std::string::npos);
}

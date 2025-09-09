#include <cstdio>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "ctl/ipc.hpp"
#include "util/constants.hpp"
#include "util/exitcodes.hpp"
#include "util/log.hpp"

namespace
{

static void print_usage()
{
    std::fprintf(stderr, "Usage:\n"
                         "  bitchatctl [--sock <path>] <command> [args]\n"
                         "\n"
                         "Commands:\n"
                         "  send <text...>\n"
                         "  tail on|off\n"
                         "  quit\n");
}

static int send_one_line(const std::string &sock, const std::string &line)
{
    if (line.empty() || line.find('\n') != std::string::npos)
    {
        print_usage();
        return exitc::bad_args;
    }
    std::string out = line;
    out.push_back('\n');
    if (!ipc::send_line(sock, out))
    {
        std::fprintf(stderr, "error: cannot reach daemon at %s\n", sock.c_str());
        return exitc::no_server;
    }
    return exitc::ok;
}

static int run_cmd(const std::string                             &cmd,
                   const std::vector<std::string>                &args,
                   const std::function<int(const std::string &)> &send_line)
{
    std::unordered_map<std::string, std::function<int()>> cmd_map = {
        {"send",
         [&]() -> int {
             if (args.size() < 2)
             {
                 print_usage();
                 return exitc::bad_args;
             }
             std::string text;
             for (size_t i = 1; i < args.size(); ++i)
             {
                 if (i > 1)
                     text.push_back(' ');
                 text += args[i];
             }
             return send_line("SEND " + text);
         }},
        {"tail",
         [&]() -> int {
             if (args.size() != 2 || (args[1] != "on" && args[1] != "off"))
             {
                 print_usage();
                 return exitc::bad_args;
             }
             return send_line("TAIL " + args[1]);
         }},
        {"quit", [&]() -> int { return send_line("QUIT"); }},
    };

    auto it = cmd_map.find(cmd);
    if (it == cmd_map.end())
    {
        std::fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
        print_usage();
        return exitc::bad_args;
    }
    LOG_DEBUG("Running command: %s", cmd.c_str());
    return it->second();
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        print_usage();
        return exitc::bad_args;
    }

    // parse options (only --sock)
    std::string              sock = ipc::expand_user(constants::ctl_sock_path());
    std::vector<std::string> args;
    args.reserve(argc - 1);

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--sock" && i + 1 < argc)
        {
            sock = ipc::expand_user(argv[++i]);
        }
        else
        {
            args.push_back(std::move(a));
        }
    }
    if (args.empty())
    {
        print_usage();
        return exitc::bad_args;
    }

    const std::string &cmd = args[0];
    auto sender = [&](const std::string &line) -> int { return send_one_line(sock, line); };

    int rc = run_cmd(cmd, args, sender);
    return rc;
}

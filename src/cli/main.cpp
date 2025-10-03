#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

#include "ctl/ipc.hpp"
#include "util/constants.hpp"
#include "util/exitcodes.hpp"
#include "util/log.hpp"

namespace
{

static bool is_valid_mac(const std::string &mac)
{
    if (mac.size() != 17)
        return false;
    for (size_t i = 0; i < mac.size(); ++i)
    {
        if ((i % 3) == 2)
        {
            if (mac[i] != ':')
                return false;
        }
        else
        {
            unsigned char c = static_cast<unsigned char>(mac[i]);
            if (!std::isxdigit(c))
                return false;
        }
    }
    return true;
}

static std::string to_upper_mac(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string to_lower(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// --------------------------------------------------------------------
// CLI usage
// --------------------------------------------------------------------
static void print_usage()
{
    std::fprintf(stderr, "Usage:\n"
                         "  bitchatctl [--sock <path>] <command> [args]\n"
                         "\n"
                         "Commands:\n"
                         "  send <text...>\n"
                         "  tail on|off\n"
                         "  peers\n"
                         "  connect AA:BB:CC:DD:EE:FF\n"
                         "  disconnect\n"
                         "  quit\n");
}

static int send_one_line(const std::string &sock, const std::string &line)
{
    if (line.empty() || line.find('\n') != std::string::npos)
    {
        print_usage();
        if (line.empty())
            std::fprintf(stderr, "error: empty command line to daemon\n");
        else
            std::fprintf(stderr, "error: command line must not contain newline characters\n");

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
             if (args.size() != 2)
             {
                 print_usage();
                 return exitc::bad_args;
             }
             std::string v = to_lower(args[1]);
             if (v != "on" && v != "off")
             {
                 std::fprintf(stderr, "error: tail expects 'on' or 'off'\n");
                 return exitc::bad_args;
             }
             return send_line("TAIL " + v);
         }},
        {"peers", [&]() -> int { return send_line("PEERS"); }},
        {"connect",
         [&]() -> int {
             if (args.size() != 2)
             {
                 print_usage();
                 return exitc::bad_args;
             }
             std::string mac = to_upper_mac(args[1]);
             if (!is_valid_mac(mac))
             {
                 std::fprintf(stderr, "error: invalid MAC address: %s\n", args[1].c_str());
                 return exitc::bad_args;
             }
             return send_line("CONNECT " + mac);
         }},
        {"disconnect", [&]() -> int { return send_line("DISCONNECT"); }},
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
    std::string sock = ipc::expand_user(constants::ctl_sock_path());
    // Allow environment override, then CLI --sock overrides env
    if (const char *e = std::getenv("BITCHAT_CTL_SOCK"))
    {
        if (*e)
            sock = ipc::expand_user(e);
    }

    std::vector<std::string> args;
    args.reserve(argc - 1);

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--help" || a == "-h")
        {
            print_usage();
            return exitc::ok;
        }
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

#include "ctl/ipc.hpp"
// TODO: Implement Unix domain socket server/client.

namespace ipc
{
bool start_server(const std::string &, void (*)(const std::string &))
{
    return true;
}
bool send_line(const std::string &, const std::string &)
{
    return true;
}
std::string expand_user(const std::string &p)
{
    return p;
}
}  // namespace ipc

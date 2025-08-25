#pragma once
#include <string>

namespace ipc
{

bool        start_server(const std::string &sock_path, void (*on_line)(const std::string &));
bool        send_line(const std::string &sock_path, const std::string &line);
std::string expand_user(const std::string &path);

}  // namespace ipc

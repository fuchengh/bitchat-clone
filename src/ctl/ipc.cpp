#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctl/ipc.hpp"
#include "util/log.hpp"

namespace ipc
{

bool start_server(const std::string &, void (*)(const std::string &))
{
    return true;
}

bool send_line(const std::string &sock_path, const std::string &line)
{
    sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));

    if (line.empty() || sock_path.empty())
    {
        errno = EINVAL;
        LOG_ERROR("Invalid line or socket path");
        return false;
    }
    if (sock_path.size() >= sizeof(addr.sun_path))
    {
        errno = ENAMETOOLONG;
        LOG_ERROR("Path name too long for AF_UNIX: %s", sock_path.c_str());
        return false;
    }

    // create socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
    {
        LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags != -1)
        (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    addr.sun_len                             = static_cast<uint8_t>(SUN_LEN(&addr));
    int one                                  = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    // connect
    socklen_t addr_len = static_cast<socklen_t>(SUN_LEN(&addr));
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), addr_len) == -1)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        LOG_ERROR("connect() failed: %s", std::strerror(errno));
        return false;
    }
    // send entire line
    LOG_DEBUG("Sending line: %s", line.c_str());
    const char *buf  = line.data();
    size_t      len  = line.size();
    size_t      sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR)
            continue;
        int saved = errno;
        close(fd);
        errno = saved;
        LOG_ERROR("send() failed: %s", std::strerror(errno));
        return false;
    }

    close(fd);
    return true;
}

std::string expand_user(const std::string &p)
{
    // expand leading '~' or '~/' to $HOME
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/'))
    {
        const char *home = std::getenv("HOME");
        if (home && (*home))  // non-empty
        {
            return (p.size() == 1) ? std::string(home) : std::string(home) + p.substr(1);
        }
    }
    return p;
}

}  // namespace ipc

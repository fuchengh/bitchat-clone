#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctl/ipc.hpp"
#include "util/log.hpp"

namespace ipc
{
namespace fs = std::filesystem;

static bool ensure_parent_dir(const std::string &sock_path)
{
    std::error_code ec;
    fs::path        p(sock_path);
    fs::path        dir = p.parent_path();
    if (dir.empty())
        return true;  // socket in CWD

    if (!fs::exists(dir, ec))
    {
        if (!fs::create_directories(dir, ec))
        {
            LOG_ERROR("create_directories(%s) failed: %s", dir.string().c_str(),
                      ec.message().c_str());
            return false;
        }
    }
    // Enforce 0700 on the directory
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec)
    {
        LOG_WARN("permissions(%s, 0700) failed: %s", dir.string().c_str(), ec.message().c_str());
    }
    return true;
}

bool start_server(const std::string &sock_path, void (*on_line)(const std::string &))
{
    sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));

    if (sock_path.empty())
    {
        errno = EINVAL;
        LOG_ERROR("Invalid socket path");
        return false;
    }
    if (sock_path.size() >= sizeof(addr.sun_path))
    {
        errno = ENAMETOOLONG;
        LOG_ERROR("Path name too long for AF_UNIX: %s", sock_path.c_str());
        return false;
    }

    // ensure parent directory exists (mkdir -p)
    if (!ensure_parent_dir(sock_path))
        return false;

    (void)::unlink(sock_path.c_str());  // ignore errors

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
    {
        LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }

    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags != -1)
        fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    socklen_t addr_len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                                std::strlen(addr.sun_path) + 1);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), addr_len) == -1)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        LOG_ERROR("bind() failed: %s", std::strerror(errno));
        return false;
    }
    if (listen(fd, 4) == -1)
    {
        int saved = errno;
        close(fd);
        unlink(sock_path.c_str());
        errno = saved;
        LOG_ERROR("listen() failed: %s", std::strerror(errno));
        return false;
    }

    LOG_DEBUG("Listening on %s", sock_path.c_str());

    while (1)
    {
        int newfd = accept(fd, nullptr, nullptr);
        if (newfd == -1)
        {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            unlink(sock_path.c_str());
            errno = saved;
            LOG_ERROR("accept() failed: %s", std::strerror(errno));
            return false;
        }

        int aflags = fcntl(newfd, F_GETFD, 0);
        if (aflags != -1)
            fcntl(newfd, F_SETFD, aflags | FD_CLOEXEC);
        std::string line;
        char        buf[256];
        bool        ok = true;

        while (1)
        {
            ssize_t n = recv(newfd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                line.append(buf, static_cast<size_t>(n));
                if (line.find('\n') != std::string::npos)
                    break;
                continue;
            }
            if (n == 0)
                break;  // EOF
            if (n == -1 && errno == EINTR)
                continue;

            int saved = errno;
            close(newfd);
            errno = saved;
            LOG_ERROR("recv() failed: %s", std::strerror(errno));
            ok = false;
            break;  // abort this connection
        }

        if (!ok)
            continue;  // keep server alive; accept next connection

        // take first line only
        auto        pos   = line.find('\n');
        std::string first = (pos == std::string::npos) ? line : line.substr(0, pos);
        // trim optional '\r'
        if (!first.empty() && first.back() == '\r')
            first.pop_back();

        if (on_line)
            on_line(first);

        close(newfd);

        if (first == "QUIT")
            break;  // graceful shutdown
    }

    close(fd);
    unlink(sock_path.c_str());
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

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
    {
        LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }

    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags != -1)
        fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    socklen_t addr_len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                                std::strlen(addr.sun_path) + 1);

    // connect
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
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

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

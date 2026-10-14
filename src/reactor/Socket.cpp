/**
 * Project Untitled
 */

#include "Socket.h"
#include <cstdio>
#include <stdexcept>

Socket::Socket()
: _fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))
{
    if (_fd < 0) {
        perror("socket");
        throw std::runtime_error("socket create failed");
    }
}

Socket::Socket(int fd)
: _fd(fd)
{
}

int Socket::getFd() const
{
    return _fd;
}

Socket::~Socket()
{
    if (_fd >= 0) {
        ::close(_fd);
    }
}

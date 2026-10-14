#include "Acceptor.h"
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/socket.h>

Acceptor::Acceptor(const std::string &ip, unsigned short port)
: _sock()
, _addr(ip, port)
{
}

void Acceptor::ready()
{
    setReuseAddr();
    setReusePort();
    bind();
    listen();
}

int Acceptor::getFd()
{
    return _sock.getFd();
}

void Acceptor::setReuseAddr()
{
    int on = 1;
    if (::setsockopt(_sock.getFd(), SOL_SOCKET, SO_REUSEADDR,
                     &on, sizeof(on)) < 0) {
        throw std::runtime_error("setsockopt SO_REUSEADDR failed");
    }
}

void Acceptor::setReusePort()
{
    int on = 1;
    if (::setsockopt(_sock.getFd(), SOL_SOCKET, SO_REUSEPORT,
                     &on, sizeof(on)) < 0) {
        throw std::runtime_error("setsockopt SO_REUSEPORT failed");
    }
}

void Acceptor::bind()
{
    const int ret = ::bind(
        _sock.getFd(),
        reinterpret_cast<const struct sockaddr *>(_addr.getInetAddressPtr()),
        sizeof(struct sockaddr_in));
    if (ret < 0) {
        throw std::runtime_error("bind failed");
    }
}

void Acceptor::listen()
{
    if (::listen(_sock.getFd(), 128) < 0) {
        throw std::runtime_error("listen failed");
    }
}

int Acceptor::accept()
{
    int fd;
    do {
        fd = ::accept4(_sock.getFd(), nullptr, nullptr,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("accept4");
    }
    return fd;
}

Acceptor::~Acceptor()
{
}

#include "SocketIO.h"
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

SocketIO::SocketIO(int fd)
: _fd(fd)
{
}

int SocketIO::readn(char *buf, int len)
{
    int totalRead = 0;
    while (totalRead < len) {
        const int ret = static_cast<int>(
            ::read(_fd, buf + totalRead, len - totalRead));
        if (ret > 0) {
            totalRead += ret;
        } else if (ret == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            perror("readn");
            return -1;
        }
    }
    return totalRead;
}

int SocketIO::readLine(char *buf, int len)
{
    int left = len - 1;
    char *current = buf;
    int total = 0;

    while (left > 0) {
        const int ret = static_cast<int>(
            ::recv(_fd, current, left, MSG_PEEK | MSG_DONTWAIT));
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (ret < 0) {
            perror("readLine");
            return -1;
        }
        if (ret == 0) {
            break;
        }

        int consume = ret;
        for (int idx = 0; idx < ret; ++idx) {
            if (current[idx] == '\n') {
                consume = idx + 1;
                break;
            }
        }

        const int actual = readn(current, consume);
        if (actual <= 0) {
            break;
        }

        current += actual;
        total += actual;
        left -= actual;
        if (actual == consume && current[-1] == '\n') {
            break;
        }
    }

    *current = '\0';
    return total;
}

int SocketIO::writen(const char *buf, int len)
{
    int totalWritten = 0;
    while (totalWritten < len) {
        const int ret = static_cast<int>(
            ::send(_fd, buf + totalWritten, len - totalWritten,
                   MSG_NOSIGNAL | MSG_DONTWAIT));
        if (ret > 0) {
            totalWritten += ret;
        } else if (ret == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            perror("writen");
            return -1;
        }
    }
    return totalWritten;
}

SocketIO::~SocketIO()
{
}

/**
 * Project Untitled
 */


#ifndef _SOCKET_H
#define _SOCKET_H

#include <sys/socket.h>
#include <unistd.h>
#include "NoCopy.h"

class Socket : public NoCopy {
public:
    Socket();
    explicit Socket(int fd);
    ~Socket();

    int getFd() const;

private:
    int _fd;
};

#endif //_SOCKET_H

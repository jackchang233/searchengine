/**
 * Project Untitled
 */


#ifndef _ACCEPTOR_H
#define _ACCEPTOR_H

#include <string>
#include "Socket.h"
#include "InetAddress.h"

class Acceptor {
public:
    /**
     * @param ip
     * @param port
     */
    Acceptor(const std::string & ip, unsigned short port);

    int getFd();

    void ready();

    int accept();

    ~Acceptor();

private:
    Socket _sock;
    InetAddress _addr;

    void setReuseAddr();
    void setReusePort();
    void bind();
    void listen();
};

#endif //_ACCEPTOR_H

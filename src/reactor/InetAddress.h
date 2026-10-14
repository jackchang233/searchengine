/**
 * Project Untitled
 */


#ifndef _INETADDRESS_H
#define _INETADDRESS_H

#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>

class InetAddress {
public:
    InetAddress() = default;

    /**
     * @param ip
     * @param port
     */
    InetAddress(const std::string & ip, unsigned short port);

    /**
     * @param addr
     */
    explicit InetAddress(const struct sockaddr_in & addr);

    std::string getIp() const;

    unsigned short getPort() const;

    const struct sockaddr_in * getInetAddressPtr() const;

    ~InetAddress();

private:
    struct sockaddr_in _addr;
};

#endif //_INETADDRESS_H

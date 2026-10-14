/**
 * Project Untitled
 */


#include "InetAddress.h"
#include <cstring>

/**
 * InetAddress implementation
 */


/**
 * @param ip
 * @param port
 */
InetAddress::InetAddress(const std::string & ip, unsigned short port) {
    std::memset(&_addr, 0, sizeof(_addr));
    _addr.sin_family = AF_INET;
    _addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &_addr.sin_addr);
}

/**
 * @param addr
 */
InetAddress::InetAddress(const struct sockaddr_in & addr)
    : _addr(addr) {
}

std::string InetAddress::getIp() const {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &_addr.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

unsigned short InetAddress::getPort() const {
    return ntohs(_addr.sin_port);
}

const struct sockaddr_in * InetAddress::getInetAddressPtr() const {
    return &_addr;
}

InetAddress::~InetAddress() {

}

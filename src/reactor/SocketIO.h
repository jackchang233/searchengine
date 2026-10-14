/**
 * Project Untitled
 */


#ifndef _SOCKETIO_H
#define _SOCKETIO_H
#include "Socket.h"
class SocketIO {
public:

/**
 * @param fd
 */
explicit SocketIO(int fd);

/**
 * @param buf
 * @param len
 */
int readn(char * buf, int len);

/**
 * @param buf
 * @param len
 */
int readLine(char * buf, int len);

/**
 * @param buf
 * @param len
 */
int writen(const char * buf, int len);

~SocketIO();
private:
    int _fd;
};

#endif //_SOCKETIO_H

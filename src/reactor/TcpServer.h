#ifndef TCPSERVER_H
#define TCPSERVER_H

#include "EventLoop.h"
#include <string>

class TcpServer
{
public:
    TcpServer(const std::string &ip, unsigned short port,
              FrameMode frameMode = FrameMode::Line,
              std::size_t maxFrameBytes = 1024 * 1024);
    ~TcpServer();

    void start();
    void stop();
    void beginDrain(std::size_t timeoutMs, std::function<bool()> workDrained);

    // 新连接、消息、关闭回调都在 Reactor 线程执行。
    void setAllCallback(TcpConnCallback onConnection,
                        TcpFrameCallback onFrame,
                        TcpConnCallback onClose,
                        TcpConnCallback onReadClosed = nullptr);

private:
    Acceptor _acceptor;
    EventLoop _loop;
};

#endif

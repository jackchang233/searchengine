#include "TcpServer.h"
#include <utility>

TcpServer::TcpServer(const std::string &ip, unsigned short port,
                     FrameMode frameMode, std::size_t maxFrameBytes)
: _acceptor(ip, port)
, _loop(_acceptor, frameMode, maxFrameBytes)
{
}

TcpServer::~TcpServer()
{
    _loop.unloop();
}

void TcpServer::start()
{
    _acceptor.ready();
    _loop.loop();
}

void TcpServer::stop()
{
    _loop.unloop();
}

void TcpServer::beginDrain(std::size_t timeoutMs,
                           std::function<bool()> workDrained)
{
    _loop.beginDrain(timeoutMs, std::move(workDrained));
}

void TcpServer::setAllCallback(TcpConnCallback onConnection,
                               TcpFrameCallback onFrame,
                               TcpConnCallback onClose,
                               TcpConnCallback onReadClosed)
{
    _loop.setOnNewConnection(std::move(onConnection));
    _loop.setOnFrame(std::move(onFrame));
    _loop.setOnClose(std::move(onClose));
    _loop.setOnReadClosed(std::move(onReadClosed));
}

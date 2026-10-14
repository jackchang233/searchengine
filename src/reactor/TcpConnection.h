#ifndef _TCPCONNECTION_H
#define _TCPCONNECTION_H

#include "InetAddress.h"
#include "Socket.h"
#include "FrameCodec.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using TcpConnPtr = TcpConnectionPtr;
using TcpConnCallback = std::function<void(const TcpConnPtr &)>;
using TcpFrameCallback = std::function<void(const TcpConnPtr &, const Frame &)>;

class TcpConnection
    : public std::enable_shared_from_this<TcpConnection> {
public:
    using TcpCallback = TcpConnCallback;

    TcpConnection(int fd,
                  EventLoop &loop,
                  TcpFrameCallback onFrame = nullptr,
                  TcpCallback onClose = nullptr,
                  FrameMode frameMode = FrameMode::Line,
                  std::size_t maxFrameBytes = 1024 * 1024,
                  TcpCallback onReadClosed = nullptr);
    ~TcpConnection();

    // 仅供 Reactor 线程直接发送。
    void send(const std::string &msg);

    // 可从工作线程调用；真正的写操作会投递到 Reactor 线程。
    void sendInLoop(const std::string &msg);

    // 将连接相关的完成处理投递回 Reactor；回调内可安全更新请求状态并发送。
    void postToLoop(std::function<void(const TcpConnPtr &)> callback);

    std::string toString() const;
    bool isConnected() const;
    bool isReadClosed() const;
    bool hasPendingOutput() const;
    // 对端关闭写方向后，由业务层确认所有请求已完成时调用。
    void closeAfterFlush();

    // 由 EventLoop 调用；返回 false 表示连接已断开。
    bool handleReadEvent();
    bool handleWriteEvent();
    void handleCloseEvent();

private:
    InetAddress getLocalAddr() const;
    InetAddress getPeerAddr() const;
    void handleClose();
    bool dispatchFrames(std::size_t &budget);
    void scheduleReadContinuation();

private:
    Socket _socket;
    InetAddress _localAddr;
    InetAddress _peerAddr;
    EventLoop &_loop;
    std::atomic<bool> _connected;
    std::string _inputBuffer;
    std::string _outputBuffer;
    TcpFrameCallback _onFrame;
    TcpCallback _onClose;
    TcpCallback _onReadClosed;
    FrameMode _frameMode;
    std::size_t _maxFrameBytes;
    bool _readContinuationQueued;
    bool _peerReadClosed;
    bool _closeAfterFlush;
    void scheduleClose();
};

#endif

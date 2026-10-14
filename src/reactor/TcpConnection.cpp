#include "TcpConnection.h"
#include "EventLoop.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

namespace {
const size_t kReadBufferSize = 64 * 1024;
const size_t kMaxInputBufferSize = 1024 * 1024;
const size_t kMaxOutputBufferSize = 16 * 1024 * 1024;
const size_t kMaxFramesPerReadTurn = 32;
}

TcpConnection::TcpConnection(int fd,
                             EventLoop &loop,
                             TcpFrameCallback onFrame,
                             TcpCallback onClose,
                             FrameMode frameMode,
                             std::size_t maxFrameBytes,
                             TcpCallback onReadClosed)
: _socket(fd)
, _localAddr(getLocalAddr())
, _peerAddr(getPeerAddr())
, _loop(loop)
, _connected(true)
, _inputBuffer()
, _outputBuffer()
, _onFrame(std::move(onFrame))
, _onClose(std::move(onClose))
, _onReadClosed(std::move(onReadClosed))
, _frameMode(frameMode)
, _maxFrameBytes(maxFrameBytes)
, _readContinuationQueued(false)
, _peerReadClosed(false)
, _closeAfterFlush(false)
{
    if (_maxFrameBytes == 0 ||
        _maxFrameBytes > std::numeric_limits<std::size_t>::max() -
                             TlvFrameCodec::HeaderSize) {
        throw std::invalid_argument("maximum frame bytes must be positive");
    }
}

TcpConnection::~TcpConnection()
{
}

void TcpConnection::send(const std::string &msg)
{
    if (!_connected.load() || msg.empty()) {
        return;
    }

    if (_outputBuffer.size() + msg.size() > kMaxOutputBufferSize) {
        scheduleClose();
        handleClose();
        return;
    }

    // 已有积压数据时保持消息顺序，等待 EPOLLOUT 继续发送。
    if (!_outputBuffer.empty()) {
        _outputBuffer.append(msg);
        _loop.enableWriting(_socket.getFd());
        return;
    }

    ssize_t written;
    do {
        written = ::send(_socket.getFd(), msg.data(), msg.size(),
                         MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (written < 0 && errno == EINTR);

    if (written >= 0) {
        const size_t count = static_cast<size_t>(written);
        if (count < msg.size()) {
            _outputBuffer.assign(msg.data() + count, msg.size() - count);
            _loop.enableWriting(_socket.getFd());
        }
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        _outputBuffer = msg;
        _loop.enableWriting(_socket.getFd());
        return;
    }

    // sendInLoop 可能从 pending functor 调用，延迟移除可避免当前调用栈
    // 中销毁 TcpConnection。
    scheduleClose();
    handleClose();
}

void TcpConnection::sendInLoop(const std::string &msg)
{
    TcpConnPtr self = shared_from_this();
    _loop.runInLoop([self, msg] {
        if (self->isConnected()) {
            self->send(msg);
        }
    });
}

void TcpConnection::postToLoop(
    std::function<void(const TcpConnPtr &)> callback)
{
    if (!callback) {
        return;
    }
    TcpConnPtr self = shared_from_this();
    _loop.runInLoop([self, callback] {
        callback(self);
    });
}

bool TcpConnection::isConnected() const
{
    return _connected.load();
}

bool TcpConnection::isReadClosed() const
{
    return _peerReadClosed;
}

bool TcpConnection::hasPendingOutput() const
{
    return !_outputBuffer.empty();
}

void TcpConnection::scheduleClose()
{
    const int fd = _socket.getFd();
    std::weak_ptr<TcpConnection> weak = shared_from_this();
    EventLoop *loop = &_loop;
    _loop.queueInLoop([loop, fd, weak] {
        const TcpConnPtr connection = weak.lock();
        if (connection) {
            loop->removeConnection(fd, connection.get());
        }
    });
}

void TcpConnection::closeAfterFlush()
{
    _closeAfterFlush = true;
    if (_outputBuffer.empty()) {
        scheduleClose();
    }
}

std::string TcpConnection::toString() const
{
    std::ostringstream output;
    output << "Local IP:" << _localAddr.getIp() << "\n"
           << "Local Port:" << _localAddr.getPort() << "\n"
           << "Peer IP:" << _peerAddr.getIp() << "\n"
           << "Peer Port:" << _peerAddr.getPort() << "\n";
    return output.str();
}

InetAddress TcpConnection::getLocalAddr() const
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    socklen_t length = sizeof(addr);
    if (::getsockname(_socket.getFd(),
                      reinterpret_cast<sockaddr *>(&addr), &length) < 0) {
        perror("getsockname");
    }
    return InetAddress(addr);
}

InetAddress TcpConnection::getPeerAddr() const
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    socklen_t length = sizeof(addr);
    if (::getpeername(_socket.getFd(),
                      reinterpret_cast<sockaddr *>(&addr), &length) < 0) {
        perror("getpeername");
    }
    return InetAddress(addr);
}

void TcpConnection::handleClose()
{
    bool expected = true;
    if (_connected.compare_exchange_strong(expected, false) && _onClose) {
        _onClose(shared_from_this());
    }
}

void TcpConnection::handleCloseEvent()
{
    handleClose();
}

bool TcpConnection::handleReadEvent()
{
    if (_peerReadClosed) {
        return true;
    }
    char buffer[kReadBufferSize];
    std::size_t budget = kMaxFramesPerReadTurn;

    if (!dispatchFrames(budget)) {
        return false;
    }
    if (budget == 0) {
        scheduleReadContinuation();
        return true;
    }

    while (true) {
        const ssize_t count = ::recv(_socket.getFd(), buffer, sizeof(buffer),
                                     MSG_DONTWAIT);
        if (count > 0) {
            _inputBuffer.append(buffer, static_cast<size_t>(count));
            if (!dispatchFrames(budget)) {
                return false;
            }
            const std::size_t maxBuffered = std::max(
                kMaxInputBufferSize,
                _maxFrameBytes + TlvFrameCodec::HeaderSize);
            if (_inputBuffer.size() > maxBuffered) {
                return false;
            }
            if (budget == 0) {
                scheduleReadContinuation();
                return true;
            }
            continue;
        }
        if (count == 0) {
            // EOF 后残留的半帧无法再完成；舍弃它，但保留先前完整帧
            // 的在途计算和已经排队的响应，待写完后再关闭连接。
            _inputBuffer.clear();
            _peerReadClosed = true;
            if (_outputBuffer.empty()) {
                _loop.disableWriting(_socket.getFd());
            } else {
                _loop.enableWriting(_socket.getFd());
            }
            if (_onReadClosed) {
                _onReadClosed(shared_from_this());
            } else {
                closeAfterFlush();
            }
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        return false;
    }

    return true;
}

bool TcpConnection::dispatchFrames(std::size_t &budget)
{
    while (_connected.load() && !_inputBuffer.empty() && budget > 0) {
        const FrameDecodeResult result =
            _frameMode == FrameMode::Line
                ? LineFrameCodec::decode(_inputBuffer, _maxFrameBytes)
                : TlvFrameCodec::decode(_inputBuffer, _maxFrameBytes,
                                        FrameDirection::RequestsOnly);
        if (result.status == FrameDecodeStatus::NeedMore) {
            break;
        }
        if (result.status == FrameDecodeStatus::InvalidFrame) {
            return false;
        }
        _inputBuffer.erase(0, result.consumed);
        --budget;
        if (_onFrame) {
            _onFrame(shared_from_this(), result.frame);
        }
    }
    return _connected.load();
}

void TcpConnection::scheduleReadContinuation()
{
    if (_readContinuationQueued) {
        return;
    }
    _readContinuationQueued = true;
    std::weak_ptr<TcpConnection> weak = shared_from_this();
    _loop.queueInLoop([weak] {
        const TcpConnPtr connection = weak.lock();
        if (!connection || !connection->isConnected()) {
            return;
        }
        connection->_readContinuationQueued = false;
        try {
            if (connection->handleReadEvent()) {
                return;
            }
        } catch (...) {
            connection->_loop.removeConnection(connection->_socket.getFd());
            throw;
        }
        connection->_loop.removeConnection(connection->_socket.getFd());
    });
}

bool TcpConnection::handleWriteEvent()
{
    while (!_outputBuffer.empty()) {
        const ssize_t count = ::send(
            _socket.getFd(), _outputBuffer.data(), _outputBuffer.size(),
            MSG_NOSIGNAL | MSG_DONTWAIT);

        if (count > 0) {
            _outputBuffer.erase(0, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }

    _loop.disableWriting(_socket.getFd());
    if (_closeAfterFlush) {
        return false;
    }
    return true;
}

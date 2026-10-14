#include "EventLoop.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

EventLoop::EventLoop(Acceptor &acceptor,
                     FrameMode frameMode,
                     std::size_t maxFrameBytes)
: _epfd(createEpollFd())
, _wakeupFd(createWakeupFd())
, _evList(1024)
, _isLooping(false)
, _stopRequested(false)
, _loopThreadId()
, _acceptor(acceptor)
, _conns()
, _onNewConnection()
, _onFrame()
, _onClose()
, _onReadClosed()
, _frameMode(frameMode)
, _maxFrameBytes(maxFrameBytes)
, _acceptingConnections(true)
, _draining(false)
, _drainDeadline()
, _workDrained()
, _pendingMutex()
, _pendingFunctors()
{
    addEpollFd(_acceptor.getFd());
    addEpollFd(_wakeupFd);
}

EventLoop::~EventLoop()
{
    ::close(_wakeupFd);
    ::close(_epfd);
}

void EventLoop::loop()
{
    _loopThreadId = std::this_thread::get_id();
    _isLooping.store(true);
    if (_stopRequested.load()) {
        _isLooping.store(false);
    }
    while (_isLooping.load()) {
        waitEpollFd();
    }
}

void EventLoop::unloop()
{
    _stopRequested.store(true);
    _isLooping.store(false);
    wakeup();
}

void EventLoop::beginDrain(std::size_t timeoutMs,
                           std::function<bool()> workDrained)
{
    queueInLoop([this, timeoutMs, workDrained] {
        if (_draining) {
            return;
        }
        _draining = true;
        _acceptingConnections = false;
        _workDrained = workDrained;
        _drainDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMs);
        delEpollFd(_acceptor.getFd());
    });
}

int EventLoop::createEpollFd()
{
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("epoll_create1 failed");
    }
    return fd;
}

int EventLoop::createWakeupFd()
{
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        ::close(_epfd);
        throw std::runtime_error("eventfd failed");
    }
    return fd;
}

void EventLoop::addEpollFd(int fd)
{
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &event) < 0) {
        throw std::runtime_error("epoll_ctl add failed");
    }
}

void EventLoop::updateEpollFd(int fd, uint32_t events)
{
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.data.fd = fd;
    event.events = events;

    if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &event) < 0 &&
        errno != ENOENT && errno != EBADF) {
        perror("epoll_ctl mod");
    }
}

void EventLoop::delEpollFd(int fd)
{
    if (::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
        errno != ENOENT && errno != EBADF) {
        perror("epoll_ctl del");
    }
}

void EventLoop::waitEpollFd()
{
    int timeout = -1;
    if (_draining) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(_drainDeadline -
                                       std::chrono::steady_clock::now()).count();
        timeout = remaining <= 0 ? 0 :
                  remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
    }
    int ready;
    do {
        ready = ::epoll_wait(_epfd, _evList.data(),
                             static_cast<int>(_evList.size()), timeout);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0) {
        perror("epoll_wait");
        return;
    }

    if (ready == static_cast<int>(_evList.size())) {
        _evList.resize(_evList.size() * 2);
    }

    const int listenFd = _acceptor.getFd();
    for (int index = 0; index < ready; ++index) {
        const int fd = _evList[index].data.fd;
        const uint32_t events = _evList[index].events;

        if (fd == _wakeupFd) {
            handleWakeup();
            doPendingFunctors();
        } else if (fd == listenFd) {
            if (_acceptingConnections) {
                handleNewConnect();
            }
        } else {
            handleConnectionEvent(fd, events);
        }
    }
    checkDrain();
}

void EventLoop::checkDrain()
{
    if (!_draining) {
        return;
    }
    bool outputDrained = true;
    for (const auto &entry : _conns) {
        if (entry.second->hasPendingOutput()) {
            outputDrained = false;
            break;
        }
    }
    if ((outputDrained && (!_workDrained || _workDrained())) ||
        std::chrono::steady_clock::now() >= _drainDeadline) {
        closeAllConnections();
        _isLooping.store(false);
    }
}

void EventLoop::closeAllConnections()
{
    while (!_conns.empty()) {
        removeConnection(_conns.begin()->first);
    }
}

void EventLoop::handleNewConnect()
{
    // LT 模式下也一次性取完已经排队的连接，降低监听 fd 的重复唤醒。
    while (true) {
        const int fd = _acceptor.accept();
        if (fd < 0) {
            return;
        }

        try {
            addEpollFd(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }

        TcpConnPtr connection = std::make_shared<TcpConnection>(
            fd, *this, _onFrame, _onClose, _frameMode, _maxFrameBytes,
            _onReadClosed);
        _conns.insert(std::make_pair(fd, connection));

        if (_onNewConnection) {
            try {
                _onNewConnection(connection);
            } catch (...) {
                removeConnection(fd);
                throw;
            }
        }
    }
}

void EventLoop::handleConnectionEvent(int fd, uint32_t events)
{
    const auto found = _conns.find(fd);
    if (found == _conns.end()) {
        delEpollFd(fd);
        return;
    }

    const TcpConnPtr connection = found->second;
    bool connected = true;

    try {
        if (!connection->isReadClosed() && (events & (EPOLLIN | EPOLLRDHUP))) {
            connected = connection->handleReadEvent();
        }
        if (connected && (events & EPOLLOUT)) {
            connected = connection->handleWriteEvent();
        }
        if (connected && (events & (EPOLLERR | EPOLLHUP))) {
            connected = false;
        }
    } catch (const std::exception &ex) {
        std::cerr << "connection callback failed: "
                  << ex.what() << std::endl;
        connected = false;
    } catch (...) {
        std::cerr << "connection callback failed: unknown exception"
                  << std::endl;
        connected = false;
    }

    if (!connected) {
        removeConnection(fd);
    }
}

void EventLoop::removeConnection(int fd, const TcpConnection *expected)
{
    const auto found = _conns.find(fd);
    if (expected && (found == _conns.end() || found->second.get() != expected)) {
        return;
    }
    if (found == _conns.end()) {
        delEpollFd(fd);
        return;
    }

    TcpConnPtr connection = found->second;
    delEpollFd(fd);
    _conns.erase(found);

    try {
        connection->handleCloseEvent();
    } catch (const std::exception &ex) {
        std::cerr << "close callback failed: " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "close callback failed: unknown exception" << std::endl;
    }
}

void EventLoop::enableWriting(int fd)
{
    const auto found = _conns.find(fd);
    const bool readClosed = found != _conns.end() &&
                            found->second->isReadClosed();
    updateEpollFd(fd, EPOLLOUT |
                        (readClosed ? 0 : (EPOLLIN | EPOLLRDHUP)));
}

void EventLoop::disableWriting(int fd)
{
    const auto found = _conns.find(fd);
    const bool readClosed = found != _conns.end() &&
                            found->second->isReadClosed();
    updateEpollFd(fd, readClosed ? 0 : (EPOLLIN | EPOLLRDHUP));
}

void EventLoop::setOnNewConnection(TcpConnCallback cb)
{
    _onNewConnection = std::move(cb);
}

void EventLoop::setOnFrame(TcpFrameCallback cb)
{
    _onFrame = std::move(cb);
}

void EventLoop::setOnClose(TcpConnCallback cb)
{
    _onClose = std::move(cb);
}

void EventLoop::setOnReadClosed(TcpConnCallback cb)
{
    _onReadClosed = std::move(cb);
}

bool EventLoop::isInLoopThread() const
{
    return _loopThreadId == std::this_thread::get_id();
}

void EventLoop::runInLoop(Functor cb)
{
    if (!cb) {
        return;
    }

    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb)
{
    if (!cb) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(_pendingMutex);
        _pendingFunctors.push_back(std::move(cb));
    }
    wakeup();
}

void EventLoop::wakeup()
{
    const uint64_t one = 1;
    ssize_t written;
    do {
        written = ::write(_wakeupFd, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);

    // EAGAIN 表示计数器中已有唤醒信号，无需重复写入。
    if (written < 0 && errno != EAGAIN) {
        perror("eventfd write");
    }
}

void EventLoop::handleWakeup()
{
    uint64_t value;
    ssize_t count;
    do {
        count = ::read(_wakeupFd, &value, sizeof(value));
    } while (count < 0 && errno == EINTR);

    if (count < 0 && errno != EAGAIN) {
        perror("eventfd read");
    }
}

void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> guard(_pendingMutex);
        functors.swap(_pendingFunctors);
    }

    for (auto &functor : functors) {
        try {
            functor();
        } catch (const std::exception &ex) {
            std::cerr << "pending functor failed: "
                      << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "pending functor failed: unknown exception"
                      << std::endl;
        }
    }
}

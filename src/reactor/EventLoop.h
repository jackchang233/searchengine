#ifndef _EVENTLOOP_H
#define _EVENTLOOP_H

#include "Acceptor.h"
#include "TcpConnection.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <sys/epoll.h>
#include <thread>
#include <vector>

class EventLoop {
public:
    using Functor = std::function<void()>;

    explicit EventLoop(Acceptor &acceptor,
                       FrameMode frameMode = FrameMode::Line,
                       std::size_t maxFrameBytes = 1024 * 1024);
    ~EventLoop();

    void loop();
    void unloop();
    // 可从管理线程调用；保留事件循环直到业务任务与输出排空或超时。
    void beginDrain(std::size_t timeoutMs, std::function<bool()> workDrained);

    void setOnNewConnection(TcpConnCallback cb);
    void setOnFrame(TcpFrameCallback cb);
    void setOnClose(TcpConnCallback cb);
    void setOnReadClosed(TcpConnCallback cb);

    // 工作线程可调用。若调用者就是 Reactor 线程则立即执行，
    // 否则加入待执行队列并通过 eventfd 唤醒 epoll。
    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    bool isInLoopThread() const;

    // TcpConnection 在输出缓冲状态变化时调用，只能在 Reactor 线程执行。
    void enableWriting(int fd);
    void disableWriting(int fd);

private:
    friend class TcpConnection;

    int _epfd;
    int _wakeupFd;
    std::vector<struct epoll_event> _evList;
    std::atomic<bool> _isLooping;
    std::atomic<bool> _stopRequested;
    std::thread::id _loopThreadId;
    Acceptor &_acceptor;
    std::map<int, TcpConnPtr> _conns;
    TcpConnCallback _onNewConnection;
    TcpFrameCallback _onFrame;
    TcpConnCallback _onClose;
    TcpConnCallback _onReadClosed;
    FrameMode _frameMode;
    std::size_t _maxFrameBytes;
    bool _acceptingConnections;
    bool _draining;
    std::chrono::steady_clock::time_point _drainDeadline;
    std::function<bool()> _workDrained;

    std::mutex _pendingMutex;
    std::vector<Functor> _pendingFunctors;

    int createEpollFd();
    int createWakeupFd();
    void addEpollFd(int fd);
    void updateEpollFd(int fd, uint32_t events);
    void delEpollFd(int fd);
    void waitEpollFd();
    void handleNewConnect();
    void handleConnectionEvent(int fd, uint32_t events);
    void removeConnection(int fd, const TcpConnection *expected = nullptr);
    void checkDrain();
    void closeAllConnections();

    void wakeup();
    void handleWakeup();
    void doPendingFunctors();
};

#endif

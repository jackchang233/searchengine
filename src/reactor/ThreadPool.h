#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include "TaskQueue.h"
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using std::function;
using std::thread;
using std::vector;

using Task = function<void()>;

class ThreadPool
{
public:
    ThreadPool(size_t threadNum, size_t queSize);
    ~ThreadPool();

    void start();
    void stop();

    // 返回 false 表示线程池未启动或已经停止。
    bool addTask(Task task);

    // 不等待队列空位；供 Reactor 回调提交耗时业务。
    SubmitResult tryAddTask(Task task);

private:
    Task getTask();
    void doTask();

private:
    size_t _threadNum;
    vector<thread> _threads;
    size_t _queSize;
    TaskQueue _taskQue;
    bool _isStarted;
    std::mutex _stateMutex;
};

#endif

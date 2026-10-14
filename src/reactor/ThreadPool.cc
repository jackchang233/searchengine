#include "ThreadPool.h"
#include <exception>
#include <iostream>
#include <stdexcept>

ThreadPool::ThreadPool(size_t threadNum, size_t queSize)
: _threadNum(threadNum)
, _threads()
, _queSize(queSize)
, _taskQue(_queSize)
, _isStarted(false)
, _stateMutex()
{
    if (_threadNum == 0 || _queSize == 0) {
        throw std::invalid_argument(
            "thread count and queue size must be greater than zero");
    }
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::start()
{
    std::lock_guard<std::mutex> guard(_stateMutex);
    if (_isStarted) {
        return;
    }

    _isStarted = true;
    _threads.reserve(_threadNum);
    for (size_t idx = 0; idx < _threadNum; ++idx) {
        _threads.push_back(thread(&ThreadPool::doTask, this));
    }
}

void ThreadPool::stop()
{
    {
        std::lock_guard<std::mutex> guard(_stateMutex);
        if (!_isStarted) {
            return;
        }
        _isStarted = false;
    }

    // close 会唤醒所有线程，并保证已入队任务被执行完。
    _taskQue.close();

    for (auto &th : _threads) {
        if (th.joinable()) {
            th.join();
        }
    }
    _threads.clear();
}

bool ThreadPool::addTask(Task task)
{
    {
        std::lock_guard<std::mutex> guard(_stateMutex);
        if (!_isStarted || !task) {
            return false;
        }
    }
    return _taskQue.push(std::move(task));
}

SubmitResult ThreadPool::tryAddTask(Task task)
{
    if (!task) {
        return SubmitResult::InvalidTask;
    }

    {
        std::lock_guard<std::mutex> guard(_stateMutex);
        if (!_isStarted) {
            return SubmitResult::Stopped;
        }
    }
    // stop() 可以在状态检查后关闭队列；tryPush() 再次检查并给出最终结果。
    return _taskQue.tryPush(std::move(task));
}

Task ThreadPool::getTask()
{
    return _taskQue.pop();
}

void ThreadPool::doTask()
{
    while (true) {
        Task task = getTask();
        if (!task) {
            break;
        }

        try {
            task();
        } catch (const std::exception &ex) {
            std::cerr << "thread pool task failed: "
                      << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "thread pool task failed: unknown exception"
                      << std::endl;
        }
    }
}

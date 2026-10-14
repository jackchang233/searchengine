#include "TaskQueue.h"

TaskQueue::TaskQueue(size_t capacity)
: _capacity(capacity)
, _que()
, _mutex()
, _notEmpty()
, _notFull()
, _closed(false)
{
}

TaskQueue::~TaskQueue()
{
}

bool TaskQueue::push(ElemType task)
{
    unique_lock<mutex> lock(_mutex);
    _notFull.wait(lock, [this] {
        return _closed || _que.size() < _capacity;
    });

    if (_closed) {
        return false;
    }

    _que.push(std::move(task));
    _notEmpty.notify_one();
    return true;
}

SubmitResult TaskQueue::tryPush(ElemType task)
{
    if (!task) {
        return SubmitResult::InvalidTask;
    }

    {
        unique_lock<mutex> lock(_mutex);
        if (_closed) {
            return SubmitResult::Stopped;
        }
        if (_que.size() >= _capacity) {
            return SubmitResult::QueueFull;
        }
        _que.push(std::move(task));
    }
    _notEmpty.notify_one();
    return SubmitResult::Accepted;
}

ElemType TaskQueue::pop()
{
    unique_lock<mutex> lock(_mutex);
    _notEmpty.wait(lock, [this] {
        return _closed || !_que.empty();
    });

    // close 后仍然取出已经提交的任务；队列排空后才通知工作线程退出。
    if (_que.empty()) {
        return nullptr;
    }

    ElemType task = std::move(_que.front());
    _que.pop();
    _notFull.notify_one();
    return task;
}

bool TaskQueue::empty() const
{
    unique_lock<mutex> lock(_mutex);
    return _que.empty();
}

bool TaskQueue::full() const
{
    unique_lock<mutex> lock(_mutex);
    return _que.size() >= _capacity;
}

void TaskQueue::close()
{
    unique_lock<mutex> lock(_mutex);
    _closed = true;
    _notEmpty.notify_all();
    _notFull.notify_all();
}

void TaskQueue::wakeup()
{
    close();
}

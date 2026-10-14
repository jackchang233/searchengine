#ifndef __TASKQUEUE_H__
#define __TASKQUEUE_H__

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

using std::queue;
using std::mutex;
using std::unique_lock;
using std::condition_variable;
using std::function;

using ElemType = function<void()>;

enum class SubmitResult
{
    Accepted,
    QueueFull,
    Stopped,
    InvalidTask
};

class TaskQueue
{
public:
    explicit TaskQueue(size_t capacity);
    ~TaskQueue();

    bool push(ElemType task);
    // 不等待空位；容量检查与入队在同一次加锁期间完成。
    SubmitResult tryPush(ElemType task);
    ElemType pop();

    bool empty() const;
    bool full() const;

    // 关闭队列后拒绝新任务，消费者会排空已有任务再退出。
    void close();

    // 兼容原有接口。
    void wakeup();

private:
    size_t _capacity;
    queue<ElemType> _que;
    mutable mutex _mutex;
    condition_variable _notEmpty;
    condition_variable _notFull;
    bool _closed;
};

#endif

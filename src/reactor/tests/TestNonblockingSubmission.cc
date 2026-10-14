#include "TaskQueue.h"
#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testQueueStates()
{
    TaskQueue queue(1);
    int executed = 0;
    require(queue.tryPush(ElemType()) == SubmitResult::InvalidTask,
            "empty task was accepted");
    require(queue.tryPush([&] { ++executed; }) == SubmitResult::Accepted,
            "first task was not accepted");
    require(queue.tryPush([&] { ++executed; }) == SubmitResult::QueueFull,
            "full queue did not reject immediately");
    queue.close();
    require(queue.tryPush([&] { ++executed; }) == SubmitResult::Stopped,
            "closed queue accepted a task");

    ElemType queued = queue.pop();
    require(static_cast<bool>(queued), "accepted task disappeared after close");
    queued();
    require(executed == 1 && !queue.pop(),
            "closed queue did not drain exactly one task");
}

void testConcurrentCapacityCheck()
{
    TaskQueue queue(1);
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool begin = false;
    std::atomic<int> accepted(0);
    std::atomic<int> full(0);
    std::vector<std::thread> producers;
    for (int index = 0; index < 16; ++index) {
        producers.emplace_back([&] {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                gateCondition.wait(lock, [&] { return begin; });
            }
            const SubmitResult result = queue.tryPush([] {});
            if (result == SubmitResult::Accepted) {
                ++accepted;
            } else if (result == SubmitResult::QueueFull) {
                ++full;
            }
        });
    }
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        begin = true;
    }
    gateCondition.notify_all();
    for (std::thread &producer : producers) {
        producer.join();
    }
    require(accepted == 1 && full == 15,
            "concurrent submissions exceeded queue capacity");
    queue.close();
    require(static_cast<bool>(queue.pop()) && !queue.pop(),
            "concurrent queue contents are inconsistent");
}

void testThreadPoolSaturationAndStop()
{
    ThreadPool pool(1, 1);
    require(pool.tryAddTask([] {}) == SubmitResult::Stopped,
            "unstarted pool accepted a task");
    require(pool.tryAddTask(Task()) == SubmitResult::InvalidTask,
            "pool accepted an empty task");
    pool.start();

    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool running = false;
    bool release = false;
    std::atomic<int> executed(0);
    const SubmitResult first = pool.tryAddTask([&] {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            running = true;
            gateCondition.notify_all();
            gateCondition.wait(lock, [&] { return release; });
        }
        ++executed;
    });

    bool workerStarted = false;
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        workerStarted = gateCondition.wait_for(
            lock, std::chrono::seconds(5), [&] { return running; });
    }
    if (!workerStarted) {
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            release = true;
        }
        gateCondition.notify_all();
        pool.stop();
        require(false, "worker did not start");
    }

    const SubmitResult second = pool.tryAddTask([&] { ++executed; });
    const SubmitResult third = pool.tryAddTask([&] { ++executed; });
    std::thread stopper([&] { pool.stop(); });
    SubmitResult duringStop = SubmitResult::QueueFull;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        duringStop = pool.tryAddTask([] {});
        if (duringStop == SubmitResult::Stopped) {
            break;
        }
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        release = true;
    }
    gateCondition.notify_all();
    stopper.join();

    require(first == SubmitResult::Accepted &&
                second == SubmitResult::Accepted &&
                third == SubmitResult::QueueFull,
            "pool saturation returned the wrong result");
    require(duringStop == SubmitResult::Stopped &&
                pool.tryAddTask([] {}) == SubmitResult::Stopped,
            "stopping pool accepted a task");
    require(executed == 2,
            "accepted tasks were not drained exactly once");
}

} // namespace

int main()
{
    try {
        testQueueStates();
        testConcurrentCapacityCheck();
        testThreadPoolSaturationAndStop();
    } catch (const std::exception &error) {
        std::cerr << "TestNonblockingSubmission failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

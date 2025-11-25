#ifndef FIBER_CONSUMER_H
#define FIBER_CONSUMER_H

#include <atomic>
#include <memory>
#include <thread>
// #include "concurrentqueue.h"
#include "fiber.h"
#include "lockfree/lockfree_linked_list.h"

namespace fiber {
class TimerWheel;
}
namespace fiber {
class IOManager;
}
namespace fiber {

class Scheduler;

/**
 * Fiber消费者 - 负责在独立线程中执行fiber
 * 专门用于Go语义的多线程并发调度
 * 使用lock-free ring buffer实现任务队列
 */
class FiberConsumer {
public:
    friend Scheduler;
    FiberConsumer(int id, Scheduler *scheduler);
    ~FiberConsumer();

    FiberConsumer(const FiberConsumer &) = delete;
    FiberConsumer &operator=(const FiberConsumer &) = delete;
    FiberConsumer(FiberConsumer &&) = delete;
    FiberConsumer &operator=(FiberConsumer &&) = delete;

    int id() const;
    void start();
    void stop();
    bool schedule(Fiber::ptr fiber);
    size_t getQueueSize() const;
    auto popTask() -> std::optional<Fiber::ptr>;

private:
    static constexpr size_t QUEUE_SIZE = 1024; // 队列容量
    int id_;

    Scheduler *scheduler_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // 使用lock-free队列存储Fiber::ptr
    std::unique_ptr<LockFreeLinkedList<Fiber::ptr>> queue_;
    // std::unique_ptr<moodycamel::ConcurrentQueue<Fiber::ptr>> queue_;
    std::unique_ptr<IOManager> io_manager_;
    std::unique_ptr<TimerWheel> timer_wheel_;

    void consumerLoop();
    void processTask();
};

} // namespace fiber

#endif // FIBER_CONSUMER_H

#ifndef FIBER_CONSUMER_H
#define FIBER_CONSUMER_H

#include "fiber.h"
#include "lockfree_queue.h"
#include <thread>
#include <atomic>
#include <memory>

namespace fiber {

class Scheduler;

/**
 * Fiber消费者 - 负责在独立线程中执行fiber
 * 专门用于Go语义的多线程并发调度
 * 使用lock-free ring buffer实现任务队列
 */
class FiberConsumer {
public:
    FiberConsumer(int id, Scheduler* scheduler);
    ~FiberConsumer();
    
    FiberConsumer(const FiberConsumer&) = delete;
    FiberConsumer& operator=(const FiberConsumer&) = delete;
    FiberConsumer(FiberConsumer&&) = delete;
    FiberConsumer& operator=(FiberConsumer&&) = delete;
    
    void start();
    void stop();
    void schedule(Fiber::ptr fiber);
    size_t getQueueSize() const;
    
private:
    static constexpr size_t QUEUE_SIZE = 1024;  // 队列容量
    
    int id_;
    Scheduler* scheduler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    
    // 使用lock-free队列存储Fiber::ptr
    std::unique_ptr<LockFreeQueue<std::shared_ptr<Fiber>>> queue_;
    
    void consumerLoop();
    void processTask();
};

} // namespace fiber

#endif // FIBER_CONSUMER_H
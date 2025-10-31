#ifndef FIBER_CONSUMER_H
#define FIBER_CONSUMER_H

#include "fiber.h"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace fiber {

class Scheduler;

/**
 * Fiber消费者 - 负责在独立线程中执行fiber
 * 专门用于Go语义的多线程并发调度
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
    int id_;
    Scheduler* scheduler_;
    std::thread thread_;
    
    std::queue<Fiber::ptr> local_queue_;
    mutable std::mutex local_mutex_;
    std::condition_variable local_cv_;
    std::atomic<bool> running_{false};
    
    void consumerLoop();
    void processTask();
};

} // namespace fiber

#endif // FIBER_CONSUMER_H
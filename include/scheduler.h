#ifndef FIBER_SCHEDULER_H
#define FIBER_SCHEDULER_H

#include "fiber.h"
#include <queue>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace fiber {

enum class SchedulerState {
    STOPPED,
    RUNNING,
    STOPPING
};

enum class SchedulerMode {
    SINGLE_THREAD,  // 单线程调度（Lua语义）
    MULTI_THREAD    // 多线程调度（Go语义）
};

class FiberConsumer;

class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;
    
    Scheduler(SchedulerMode mode = SchedulerMode::SINGLE_THREAD);
    ~Scheduler();
    
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;
    
    void init(int worker_count = 1);  // 支持指定工作线程数
    void stop();
    bool isRunning() const;
    SchedulerState getState() const;
    SchedulerMode getMode() const { return mode_; }
    
    void schedule(Fiber::ptr fiber);
    void start();
    void runOnce();
    bool hasReadyFibers() const;
    
    static Scheduler::ptr GetScheduler();
    static Scheduler::ptr GetOrCreateScheduler(SchedulerMode mode = SchedulerMode::SINGLE_THREAD);
    static Scheduler::ptr GetOrCreateMultiThreadScheduler();  // 获取多线程调度器
    static void SetScheduler(Scheduler::ptr scheduler);
    
    static Fiber::ptr GetMainFiber();
    
    // 多线程调度专用接口
    void scheduleImmediate(Fiber::ptr fiber);  // 立即调度（多线程模式）
    int getWorkerCount() const;
    
private:
    SchedulerMode mode_;
    SchedulerState state_;
    
    // 单线程模式成员（仅为Lua语义提供main_fiber_）
    std::queue<Fiber::ptr> ready_queue_;
    std::vector<Fiber::ptr> all_fibers_;
    Fiber::ptr scheduler_fiber_;
    Fiber::ptr current_fiber_;
    Fiber::ptr main_fiber_;
    
    // 多线程模式成员
    std::vector<std::unique_ptr<FiberConsumer>> consumers_;
    std::queue<Fiber::ptr> global_queue_;
    mutable std::mutex global_mutex_;
    std::condition_variable global_cv_;
    
    static thread_local Scheduler::ptr thread_scheduler_;
    
    // 单线程调度方法
    void cleanup_finished_fibers();
    Fiber::ptr next_ready_fiber();
    
    // 多线程调度方法
    FiberConsumer* selectConsumer();
    void startConsumers(int count);
    void stopConsumers();
};

} // namespace fiber

#endif // FIBER_SCHEDULER_H
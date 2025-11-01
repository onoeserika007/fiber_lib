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

class FiberConsumer;

class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;

    friend class Fiber;
    friend class WaitQueue;

    static Scheduler &GetScheduler();  // 获取多线程调度器
    ~Scheduler();
    
    void init(int worker_count = 1);  // 支持指定工作线程数
    void run();
    void stop();
    bool isRunning() const;
    SchedulerState getState() const;

    bool hasReadyFibers() const;

    // 多线程调度专用接口
    void scheduleImmediate(Fiber::ptr fiber);  // 立即调度（多线程模式）
    int getWorkerCount() const;
    
private:
    SchedulerState state_;
    
    // 单线程模式成员（仅为Lua语义提供main_fiber_）
    std::queue<Fiber::ptr> ready_queue_;
    std::vector<Fiber::ptr> all_fibers_;
    Fiber::ptr main_fiber_;
    
    // lock-free consumers
    std::vector<std::unique_ptr<FiberConsumer>> consumers_;

    // 多线程调度方法
    FiberConsumer* selectConsumer();
    void startConsumers(int count);
    void stopConsumers();

    Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;
};

} // namespace fiber

// =========================
// Fiber Main Macro (协程化main函数)
// =========================

/**
 * @brief 将main函数协程化的宏
 *
 * 使用方式：
 * FIBER_MAIN() {
 *     // 这里的代码运行在协程中，可以使用Fiber::yield()、WaitGroup等
 *     LOG_INFO("Hello from fiber");
 *     return 0;
 * }
 *
 * 效果：整个main函数内容在多线程调度器的协程中运行
 */
#define FIBER_MAIN()                                          \
int __fiber_main_impl();                                      \
int main(int argc, char** argv) {                             \
    (void)argc; (void)argv;                                   \
    std::atomic<int> exit_code{0};                           \
    fiber::Fiber::go([&exit_code]() {                        \
        int ret = __fiber_main_impl();                        \
        exit_code.store(ret);                                 \
        fiber::Scheduler::GetScheduler().stop();            \
    });                                                       \
    fiber::Scheduler::GetScheduler().run();                 \
    return exit_code.load();                                  \
}                                                             \
int __fiber_main_impl()

#endif // FIBER_SCHEDULER_H
#ifndef FIBER_FIBER_H
#define FIBER_FIBER_H

#include <stdint.h>
#include <functional>
#include <memory>
#include <thread>
#include <chrono>

namespace fiber {

class Context;
class Scheduler;

enum class FiberState {
    READY,
    RUNNING,
    SUSPENDED,
    DONE
};

// must be public inheritance
class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    friend class Scheduler;
    using ptr = std::shared_ptr<Fiber>;
    using FiberFunction = std::function<void()>;
    
    enum class RunMode {
        MANUAL,      // Lua语义：手动控制
        SCHEDULED    // Go语义：调度器管理  
    };
    
    void resume();
    static void yield();
    
    FiberState getState() const;
    uint64_t getId() const;
    
    // =========================
    // Lua语义接口 (手动控制)
    // =========================
    
    /**
     * 创建fiber但不启动 (Lua语义)
     * @param func 要执行的函数
     * @return fiber指针
     */
    static Fiber::ptr create(FiberFunction func) {
        auto fiber = std::shared_ptr<Fiber>(new Fiber(std::move(func)));
        fiber->Init();
        fiber->setRunMode(RunMode::MANUAL);
        return fiber;
    }
    
    /**
     * 检查fiber是否已完成
     * @param fiber 要检查的fiber
     * @return true表示已完成
     */
    bool isDone() const {
        return getState() == FiberState::DONE;
    }
    
    // =========================  
    // Go语义接口 (自动调度)
    // =========================
    
    /**
     * 创建并立即执行goroutine (Go语义)
     * 立即在多线程中开始执行
     * @param func 要执行的函数
     */
    static void go(FiberFunction func);
    
    /**
     * 等待协程完成（给正在执行的协程一些时间）
     */
    static void waitAll() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    /**
     * 获取工作线程数量
     */
    static int getWorkerCount();
    
    /**
     * @brief 获取当前协程的shared_ptr（用于WaitQueue等同步机制）
     * @return 当前协程的shared_ptr，如果不在协程中则返回nullptr
     */
    static ptr GetCurrentFiberPtr();
    
    /**
     * @brief 设置当前协程的weak_ptr（供调度器调用）
     * @param fiber 当前协程的shared_ptr
     */
    static void SetCurrentFiberPtr(const ptr& fiber);

    ~Fiber();

private:
    explicit Fiber(FiberFunction func);

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = default;
    Fiber& operator=(Fiber&&) = default;

    void Init();

    static void fiberEntry();

    // RunMode管理
    void setRunMode(RunMode mode) { run_mode_ = mode; }
    RunMode getRunMode() const { return run_mode_; }

    uint64_t id_;
    FiberState state_;
    FiberFunction func_;
    std::unique_ptr<Context> context_;
    RunMode run_mode_;
    
    static uint64_t generateId();
    static thread_local Fiber* current_fiber_;
    static thread_local std::weak_ptr<Fiber> current_fiber_weak_;
};

} // namespace fiber

#endif // FIBER_FIBER_H
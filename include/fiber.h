#ifndef FIBER_FIBER_H
#define FIBER_FIBER_H

#include <stdint.h>
#include <functional>
#include <memory>

namespace fiber {

// 前向声明
class Context;

enum class FiberState {
    READY,
    RUNNING,
    SUSPENDED,
    DONE
};

class Fiber {
public:
    using ptr = std::shared_ptr<Fiber>;
    using FiberFunction = std::function<void()>;

    // 构造函数
    explicit Fiber(FiberFunction func);
    
    // 析构函数
    ~Fiber();
    
    // 禁止拷贝
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    
    // 允许移动
    Fiber(Fiber&&) = default;
    Fiber& operator=(Fiber&&) = default;
    
    // 协程操作接口
    void resume();
    static void yield();
    
    // 获取协程状态
    FiberState getState() const;
    
    // 获取协程ID
    uint64_t getId() const;

    // 静态方法
    static Fiber::ptr GetCurrentFiber();
    static void YieldCurrent();

    // 协程入口函数
    static void fiberEntry();

private:
    // 协程ID
    uint64_t id_;
    
    // 协程状态
    FiberState state_;
    
    // 协程执行函数
    FiberFunction func_;
    
    // 协程上下文
    std::unique_ptr<Context> context_;
    
    // 初始化ID生成器
    static uint64_t generateId();
    
    // 主协程
    static Fiber::ptr main_fiber_;
    
    // 当前运行的协程
    static thread_local Fiber* current_fiber_;
};

} // namespace fiber

#endif // FIBER_FIBER_H
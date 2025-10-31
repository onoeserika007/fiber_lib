#include "fiber.h"
#include "context.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdlib.h>

namespace fiber {

// 静态成员初始化
thread_local Fiber* Fiber::current_fiber_ = nullptr;
Fiber::ptr Fiber::main_fiber_ = nullptr;
std::atomic<uint64_t> fiber_id_counter{0};

// 生成协程ID
uint64_t Fiber::generateId() {
    return ++fiber_id_counter;
}

// 构造函数
Fiber::Fiber(FiberFunction func) 
    : id_(generateId()), 
      state_(FiberState::READY),
      func_(std::move(func)) {
      
    // 为主协程特殊处理
    if (!func_) {
        current_fiber_ = this;
        state_ = FiberState::RUNNING;
        // 主协程也需要上下文用于切换
        context_.reset(ContextFactory::createContext());
        return;
    }
    
    // 创建上下文
    context_.reset(ContextFactory::createContext());
    
    // 初始化上下文
    context_->initialize(&Fiber::fiberEntry);
    
    std::cout << "Fiber created with ID: " << id_ << std::endl;
}

// 析构函数
Fiber::~Fiber() {
    // Context会自动释放栈空间
    std::cout << "Fiber destroyed with ID: " << id_ << std::endl;
}

// 恢复协程执行
void Fiber::resume() {
    assert(state_ != FiberState::DONE && "Cannot resume a finished fiber");
    assert(context_ && "Fiber context is null");
    
    if (state_ == FiberState::READY || state_ == FiberState::SUSPENDED) {
        Fiber* old_fiber = current_fiber_;
        
        std::cout << "Resuming fiber " << id_ << std::endl;
        
        // 从当前协程切换到目标协程
        if (old_fiber && old_fiber->context_) {
            // 设置目标协程为当前协程并切换
            current_fiber_ = this;
            state_ = FiberState::RUNNING;
            old_fiber->context_->switchTo(context_.get());
        } else {
            // 开发阶段：如果没有 old_fiber，说明调用逻辑有问题
            assert(false && "No valid old fiber context for switching");
        }
    }
}

// 挂起当前协程
void Fiber::yield() {
    assert(current_fiber_ && "No current fiber");
    assert(main_fiber_ && "No main fiber");
    
    Fiber* current = current_fiber_;
    current->state_ = FiberState::SUSPENDED;
    
    std::cout << "Yielding fiber " << current->id_ << std::endl;
    
    // 切换回主协程
    current_fiber_ = main_fiber_.get();
    main_fiber_->state_ = FiberState::RUNNING;
    
    assert(main_fiber_->context_ && "Main fiber context is null");
    assert(current->context_ && "Current fiber context is null");
    
    current->context_->switchTo(main_fiber_->context_.get());
}

// 获取协程状态
FiberState Fiber::getState() const {
    return state_;
}

// 获取协程ID
uint64_t Fiber::getId() const {
    return id_;
}

// 获取当前协程
Fiber::ptr Fiber::GetCurrentFiber() {
    if (!main_fiber_) {
        // 创建主协程
        main_fiber_ = std::make_shared<Fiber>(FiberFunction{});
    }
    if (!current_fiber_) {
        current_fiber_ = main_fiber_.get();
    }
    return std::shared_ptr<Fiber>(current_fiber_, [](Fiber*){});
}

// 挂起当前协程
void Fiber::YieldCurrent() {
    yield();
}

// 协程入口函数
void Fiber::fiberEntry() {
    assert(current_fiber_ && "No current fiber in fiberEntry");
    
    // 执行协程函数
    if (current_fiber_->func_) {
        current_fiber_->func_();
    }
    
    // 标记协程完成
    current_fiber_->state_ = FiberState::DONE;
    
    // 自动yield回主协程
    std::cout << "Fiber " << current_fiber_->id_ << " finished" << std::endl;
    yield();
}

} // namespace fiber
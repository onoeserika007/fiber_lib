#include "context.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace fiber {

// UContext实现
UContext::UContext() : stack_(nullptr), stack_size_(DEFAULT_STACK_SIZE) {
    // 初始化上下文
    if (getcontext(&context_) == -1) {
        throw std::runtime_error("getcontext failed");
    }
}

UContext::~UContext() {
    if (stack_) {
        free(stack_);
    }
}

void UContext::switchTo(Context* to) {
    if (UContext* uctx = dynamic_cast<UContext*>(to)) {
        std::cout << "Switching context (ucontext implementation)" << std::endl;
        if (swapcontext(&context_, &uctx->context_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }
    }
}

void UContext::initialize(void (*func)()) {
    if (!stack_) {
        stack_ = malloc(stack_size_);
        if (!stack_) {
            throw std::runtime_error("Failed to allocate stack");
        }
    }
    
    context_.uc_stack.ss_sp = stack_;
    context_.uc_stack.ss_size = stack_size_;
    context_.uc_link = nullptr;
    makecontext(&context_, func, 0);
}

ucontext_t* UContext::getUContext() {
    return &context_;
}

// ContextFactory实现
Context* ContextFactory::createContext() {
    // 目前返回ucontext实现，后续可以替换为更高效的实现
    return new UContext();
}

void ContextFactory::destroyContext(Context* context) {
    delete context;
}

} // namespace fiber
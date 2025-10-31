#ifndef FIBER_CONTEXT_H
#define FIBER_CONTEXT_H

#include <cstddef>
#include <ucontext.h>

namespace fiber {

// 上下文接口，负责上下文的保存和切换
class Context {
public:
    virtual ~Context() = default;
    
    // Save current context to [this], and switch context to [to]
    virtual void switchTo(Context* to) = 0;
    
    // 初始化上下文，设置协程函数
    virtual void initialize(void (*func)()) = 0;
    
protected:
    Context() = default;
};

// 有栈协程上下文
class StackfulContext : public Context {
protected:
    StackfulContext() = default;
};

// 基于ucontext的有栈实现
class UContext : public StackfulContext {
public:
    UContext();
    ~UContext();
    
    // 禁止拷贝
    UContext(const UContext&) = delete;
    UContext& operator=(const UContext&) = delete;
    
    // 允许移动
    UContext(UContext&&) = default;
    UContext& operator=(UContext&&) = default;
    
    void switchTo(Context* to) override;
    void initialize(void (*func)()) override;
    
    // 获取ucontext结构
    ucontext_t* getUContext();
    
private:
    ucontext_t context_;
    void* stack_;
    size_t stack_size_;
    
    // 默认栈大小 128KB
    static const size_t DEFAULT_STACK_SIZE = 128 * 1024;
};

// 上下文工厂，用于创建具体类型的上下文
class ContextFactory {
public:
    // 创建上下文实例 (目前使用ucontext实现)
    static Context* createContext();
    
    // 销毁上下文实例
    static void destroyContext(Context* context);
};

} // namespace fiber

#endif // FIBER_CONTEXT_H
#ifndef FIBER_CONTEXT_H
#define FIBER_CONTEXT_H

#include <cstddef>
#include <ucontext.h>

namespace fiber {

class Context {
public:
    virtual ~Context() = default;
    virtual void switchTo(Context* to) = 0;
    virtual void initialize(void (*func)()) = 0;
    
protected:
    Context() = default;
};

class StackfulContext : public Context {
protected:
    StackfulContext() = default;
};

class UContext : public StackfulContext {
public:
    UContext();
    ~UContext();
    
    UContext(const UContext&) = delete;
    UContext& operator=(const UContext&) = delete;
    UContext(UContext&&) = default;
    UContext& operator=(UContext&&) = default;
    
    void switchTo(Context* to) override;
    void initialize(void (*func)()) override;
    ucontext_t* getUContext();
    
private:
    ucontext_t context_;
    void* stack_;
    size_t stack_size_;
    
    static const size_t DEFAULT_STACK_SIZE = 128 * 1024;
};

class ContextFactory {
public:
    static Context* createContext();
    static void destroyContext(Context* context);
};

} // namespace fiber

#endif // FIBER_CONTEXT_H
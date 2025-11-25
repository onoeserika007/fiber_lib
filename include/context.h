#ifndef FIBER_CONTEXT_H
#define FIBER_CONTEXT_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <sanitizer/asan_interface.h>
#include <sys/mman.h>
#include <ucontext.h>

namespace fiber {

class Context {
public:
    virtual ~Context() = default;
    virtual void switchTo(Context *to) = 0;
    virtual void initialize(void (*func)()) = 0;

protected:
    Context() = default;
};

class StackfulContext : public Context {
public:
    static const size_t DEFAULT_STACK_SIZE = 256 * 1024;

protected:
    StackfulContext() = default;

    size_t stack_size_;
    void *stack_;
};

/**
 * UContext
 */
class UContext : public StackfulContext {
public:
    static inline std::unique_ptr<Context> createContext(size_t stack_size = DEFAULT_STACK_SIZE) {
        return std::unique_ptr<Context>(new UContext(stack_size));
    }
    ~UContext();

    // 禁止拷贝
    UContext(const UContext &) = delete;
    UContext &operator=(const UContext &) = delete;

    // 支持移动
    UContext(UContext &&other) noexcept { *this = std::move(other); }

    UContext &operator=(UContext &&other) noexcept {
        if (this != &other) {
            if (mapping_base_) {
#ifdef ADDRESS_SANITIZER
                __asan_poison_memory_region(stack_ptr_, stack_size_);
#endif
                munmap(mapping_base_, total_size_);
            }

            mapping_base_ = other.mapping_base_;
            stack_ = other.stack_;
            stack_size_ = other.stack_size_;

            other.mapping_base_ = nullptr;
            other.stack_ = nullptr;
            other.stack_size_ = 0;
        }
        return *this;
    }

    void switchTo(Context *to) override;
    void initialize(void (*func)()) override;
    ucontext_t *getUContext();

private:
    UContext(size_t st_sz = DEFAULT_STACK_SIZE);

    ucontext_t context_;
    size_t total_size_;
    void *mapping_base_; // 实际 mmap 起始地址

    static size_t get_page_size() { return sysconf(_SC_PAGESIZE); }

    static size_t align_to_page(size_t sz) {
        size_t page_size = sysconf(_SC_PAGESIZE);
        return (sz + page_size - 1) & ~(page_size - 1); // 向上对齐到页
    }
};

/**
 * AsmContext
 */

using coctx_pfn_t = void *(*) (void *s, void *s2);
struct coctx_param_t {
    const void *s1;
    const void *s2;
};

struct coctx_t {
#if defined(__i386__)
    void *regs[8] = {nullptr};
#else
    void *regs[14] = {nullptr};
#endif
    size_t ss_size{};
    char *ss_sp{};
    // std::atomic<bool> can_enter{true};
};

int coctx_init(coctx_t *ctx);
int coctx_make(coctx_t *ctx, coctx_pfn_t pfn, const void *s, const void *s1);

class AsmContext : public StackfulContext {
public:
    ~AsmContext() override;

    // 禁止拷贝
    AsmContext(const AsmContext &) = delete;
    AsmContext &operator=(const AsmContext &) = delete;

    // 禁止移动
    AsmContext(AsmContext &&) noexcept = delete;
    AsmContext operator=(AsmContext &&) noexcept = delete;

    static inline std::unique_ptr<Context> createContext(size_t stack_size = DEFAULT_STACK_SIZE) {
        return std::unique_ptr<Context>(new AsmContext(stack_size));
    }

    void switchTo(Context *to) override;
    void initialize(void (*func)()) override;

    static int fiber_trampoline(void * /*s*/, void * /*s1*/);

private:
    AsmContext(size_t stack_size);

    coctx_t context_{};
};


} // namespace fiber

#endif // FIBER_CONTEXT_H

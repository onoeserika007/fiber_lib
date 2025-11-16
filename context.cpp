#include "context.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

#include "fiber.h"
#include "logger.h"

namespace fiber {

// ============================== UContext Begin ================================== //

UContext::UContext(size_t st_sz) :context_() {
    stack_size_ = align_to_page(st_sz);
    stack_ = nullptr;
    total_size_ = stack_size_ + get_page_size();
    std::memset(&context_, 0, sizeof(context_));
    if (getcontext(&context_) == -1) {
        throw std::runtime_error("getcontext failed");
    }
}

UContext::~UContext() {
    if (mapping_base_) {
#ifdef ADDRESS_SANITIZER
        __asan_poison_memory_region(stack_ptr_, stack_size_);
#endif
        munmap(mapping_base_, total_size_);
    }
}

void UContext::switchTo(Context* to) {
    if (UContext* uctx = dynamic_cast<UContext*>(to)) {
        if (swapcontext(&context_, &uctx->context_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }
    }
}

void UContext::initialize(void (*func)()) {
    if (!stack_ || !mapping_base_) {
        mapping_base_ = mmap(
            nullptr,
            total_size_,
            PROT_NONE,  // 整个区域初始不可访问
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
            -1, 0
        );
        if (mapping_base_ == MAP_FAILED) {
            throw std::runtime_error("mmap failed");
        }

        // 设置栈区域为可读写
        stack_ = static_cast<char*>(mapping_base_) + get_page_size();
        if (mprotect(stack_, stack_size_, PROT_READ | PROT_WRITE) == -1) {
            munmap(mapping_base_, total_size_);
            throw std::runtime_error("mprotect failed");
        }

#ifdef ADDRESS_SANITIZER
        __asan_unpoison_memory_region(stack_ptr_, stack_size_);
#endif
    }

    context_.uc_stack.ss_sp = stack_;
    context_.uc_stack.ss_size = stack_size_;
    context_.uc_link = nullptr;
    makecontext(&context_, func, 0);
}

ucontext_t *UContext::getUContext() { return &context_; }

// ============================== UContext End ================================== //

// ============================== Asm Context Begin ============================= //

#define ESP 0
#define EIP 1
#define EAX 2
#define ECX 3
// -----------
#define RSP 0
#define RIP 1
#define RBX 2
#define RDI 3
#define RSI 4

#define RBP 5
#define R12 6
#define R13 7
#define R14 8
#define R15 9
#define RDX 10
#define RCX 11
#define R8 12
#define R9 13

//----- --------
// 32 bit
// | regs[0]: ret |
// | regs[1]: ebx |
// | regs[2]: ecx |
// | regs[3]: edx |
// | regs[4]: edi |
// | regs[5]: esi |
// | regs[6]: ebp |
// | regs[7]: eax |  = esp
enum {
    kEIP = 0,
    kEBP = 6,
    kESP = 7,
};

//-------------
// 64 bit
// low | regs[0]: r15 |
//    | regs[1]: r14 |
//    | regs[2]: r13 |
//    | regs[3]: r12 |
//    | regs[4]: r9  |
//    | regs[5]: r8  |
//    | regs[6]: rbp |
//    | regs[7]: rdi |
//    | regs[8]: rsi |
//    | regs[9]: ret |  //ret func addr
//    | regs[10]: rdx |
//    | regs[11]: rcx |
//    | regs[12]: rbx |
// hig | regs[13]: rsp |

enum {
    kRDI = 7,
    kRSI = 8,
    kRETAddr = 9,
    kRSP = 13,
};

// 64 bit
extern "C" {
    extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap");
};

#if defined(__i386__)
int coctx_init(coctx_t* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return 0;
}
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
    // make room for coctx_param
    char* sp = ctx->ss_sp + ctx->ss_size - sizeof(coctx_param_t);
    sp = (char*)((unsigned long)sp & -16L);

    coctx_param_t* param = (coctx_param_t*)sp;
    void** ret_addr = (void**)(sp - sizeof(void*) * 2);
    *ret_addr = (void*)pfn;
    param->s1 = s;
    param->s2 = s1;

    memset(ctx->regs, 0, sizeof(ctx->regs));

    ctx->regs[kESP] = (char*)(sp) - sizeof(void*) * 2;
    return 0;
}
#elif defined(__x86_64__)
int coctx_make(coctx_t *ctx, coctx_pfn_t pfn, const void *s, const void *s1) {
    char *sp = ctx->ss_sp + ctx->ss_size - sizeof(void *);
    sp = (char *) ((unsigned long) sp & -16LL);

    memset(ctx->regs, 0, sizeof(ctx->regs));
    void **ret_addr = (void **) (sp);
    *ret_addr = (void *) pfn;

    ctx->regs[kRSP] = sp;

    ctx->regs[kRETAddr] = (char *) pfn;

    ctx->regs[kRDI] = (char *) s;
    ctx->regs[kRSI] = (char *) s1;
    return 0;
}

int coctx_init(coctx_t* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return 0;
}

#endif

AsmContext::AsmContext(size_t stack_size) {
    stack_ = nullptr;
    stack_size_ = stack_size;
    memset(&context_, 0, sizeof(context_));
}

AsmContext::~AsmContext() {
    if (stack_) {
        std::free(stack_);
        stack_ = nullptr;
    }
}

void AsmContext::initialize(void (*func)()) {
    if (!stack_) {
        stack_ = std::malloc(stack_size_);
        if (!stack_) {
            throw std::runtime_error("Failed to allocate stack");
        }
        memset(stack_, 0, stack_size_);
    }

    context_.ss_sp = static_cast<char *>(stack_);
    context_.ss_size = stack_size_;
    //
    // coctx_make(&context_, reinterpret_cast<coctx_pfn_t>(&fiber_trampoline), nullptr, nullptr);

    // 1. 计算栈顶 (16字节对齐)
    void *sp = reinterpret_cast<char *>(stack_) + stack_size_ - sizeof(void *);
    sp = reinterpret_cast<char *>(reinterpret_cast<uintptr_t>(sp) & -16LL);

    // 2. 设置返回地址 (压栈)
    auto func_addr = reinterpret_cast<void *>(&fiber_trampoline);
    *reinterpret_cast<void **>(sp) = func_addr;

    // 3. 初始化寄存器状态 (完全复制 libco 逻辑)
    memset(context_.regs, 0, sizeof(context_.regs)); // 清零所有寄存器

    context_.regs[kRSP] = sp; // 设置 RSP 指向栈顶
    context_.regs[kRETAddr] = reinterpret_cast<void *>(&fiber_trampoline); // 设置返回地址
    context_.regs[kRDI] = nullptr; // 无参数传递 (libco 用于传递 s1)
    context_.regs[kRSI] = nullptr; // 无参数传递 (libco 用于传递 s2)
}

int AsmContext::fiber_trampoline(void *, void *) {
    Fiber::fiberEntry();
    return 0;
}

void AsmContext::switchTo(Context *to) {
    if (AsmContext* ctx = dynamic_cast<AsmContext*>(to)) {
        // save to this, and switch to ctx
        // LOG_INFO("Jumping to {}", ctx->context_.regs[kRETAddr]);
        assert(ctx->context_.regs[kRETAddr] != 0 && "Invalid ret addr");
        coctx_swap(&context_, &ctx->context_);
    }
}


// ============================== Asm Context Begin ============================= //

} // namespace fiber
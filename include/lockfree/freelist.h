//
// Created by inory on 11/15/25.
//

#ifndef FREELIST_H
#define FREELIST_H

#include <atomic>
#include <cstring>
#include <memory>
#include "lockfree/tagged_node_ptr.h"

namespace fiber {

constexpr size_t cacheline_bytes = 64;

template<typename T, typename Alloc = std::allocator<T>>
class alignas(cacheline_bytes) FreeList : Alloc {
    struct FreeListNode {
        TaggedPtr<FreeListNode> next;
    };

    using TaggedHandlePtr = TaggedPtr<FreeListNode>;

public:
    typedef T *index_t;
    typedef TaggedPtr<T> tagged_node_handle;

    template<typename Allocator>
    FreeList(Allocator const &alloc, std::size_t n = 0) : Alloc(alloc), pool_(TaggedHandlePtr(nullptr)) {
        for (std::size_t i = 0; i != n; ++i) {
            T *node = Alloc::allocate(1);
            std::memset((void *) node, 0, sizeof(T));
            destruct<false>(node);
        }
    }

    template<bool ThreadSafe>
    void reserve(std::size_t count) {
        for (std::size_t i = 0; i != count; ++i) {
            T *node = Alloc::allocate(1);
            std::memset((void *) node, 0, sizeof(T));
            deallocate<ThreadSafe>(node);
        }
    }

    template<bool ThreadSafe, bool Bounded>
    T *construct(void) {
        T *node = allocate<ThreadSafe, Bounded>();
        if (node) {
            new (node) T();
        }
        return node;
    }

    template<bool ThreadSafe, bool Bounded, typename ArgumentType>
    T *construct(const ArgumentType &arg) {
        T *node = allocate<ThreadSafe, Bounded>();
        if (node) {
            new (node) T(arg);
        }
        return node;
    }

    template<bool ThreadSafe, bool Bounded, typename ArgumentType>
    T *construct(ArgumentType &&arg) {
        T *node = allocate<ThreadSafe, Bounded>();
        if (node) {
            new (node) T(std::forward<ArgumentType>(arg));
        }
        return node;
    }

    template<bool ThreadSafe, bool Bounded, typename ArgumentType1, typename ArgumentType2>
    T *construct(ArgumentType1 &&arg1, ArgumentType2 &&arg2) {
        T *node = allocate<ThreadSafe, Bounded>();
        if (node) {
            new (node) T(arg1, arg2);
        }
        return node;
    }

    template<bool ThreadSafe>
    void destruct(tagged_node_handle const &tagged_ptr) {
        T *n = tagged_ptr.get_ptr();
        n->~T();
        deallocate<ThreadSafe>(n);
    }

    template<bool ThreadSafe>
    void destruct(T *n) {
        n->~T();
        deallocate<ThreadSafe>(n);
    }

    ~FreeList(void) {
        TaggedHandlePtr current = pool_.load();

        while (current) {
            FreeListNode *current_ptr = current.get_ptr();
            if (current_ptr)
                current = current_ptr->next;
            Alloc::deallocate((T *) current_ptr, 1);
        }
    }

    bool is_lock_free(void) const { return pool_.is_lock_free(); }

    T *get_handle(T *pointer) const { return pointer; }

    T *get_handle(tagged_node_handle const &handle) const { return get_pointer(handle); }

    T *get_pointer(tagged_node_handle const &tptr) const { return tptr.get_ptr(); }

    T *get_pointer(T *pointer) const { return pointer; }

    T *null_handle(void) const { return NULL; }

protected: // allow use from subclasses
    template<bool ThreadSafe, bool Bounded>
    T *allocate(void) {
        if (ThreadSafe) {
            return allocate_impl<Bounded>();
        } else
            return allocate_impl_unsafe<Bounded>();
    }

private:
    template<bool Bounded>
    T *allocate_impl(void) {
        TaggedHandlePtr old_pool = pool_.load(std::memory_order_consume);

        for (;;) {
            if (!old_pool.get_ptr()) {
                if (!Bounded) {
                    T *ptr = Alloc::allocate(1);
                    std::memset((void *) ptr, 0, sizeof(T));
                    return ptr;
                } else
                    return 0;
            }

            FreeListNode *new_pool_ptr = old_pool->next.get_ptr();
            TaggedHandlePtr new_pool(new_pool_ptr, old_pool.get_next_tag());

            if (pool_.compare_exchange_weak(old_pool, new_pool)) {
                void *ptr = old_pool.get_ptr();
                return reinterpret_cast<T *>(ptr);
            }
        }
    }

    template<bool Bounded>
    T *allocate_impl_unsafe(void) {
        TaggedHandlePtr old_pool = pool_.load(std::memory_order_relaxed);

        if (!old_pool.get_ptr()) {
            if (!Bounded) {
                T *ptr = Alloc::allocate(1);
                std::memset((void *) ptr, 0, sizeof(T));
                return ptr;
            } else
                return 0;
        }

        FreeListNode *new_pool_ptr = old_pool->next.get_ptr();
        TaggedHandlePtr new_pool(new_pool_ptr, old_pool.get_next_tag());

        pool_.store(new_pool, std::memory_order_relaxed);
        void *ptr = old_pool.get_ptr();
        return reinterpret_cast<T *>(ptr);
    }

protected:
    template<bool ThreadSafe>
    void deallocate(T *n) {
        if (ThreadSafe)
            deallocate_impl(n);
        else
            deallocate_impl_unsafe(n);
    }

private:
    void deallocate_impl(T *n) {
        void *node = n;
        TaggedHandlePtr old_pool = pool_.load(std::memory_order_consume);
        FreeListNode *new_pool_ptr = reinterpret_cast<FreeListNode *>(node);

        for (;;) {
            TaggedHandlePtr new_pool(new_pool_ptr, old_pool.get_tag());
            new_pool->next.set_ptr(old_pool.get_ptr());

            if (pool_.compare_exchange_weak(old_pool, new_pool))
                return;
        }
    }

    void deallocate_impl_unsafe(T *n) {
        void *node = n;
        TaggedHandlePtr old_pool = pool_.load(std::memory_order_relaxed);
        FreeListNode *new_pool_ptr = reinterpret_cast<FreeListNode *>(node);

        TaggedHandlePtr new_pool(new_pool_ptr, old_pool.get_tag());
        new_pool->next.set_ptr(old_pool.get_ptr());

        pool_.store(new_pool, std::memory_order_relaxed);
    }

    std::atomic<TaggedHandlePtr> pool_;
};
} // namespace fiber

#endif // FREELIST_H

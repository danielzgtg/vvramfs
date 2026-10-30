#include <cstdlib>
#include <dlfcn.h>

#include "free_interposer.hpp"

using namespace std;

void *free_interposer_ignore_start{};
atomic_uint free_interposer_locks_head{};
atomic_bool free_interposer_locks[FREE_INTERPOSER_BUF_COUNT]{};
void *free_interposer_ignore_start2{};
atomic_uint free_interposer_locks_head2{};
atomic_bool free_interposer_locks2[FREE_INTERPOSER_BUF_COUNT]{};

void *free_interposer_malloc() {
    // if (true) return malloc(FREE_INTERPOSER_BUF_SIZE);
    if (true) return free_interposer_ignore_start;
    void *const start{free_interposer_ignore_start};
    if (!start) abort();
    bool expected{false};
    for (;;) {
        const uint i{free_interposer_locks_head.fetch_add(1, memory_order_relaxed) & FREE_INTERPOSER_BUF_MASK};
        if (free_interposer_locks[i].compare_exchange_weak(expected, true, memory_order_acq_rel, memory_order_relaxed))
            return static_cast<char*>(start) + FREE_INTERPOSER_BUF_SIZE * i;
    }
}

void *free_interposer_malloc2() {
    if (true) return free_interposer_ignore_start2;
    void *const start{free_interposer_ignore_start2};
    if (!start) abort();
    bool expected{false};
    for (;;) {
        const uint i{free_interposer_locks_head2.fetch_add(1, memory_order_relaxed) & FREE_INTERPOSER_BUF_MASK};
        if (free_interposer_locks2[i].compare_exchange_weak(expected, true, memory_order_acq_rel, memory_order_relaxed))
            return static_cast<char*>(start) + FREE_INTERPOSER_BUF_SIZE * i;
    }
}

extern "C" {
void __libc_free(void *); // NOLINT(*-reserved-identifier)
void free(void* ptr) {
    // write(1, "test\n", 5);
    const uintptr_t start{reinterpret_cast<uintptr_t>(free_interposer_ignore_start)};
    const uintptr_t start2{reinterpret_cast<uintptr_t>(free_interposer_ignore_start2)};
    const uintptr_t addr{reinterpret_cast<uintptr_t>(ptr)};
    if (start && addr >= start && addr < start + FREE_INTERPOSER_BUFS_SIZE) {
        if ((addr - start) % FREE_INTERPOSER_BUF_SIZE) abort();
        free_interposer_locks[(addr - start) / FREE_INTERPOSER_BUF_SIZE].store(false, memory_order_release);
        return;
    }
    if (start2 && addr >= start2 && addr < start2 + FREE_INTERPOSER_BUFS_SIZE) {
        if ((addr - start2) % FREE_INTERPOSER_BUF_SIZE) abort();
        free_interposer_locks2[(addr - start2) / FREE_INTERPOSER_BUF_SIZE].store(false, memory_order_release);
        return;
    }
    __libc_free(ptr);
}
}

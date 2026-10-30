#pragma once

#include <atomic>
#include <bit>
#include <sys/types.h>

static constexpr uint FREE_INTERPOSER_BUF_COUNT = 4;
static constexpr size_t FREE_INTERPOSER_BUF_SIZE = 1024UL * 1024UL;
static constexpr size_t FREE_INTERPOSER_BUFS_SIZE = FREE_INTERPOSER_BUF_SIZE * FREE_INTERPOSER_BUF_COUNT;
static_assert(std::has_single_bit(FREE_INTERPOSER_BUF_COUNT));
static constexpr uint FREE_INTERPOSER_BUF_MASK = FREE_INTERPOSER_BUF_COUNT - 1;

extern void *free_interposer_ignore_start;
extern std::atomic_uint free_interposer_locks_head;
extern std::atomic_bool free_interposer_locks[FREE_INTERPOSER_BUF_COUNT];

extern void *free_interposer_ignore_start2;
extern std::atomic_uint free_interposer_locks_head2;
extern std::atomic_bool free_interposer_locks2[FREE_INTERPOSER_BUF_COUNT];

[[gnu::returns_nonnull]] void *free_interposer_malloc();
[[gnu::returns_nonnull]] void *free_interposer_malloc2();

#define NO_MMAP_EXPERIMENT 1
// writev(2) EFAULTS with /dev/dri/renderD128-backed memory,
// but apparently it's fine with anon mmap that should theoretically end up with similar page table structures
#define NO_RENDERD128_WRITEV 0


#include <cstring>
#include <ranges>
#include <sys/user.h>

#include "memory_vulkan.hpp"

using namespace std;

/// Connection with GPU
namespace vram::memory {
using namespace detail;

VkDevice vk_device;
VkQueue transfer_queue;
mutex transfer_queue_mutex;
VkSemaphore timeline_semaphore;
VkBuffer download_buffer;

VkCommandPoolCreateInfo pool_create_info{
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
};

size_t device_num = ~size_t{};
int _pool_size{};
atomic_int32_t _pool_available{};

shared_ptr<block> allocate() {
    if (!_pool_available.load(memory_order_relaxed)) return {};
    return shared_ptr<block>(new block());
}

block::block() {
    lock_guard lock{pool_lock};
    buffer = pool.back();
    if (!buffer) abort();
    pool.pop_back();
    _pool_available.fetch_sub(1, memory_order_relaxed);
}

block::~block() {
    truncate(0);
    lock_guard lock{pool_lock};
    pool.push_back(buffer);
    _pool_available.fetch_add(1, memory_order_relaxed);
    buffer = nullptr;
}

void *download_arena_start{};
atomic_uint download_arena_locks_head{};
atomic_bool download_arena_locks[DOWNLOAD_ARENA_BUF_COUNT]{};

void block::read(size_t offset, size_t size, void *data) const {
    if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
    if (size < PAGE_SIZE || !buffer->vk) {
        memcpy(data, static_cast<char*>(buffer->mmap) + offset, size);
        return;
    }
    void *mem;
    void *const start{download_arena_start};
    if (!start) abort();
    uint download_arena_i;
    for (;;) {
        static_assert(std::has_single_bit(DOWNLOAD_ARENA_BUF_COUNT));
        download_arena_i = download_arena_locks_head.fetch_add(1, memory_order_relaxed) % DOWNLOAD_ARENA_BUF_COUNT;
        bool expected{false};
        if (download_arena_locks[download_arena_i].compare_exchange_weak(expected, true, memory_order_acq_rel, memory_order_relaxed)) {
            mem = static_cast<char*>(start) + DOWNLOAD_ARENA_BUF_SIZE * download_arena_i;
            break;
        }
    }
    copy_buffer(buffer->vk, download_buffer, offset, reinterpret_cast<uintptr_t>(mem) - reinterpret_cast<uintptr_t>(start), size);
    download_arena_locks[download_arena_i].store(false, memory_order_relaxed);
    memcpy(data, mem, size);
}

void block::write(size_t offset, size_t size, const void *data) {
    if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
    memcpy(static_cast<char*>(buffer->mmap) + offset, data, size);
}

void block::truncate(size_t size) {
    if (size > SIZE) abort();
    size_t size_vk{SIZE};
    if (buffer->vk) {
        size_vk = size & 3 ? (size & ~3) + 4 : size;
        clear_buffer(buffer->vk, size_vk, SIZE - size_vk);
    }
    memset(static_cast<char*>(buffer->mmap) + size, 0, size_vk - size);
}
}

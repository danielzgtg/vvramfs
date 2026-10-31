#pragma once

#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

#include "memory.hpp"

namespace vram::memory {
extern VkDevice vk_device;
extern VkQueue transfer_queue;
extern std::mutex transfer_queue_mutex;
extern VkSemaphore timeline_semaphore;
extern VkBuffer download_buffer;
extern VkCommandPoolCreateInfo pool_create_info;

static constexpr uint DOWNLOAD_ARENA_BUF_COUNT = 16;
static constexpr size_t DOWNLOAD_ARENA_BUFS_SIZE = DOWNLOAD_ARENA_BUF_SIZE * DOWNLOAD_ARENA_BUF_COUNT;
extern void *download_arena_start;

namespace detail {
struct vram_buffer {
    void *mmap;
    VkBuffer vk;
};

extern uint32_t host_memory_type;
extern std::vector<vram_buffer*> pool;
extern std::mutex pool_lock;
extern PFN_vkGetMemoryHostPointerPropertiesEXT pfn_vk_get_memory_host_pointer_properties_ext;
}

void clear_buffer(VkBuffer buffer, size_t offset, size_t size);
void copy_buffer(VkBuffer src, VkBuffer dst, size_t src_offest, size_t dst_offset, size_t size);
}

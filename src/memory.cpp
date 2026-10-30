#include <cstring>
#include <ranges>
#include <sys/user.h>

#include "free_interposer.hpp"
#include "memory_vulkan.hpp"

using namespace std;

/// Connection with GPU
namespace vram::memory {
using namespace detail;

VkDevice vk_device;
VkQueue transfer_queue;
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

// TODO move to read_buf and aligned_alloc
static mutex page_mmap_mutex{};

void block::read(size_t offset, size_t size, void *data) const {
    if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
    if (size < PAGE_SIZE || !buffer->vk) {
        memcpy(data, static_cast<char*>(buffer->mmap) + offset, size);
        return;
    }
#if NO_MMAP_EXPERIMENT
    void *mem = free_interposer_malloc();
    copy_buffer(buffer->vk, download_buffer, offset, (uintptr_t)mem - (uintptr_t)free_interposer_ignore_start, size);
    memcpy(data, mem, size);
#else
    lock_guard page_mmap_lock{page_mmap_mutex};
    const char *const data_addr{static_cast<char*>(data)};
    const auto vk_page_mmap_start = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(data) & PAGE_MASK);
    const auto vk_page_mmap_end = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(data) + size + ~PAGE_MASK &
        PAGE_MASK);
    const size_t vk_page_mmap_size = vk_page_mmap_end - vk_page_mmap_start;
    VkMemoryHostPointerPropertiesEXT host_pointer_properties_ext{
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    if (pfn_vk_get_memory_host_pointer_properties_ext(vk_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                      vk_page_mmap_start,
                                                      &host_pointer_properties_ext) != VK_SUCCESS) abort();
    if (~host_pointer_properties_ext.memoryTypeBits & (1 << host_memory_type)) abort();
    static constexpr VkExternalMemoryBufferCreateInfo external_memory_buffer_create_info{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    const VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_memory_buffer_create_info,
        .size = vk_page_mmap_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer dst_buffer;
    if (vkCreateBuffer(vk_device, &create_info, nullptr, &dst_buffer) != VK_SUCCESS) abort();
    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(vk_device, dst_buffer, &memory_requirements);
    if (memory_requirements.size != vk_page_mmap_size || memory_requirements.alignment > PAGE_SIZE || ~
        memory_requirements.memoryTypeBits & (1 << host_memory_type)) abort();
    const VkImportMemoryHostPointerInfoEXT host_pointer_info_ext{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = vk_page_mmap_start,
    };
    const VkMemoryAllocateInfo memory_allocate_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &host_pointer_info_ext,
        .allocationSize = vk_page_mmap_size,
        .memoryTypeIndex = host_memory_type,
    };
    VkDeviceMemory host_memory;
    if (vkAllocateMemory(vk_device, &memory_allocate_info, nullptr, &host_memory) != VK_SUCCESS) abort();
    if (vkBindBufferMemory(vk_device, dst_buffer, host_memory, 0) != VK_SUCCESS) abort();
    copy_buffer(buffer->vk, dst_buffer, offset, data_addr - vk_page_mmap_start, size);
    vkFreeMemory(vk_device, host_memory, nullptr);
    vkDestroyBuffer(vk_device, dst_buffer, nullptr);
#endif
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

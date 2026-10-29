#include <vulkan/vulkan.h>

#include "memory_vulkan.hpp"

using namespace std;

namespace vram::memory {
namespace {
class command_recording;
class command_pool {
    friend command_recording;
    VkCommandPool pool{};
    VkCommandBuffer buffer{};
public:
    explicit command_pool() {
        if (vkCreateCommandPool(vk_device, &pool_create_info, nullptr, &pool) != VK_SUCCESS) abort();
        const VkCommandBufferAllocateInfo buffer_allocate_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(vk_device, &buffer_allocate_info, &buffer) != VK_SUCCESS) abort();
    }
    command_pool(const command_pool& other) = delete;
    command_pool(command_pool&& other) = delete;
    command_pool& operator=(const command_pool& other) = delete;
    command_pool& operator=(command_pool&& other) = delete;
};

vector<unique_ptr<command_pool>> command_pools;
mutex command_pools_lock;

class command_pool_lease {
    friend command_recording;
    unique_ptr<command_pool> inner;
    bool in_use{};
public:
    explicit command_pool_lease() {
        {
            lock_guard lock{command_pools_lock};
            if (!command_pools.empty()) {
                inner = std::move(command_pools.back());
                command_pools.pop_back();
                return;
            }
        }
        inner = make_unique<command_pool>();
    }
    ~command_pool_lease() {
        lock_guard lock{command_pools_lock};
        command_pools.emplace_back(std::move(inner));
    }
    command_pool& operator*() const {
        return *inner;
    }
    command_pool* operator->() const {
        return &*inner;
    }
    command_pool_lease(const command_pool_lease& other) = delete;
    command_pool_lease(command_pool_lease&& other) = delete;
    command_pool_lease& operator=(const command_pool_lease& other) = delete;
    command_pool_lease& operator=(command_pool_lease&& other) = delete;
};

atomic_uint64_t cur_wait_semaphore_value{};
class command_recording {
    command_pool_lease &inner;
public:
    explicit command_recording(command_pool_lease &inner_) : inner{inner_} {
        if (inner.in_use) abort();
        inner.in_use = true;
        static constexpr VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (vkBeginCommandBuffer(**this, &begin_info) != VK_SUCCESS) abort();
    }
    ~command_recording() {
        if (vkEndCommandBuffer(**this) != VK_SUCCESS) abort();
        const uint64_t last_semaphore_value = cur_wait_semaphore_value.fetch_add(1, memory_order_relaxed);
        const uint64_t next_semaphore_value = last_semaphore_value + 1;
        const VkTimelineSemaphoreSubmitInfo timeline_semaphore_submit_info{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &last_semaphore_value,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &next_semaphore_value,
        };
        static constexpr VkPipelineStageFlags pipeline_stage_flags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        const VkSubmitInfo submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_semaphore_submit_info,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &timeline_semaphore,
            .pWaitDstStageMask = &pipeline_stage_flags,
            .commandBufferCount = 1,
            .pCommandBuffers = &**this,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &timeline_semaphore,
        };
        if (vkQueueSubmit(transfer_queue, 1, &submit_info, nullptr) != VK_SUCCESS) abort();
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &timeline_semaphore,
            .pValues = &next_semaphore_value,
        };
        if (vkWaitSemaphores(vk_device, &wait_info, ~0UL) != VK_SUCCESS) abort();
        vkResetCommandPool(vk_device, inner.inner->pool, 0);
        inner.in_use = false;
    }
    VkCommandBuffer& operator*() const {
        return inner.inner->buffer;
    }
    VkCommandBuffer* operator->() const {
        return &inner.inner->buffer;
    }
    command_recording(const command_recording& other) = delete;
    command_recording(command_recording&& other) = delete;
    command_recording& operator=(const command_recording& other) = delete;
    command_recording& operator=(command_recording&& other) = delete;
};
}

void clear_buffer(VkBuffer buffer, size_t offset, size_t size) {
    if (!size) return;
    command_pool_lease lease{};
    command_recording recording{lease};
    vkCmdFillBuffer(*recording, buffer, offset, size, 0);
}

void copy_buffer(VkBuffer src, VkBuffer dst, size_t src_offest, size_t dst_offset, size_t size) {
    if (!size) return;
    command_pool_lease lease{};
    command_recording recording{lease};
    const VkBufferCopy copy {
        .srcOffset = src_offest,
        .dstOffset = dst_offset,
        .size = size,
    };
    vkCmdCopyBuffer(*recording, src, dst, 1, &copy);
}
}
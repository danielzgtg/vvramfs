#pragma once

#include <vulkan/vulkan.h>
#include "memory.hpp"

namespace vram::memory {
extern VkDevice vk_device;
extern VkQueue transfer_queue;
extern VkSemaphore timeline_semaphore;
extern VkCommandPoolCreateInfo pool_create_info;

void clear_buffer(VkBuffer buffer, size_t offset, size_t size);
}
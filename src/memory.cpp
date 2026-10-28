#include <cstring>
#include <ranges>
#include <vector>
#include <vulkan/vulkan.h>

#include "memory.hpp"

using namespace std;

/// Connection with GPU
namespace vram {
    namespace memory {
        VkDevice vk_device;
        uint32_t device_memory_type;

        vector<void *> pool;
        mutex pool_lock{};
        size_t total_blocks = 0;

        size_t device_num = ~size_t{};

        // Find platform with capable GPU
        static void init_gpu() {
            if (vk_device) return;
            static const VkApplicationInfo application_info {
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pApplicationName = "vvramfs",
                .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                .apiVersion = VK_API_VERSION_1_0,
            };
            static const VkInstanceCreateInfo instance_create_info {
                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                .pApplicationInfo = &application_info,
            };
            VkInstance vk_instance;
            if (vkCreateInstance(&instance_create_info, nullptr, &vk_instance) != VK_SUCCESS) abort();
            uint32_t physical_device_count;
            if (vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, nullptr) != VK_SUCCESS) abort();
            if (!physical_device_count) abort();
            vector<VkPhysicalDevice> physical_devices(physical_device_count);
            if (vkEnumeratePhysicalDevices(vk_instance, &physical_device_count, physical_devices.data()) != VK_SUCCESS) abort();
            VkPhysicalDevice physical_device{};
            for (const auto [i, device] : physical_devices | views::as_const | views::enumerate) {
                if (~device_num && i != device_num) continue;
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(device, &properties);
                if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
                physical_device = device;
                break;
            }
            if (!physical_device) abort();
            static const VkDeviceCreateInfo device_create_info {
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            };
            if (vkCreateDevice(physical_device, &device_create_info,nullptr, &vk_device) != VK_SUCCESS) abort();
            VkPhysicalDeviceMemoryProperties memory_properties;
            vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
            device_memory_type = ~uint32_t{};
            for (auto [i, memory_type] : memory_properties.memoryTypes | views::as_const | views::take(memory_properties.memoryTypeCount) | views::enumerate) {
                if (~memory_type.propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
                device_memory_type = i;
                break;
            }
            if (!~device_memory_type) abort();
        }

        bool is_available() {
            init_gpu();
            return true;
        }

        void set_device(size_t device) {
            device_num = device;
        }

        int pool_size() {
            return total_blocks;
        }

        int pool_available() {
            return pool.size();
        }

        void increase_pool(size_t size) {
            if (size % block::SIZE) abort();
            total_blocks = size / block::SIZE;
            const VkMemoryAllocateInfo memory_allocate_info {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = block::SIZE,
                .memoryTypeIndex = device_memory_type,
            };
            lock_guard lock{pool_lock};
            for (auto _ : views::iota(size_t{}, total_blocks)) {
                VkDeviceMemory device_memory;
                void *buf;
#if 1
                if (vkAllocateMemory(vk_device, &memory_allocate_info, nullptr, &device_memory) != VK_SUCCESS) abort();
                if (vkMapMemory(vk_device, device_memory, 0, block::SIZE, 0, &buf) != VK_SUCCESS) abort();
#else
                buf = malloc(block::SIZE);
#endif
                memset(buf, 0, block::SIZE);
                pool.push_back(buf);
            }
        }

        block_ref allocate() {
            if (pool.empty()) return {};
            return block_ref(new block());
        }

        block::block() {
            lock_guard lock{pool_lock};
            buffer = pool.back();
            if (!buffer) abort();
            pool.pop_back();
        }

        block::~block() {
            truncate(0);
            lock_guard lock{pool_lock};
            pool.push_back(buffer);
        }

        void block::read(size_t offset, size_t size, void* data) const {
            if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
            memcpy(data, static_cast<char*>(buffer) + offset, size);
        }

        void block::write(size_t offset, size_t size, const void* data) {
            if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
            memcpy(static_cast<char *>(buffer) + offset, data, size);
        }

        void block::truncate(size_t size) {
            if (size > SIZE) abort();
            memset(static_cast<char *>(buffer) + size, 0, SIZE - size);
        }
    }
}

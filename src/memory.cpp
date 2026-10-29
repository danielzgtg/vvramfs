#include <atomic>
#include <cstring>
#include <ranges>
#include <sys/user.h>
#include <vector>

#include "memory_vulkan.hpp"

using namespace std;

/// Connection with GPU
namespace vram {
    namespace memory {
        struct vram_buffer {
            void *mmap;
            VkBuffer vk;
        };

        VkDevice vk_device;
        VkQueue transfer_queue;
        uint32_t device_memory_type;
        uint32_t host_memory_type;
        VkSemaphore timeline_semaphore;
        PFN_vkGetMemoryHostPointerPropertiesEXT pfn_vk_get_memory_host_pointer_properties_ext;

        VkCommandPoolCreateInfo pool_create_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        };

        namespace {
        vector<vram_buffer *> pool;
        mutex pool_lock{};
        }
        size_t device_num = ~size_t{};
        int _pool_size{};
        atomic_int32_t _pool_available{};

        // Find platform with capable GPU
        void init_gpu(size_t size) {
            if (vk_device || !size || size % block::SIZE || size > INT16_MAX * block::SIZE) abort();
            static const VkApplicationInfo application_info {
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pApplicationName = "vvramfs",
                .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                .apiVersion = VK_API_VERSION_1_2,
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
            {
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT memory_host_properties_ext {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
                };
                VkPhysicalDeviceProperties2 physical_device_properties2 {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                    .pNext = &memory_host_properties_ext,
                };
                for (const auto [i, device] : physical_devices | views::as_const | views::enumerate) {
                    if (~device_num && i != device_num) continue;
                    vkGetPhysicalDeviceProperties2(device, &physical_device_properties2);
                    if (physical_device_properties2.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
                    if (memory_host_properties_ext.minImportedHostPointerAlignment > PAGE_SIZE) abort();
                    physical_device = device;
                    break;
                }
                if (!physical_device) abort();
            }
            uint32_t queue_family_count{};
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
            vector<VkQueueFamilyProperties> queue_families(queue_family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
            uint32_t transfer_family{~uint32_t{}};
            for (const auto [i, family] : queue_families | views::as_const | views::enumerate) {
                if (~family.queueFlags & VK_QUEUE_TRANSFER_BIT) continue;
                transfer_family = i;
            }
            for (const auto [i, family] : queue_families | views::as_const | views::enumerate) {
                if (~family.queueFlags & VK_QUEUE_TRANSFER_BIT || family.queueFlags & VK_QUEUE_GRAPHICS_BIT) continue;
                transfer_family = i;
            }
            if (!~transfer_family) abort();
            static constexpr float ONE = 1.0f;
            const VkDeviceQueueCreateInfo queue_create_info{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = transfer_family,
                .queueCount = 1,
                .pQueuePriorities = &ONE,
            };
            pool_create_info.queueFamilyIndex = transfer_family;
            VkPhysicalDeviceVulkan12Features vulkan12_features {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .timelineSemaphore = VK_TRUE,
            };
            static constexpr std::array<const char *, 1> extension_names{
                VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
            };
            const VkDeviceCreateInfo device_create_info {
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext = &vulkan12_features,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos = &queue_create_info,
                .enabledExtensionCount = 1,
                .ppEnabledExtensionNames = extension_names.data(),
            };
            if (vkCreateDevice(physical_device, &device_create_info,nullptr, &vk_device) != VK_SUCCESS) abort();
            vkGetDeviceQueue(vk_device, transfer_family, 0, &transfer_queue);
            VkPhysicalDeviceMemoryProperties memory_properties;
            vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
            device_memory_type = ~uint32_t{};
            for (const auto [i, memory_type] : memory_properties.memoryTypes | views::as_const | views::take(memory_properties.memoryTypeCount) | views::enumerate) {
                if (~memory_type.propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
                device_memory_type = i;
                break;
            }
            if (!~device_memory_type) abort();
            host_memory_type = ~uint32_t{};
            for (const auto [i, memory_type] : memory_properties.memoryTypes | views::as_const | views::take(memory_properties.memoryTypeCount) | views::enumerate) {
                if (~memory_type.propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
                host_memory_type = i;
                break;
            }
            if (!~host_memory_type) abort();
            static constexpr VkSemaphoreTypeCreateInfo semaphore_type_create_info{
                .sType =  VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            };
            static constexpr VkSemaphoreCreateInfo semaphore_create_info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = &semaphore_type_create_info,
            };
            if (vkCreateSemaphore(vk_device, &semaphore_create_info, nullptr, &timeline_semaphore) != VK_SUCCESS) abort();
            pfn_vk_get_memory_host_pointer_properties_ext = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(vkGetDeviceProcAddr(
                vk_device, "vkGetMemoryHostPointerPropertiesEXT"));
            _pool_size = static_cast<int>(size / block::SIZE);
            lock_guard lock{pool_lock};
            for (auto _ : views::iota(int{}, _pool_size)) {
                static constexpr VkBufferCreateInfo buffer_create_info{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = block::SIZE,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                };
                VkBuffer vk_buffer{};
                if (vkCreateBuffer(vk_device, &buffer_create_info, nullptr, &vk_buffer) != VK_SUCCESS) abort();
                VkMemoryRequirements memory_requirements;
                vkGetBufferMemoryRequirements(vk_device, vk_buffer, &memory_requirements);
                if (memory_requirements.size != block::SIZE || ~memory_requirements.memoryTypeBits & (1 << device_memory_type)) abort();
                const VkMemoryAllocateInfo memory_allocate_info {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = block::SIZE,
                    .memoryTypeIndex = device_memory_type,
                };
                void *buf;
#if 1
                VkDeviceMemory device_memory;
                if (vkAllocateMemory(vk_device, &memory_allocate_info, nullptr, &device_memory) != VK_SUCCESS) abort();
                if (vkMapMemory(vk_device, device_memory, 0, block::SIZE, 0, &buf) != VK_SUCCESS) abort();
                vkBindBufferMemory(vk_device, vk_buffer, device_memory, 0);
                clear_buffer(vk_buffer, 0, block::SIZE);
#else
                vk_buffer = nullptr;
                buf = calloc(1, block::SIZE);
#endif
                pool.push_back(new vram_buffer {
                    .mmap = buf,
                    .vk = vk_buffer,
                });
            }
            _pool_available.store(_pool_size, memory_order_relaxed);
        }

        std::shared_ptr<block> allocate() {
            if (!_pool_available.load(memory_order_relaxed)) return {};
            return std::shared_ptr<block>(new block());
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
            lock_guard page_mmap_lock{page_mmap_mutex};
            const char *const data_addr{static_cast<char *>(data)};
            const auto vk_page_mmap_start = reinterpret_cast<char *>(reinterpret_cast<uintptr_t>(data) & PAGE_MASK);
            const auto vk_page_mmap_end = reinterpret_cast<char *>(reinterpret_cast<uintptr_t>(data) + size + ~PAGE_MASK & PAGE_MASK);
            const size_t vk_page_mmap_size = vk_page_mmap_end - vk_page_mmap_start;
            VkMemoryHostPointerPropertiesEXT host_pointer_properties_ext{
                .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
            };
            if (pfn_vk_get_memory_host_pointer_properties_ext(vk_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, vk_page_mmap_start, &host_pointer_properties_ext) != VK_SUCCESS) abort();
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
            if (memory_requirements.size != vk_page_mmap_size || memory_requirements.alignment > PAGE_SIZE || ~memory_requirements.memoryTypeBits & (1 << host_memory_type)) abort();
            const VkImportMemoryHostPointerInfoEXT host_pointer_info_ext{
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                .pHostPointer = vk_page_mmap_start,
            };
            const VkMemoryAllocateInfo memory_allocate_info {
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
        }

        void block::write(size_t offset, size_t size, const void* data) {
            if (offset > SIZE || size > SIZE || offset + size > SIZE) abort();
            memcpy(static_cast<char *>(buffer->mmap) + offset, data, size);
        }

        void block::truncate(size_t size) {
            if (size > SIZE) abort();
            size_t size_vk{SIZE};
            if (buffer->vk) {
                size_vk = size & 3 ? (size & ~3) + 4 : size;
                clear_buffer(buffer->vk, size_vk, SIZE - size_vk);
            }
            memset(static_cast<char *>(buffer->mmap) + size, 0, size_vk - size);
        }
    }
}

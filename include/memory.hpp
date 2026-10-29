#pragma once

#include <memory>

/**
 * VRAM block allocation
 */
namespace vram {
    namespace memory {
        extern size_t device_num;
        extern int _pool_size;
        extern std::atomic_int32_t _pool_available;
        void init_gpu(size_t size);
        /// Get a new block of memory from the pool, returns nullptr if pool is empty
        std::shared_ptr<class block> allocate();
        /// Block of allocated VRAM
        class block : public std::enable_shared_from_this<block> {
            friend std::shared_ptr<block> allocate();

        public:
            static constexpr size_t SIZE = 128UL * 1024UL * 1024UL;
            block(const block& other) = delete;
            ~block();
            void read(size_t offset, size_t size, void* data) const;
            void write(size_t offset, size_t size, const void* data);
            void truncate(size_t size);
        private:
            struct vram_buffer *buffer;
            block();
        };
    }
}

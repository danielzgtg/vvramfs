#pragma once

#include <memory>

/**
 * VRAM block allocation
 */
namespace vram {
    namespace memory {
        class block;
        typedef std::shared_ptr<block> block_ref;

        // Check if current machine supports VRAM allocation
        bool is_available();

        // Set the device to use
        void set_device(size_t num);

        // Total blocks and blocks currently free
        int pool_size();
        int pool_available();

        // Allocate pool of memory blocks, returns actual amount allocated (in bytes)
        void increase_pool(size_t size);

        // Get a new block of memory from the pool, returns nullptr if pool is empty
        block_ref allocate();

        /**
         * Block of allocated VRAM
         */
        class block : public std::enable_shared_from_this<block> {
            friend block_ref allocate();

        public:
            static constexpr size_t SIZE = 128UL * 1024UL * 1024UL;

            block(const block& other) = delete;

            ~block();

            void read(off_t offset, size_t size, void* data) const;

            // Data may be freed afterwards, even if called with async = true
            void write(off_t offset, size_t size, const void* data, bool async = false);

            void truncate(size_t size);

        private:
            void *buffer;
            block();
        };
    }
}

#include <cstring>
#include <ranges>

#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        file_ref file_t::make(dir_ptr parent, const string& name) {
            auto file = file_ref(new file_t());
            file->link(parent, name);
            return file;
        }

        file_t::file_t() {
            mode(0644);
        }

        type::type_t file_t::type() const {
            return type::file;
        }

        size_t file_t::size() const {
            return _size;
        }

        void file_t::size(size_t new_size) {
            if (new_size < _size) {
                free_blocks(new_size);
            }

            _size = new_size;

            mtime(util::time());
        }

        int file_t::read(off_t off, size_t size, char* data, std::mutex& wait_mutex) {
            if ((size_t) off >= _size) return 0;
            size = std::min(_size - off, size);

            // Walk over blocks in read region
            off_t end_pos = off + size;
            size_t total_read = size;

            while (off < end_pos) {
                // Find block corresponding to current offset
                off_t block_start = (off / memory::block::SIZE) * memory::block::SIZE;
                off_t block_off = off - block_start;
                size_t read_size = std::min(memory::block::SIZE - block_off, size);

                auto block = get_block(block_start);

                // Allow multiple threads to block for reading simultaneously
                wait_mutex.unlock();
                if (block) {
                    block->read(block_off, read_size, data);
                } else {
                    // Non-written part of file
                    memset(data, 0, read_size);
                }
                wait_mutex.lock();

                data += read_size;
                off += read_size;
                size -= read_size;
            }

            atime(util::time());

            return total_read;
        }

        int file_t::write(off_t off, size_t size, const char* data, bool async) {
            // Walk over blocks in write region
            off_t end_pos = off + size;
            size_t total_write = size;

            while (off < end_pos) {
                // Find block corresponding to current offset
                off_t block_start = (off / memory::block::SIZE) * memory::block::SIZE;
                off_t block_off = off - block_start;
                size_t write_size = std::min(memory::block::SIZE - block_off, size);

                auto block = get_block(block_start);

                if (!block) {
                    block = alloc_block(block_start);

                    // Failed to allocate buffer, likely out of VRAM
                    if (!block) break;
                }

                block->write(block_off, write_size, data, async);

                data += write_size;
                off += write_size;
                size -= write_size;
            }

            if (_size < (size_t) off) {
                _size = off;
            }
            mtime(util::time());

            if (off < end_pos) {
                return -ENOSPC;
            } else {
                return total_write;
            }
        }

        void file_t::sync() {
            // Unneeded with vkMapMemory
        }

        memory::block_ref file_t::get_block(off_t off) const {
            off_t start_off = off / memory::block::SIZE;
            if (start_off >= file_blocks.size()) return {};
            return file_blocks[start_off];
        }

        memory::block_ref file_t::alloc_block(off_t off) {
            off_t start_off = off / memory::block::SIZE;
            if (start_off >= file_blocks.size()) return {};
            if (file_blocks[start_off]) abort();
            memory::block_ref block = memory::allocate();
            if (block) file_blocks[start_off] = block;
            return block;
        }

        void file_t::free_blocks(off_t off) {
            off_t start_off = off / memory::block::SIZE;
            if (start_off >= file_blocks.size()) return;
            size_t tail = off % memory::block::SIZE;
            if (tail != 0) {
                memory::block_ref &block{file_blocks[start_off]};
                if (block) block->truncate(tail);
                ++start_off;
            }
            for (memory::block_ref &block : file_blocks | std::views::drop(start_off)) block.reset();
        }
    }
}

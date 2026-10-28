#include <cstring>
#include <ranges>

#include "entry.hpp"
#include "util.hpp"

using namespace std;

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

        int file_t::size(size_t new_size) {
            if (new_size > MAX_FILE_SIZE) return -EINVAL;
            const unique_lock lock{rwlock};
            if (new_size < _size) free_blocks(new_size, lock);
            _size = new_size;
            mtime(util::time());
            return 0;
        }

        size_t file_t::bmap_walk(off_t off_, const size_t size, char* data, bmap_cb cb, bmap_t &bmap_dup) {
            if (off_ < 0 || off_ > MAX_FILE_SIZE || size > MAX_IO_SIZE) return -EINVAL;
            size_t off = off_;
            const size_t end_pos = off + size;
            if (end_pos > MAX_FILE_SIZE) return -EINVAL;
            while (off < end_pos) {
                // Find block corresponding to current offset
                const size_t block_idx{off / memory::block::SIZE};
                const size_t block_off{off % memory::block::SIZE};
                const size_t io_size{min(memory::block::SIZE - block_off, end_pos - off)};
                if (block_idx >= bmap.size()) abort();
                if (cb(bmap[block_idx], block_idx, block_off, io_size, data, bmap_dup)) break;
                data += io_size;
                off += io_size;
            }
            return off - off_;
        }

        static int read_cb1(memory::block_ref &orig_block, size_t block_idx, size_t, size_t, char*, bmap_t &bmap_dup) {
            bmap_dup[block_idx] = orig_block;
            return 0;
        }

        static int read_cb2(memory::block_ref &, size_t block_idx, size_t block_off, size_t io_size, char* data, bmap_t &bmap_dup) {
            memory::block_ref &block = bmap_dup[block_idx];
            if (block) {
                block->read(block_off, io_size, data);
            } else {
                // Non-written part of file
                memset(data, 0, io_size);
            }
            return 0;
        }

        int file_t::read(off_t off, size_t size, char* data) {
            if ((size_t) off >= _size) return 0;
            size = min(_size - off, size);
            bmap_t bmap_dup{};
            {
                const shared_lock lock{rwlock};
                size = bmap_walk(off, size, data, read_cb1, bmap_dup);
            }
            bmap_walk(off, size, data, read_cb2, bmap_dup);
            atime(util::time());
            return size;
        }

        static int write_cb1(memory::block_ref &orig_block, size_t block_idx, size_t, size_t, char*, bmap_t &bmap_dup) {
            if (!orig_block) {
                orig_block = memory::allocate();
                if (!orig_block) return -ENOSPC;
            }
            bmap_dup[block_idx] = orig_block;
            return 0;
        }

        static int write_cb2(memory::block_ref &, size_t block_idx, size_t block_off, size_t io_size, char* data_, bmap_t &bmap_dup) {
            const char *data{data_};
            memory::block_ref &block = bmap_dup[block_idx];
            if (!block) abort();
            block->write(block_off, io_size, data);
            return 0;
        }

        int file_t::write(off_t off, size_t size, const char* data) {
            if (!size) return 0;
            bmap_t bmap_dup{};
            const unique_lock lock{rwlock};
            size = bmap_walk(off, size, const_cast<char *>(data), write_cb1, bmap_dup);
            if (!size) return -ENOSPC;
            bmap_walk(off, size, const_cast<char *>(data), write_cb2, bmap_dup);
            if (_size < size + off) _size = size + off;
            mtime(util::time());
            return size;
        }

        void file_t::free_blocks(size_t off, const unique_lock<shared_mutex>&) {
            size_t block_start = off / memory::block::SIZE;
            if (block_start >= bmap.size()) abort();
            size_t tail = off % memory::block::SIZE;
            if (tail != 0) {
                memory::block_ref &block{bmap[block_start]};
                if (block) block->truncate(tail);
                ++block_start;
            }
            for (memory::block_ref &block : bmap | views::drop(block_start)) block.reset();
        }
    }
}

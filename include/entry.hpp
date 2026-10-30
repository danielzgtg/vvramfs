#pragma once

#include <array>
#include <mutex>
#include <string>
#include <shared_mutex>
#include <sys/stat.h>
#include <unordered_map>

#include "memory.hpp"

namespace vram::entry {
class entry_t;
class file_t;
class dir_t;
class symlink_t;

enum type_t {
    TYPE_NONE = 0,
    TYPE_FILE = S_IFREG,
    TYPE_DIR = S_IFDIR,
    TYPE_SYMLINK = S_IFLNK,
    TYPE_ALL = TYPE_FILE | TYPE_DIR | TYPE_SYMLINK,
};

extern std::atomic_int entry_count;

class entry_t : public std::enable_shared_from_this<entry_t> {
public:
    entry_t(const entry_t &other) = delete;
    virtual ~entry_t();
    const type_t type;
    std::atomic_size_t _size;
    std::atomic<mode_t> _mode;
    std::atomic<uid_t> _user{0};
    std::atomic<gid_t> _group{0};
    const timespec btime;
    std::atomic<timespec> _atime;
    std::atomic<timespec> _mtime;
    std::atomic<timespec> _ctime;
    void atime(timespec t);
    void mtime(timespec t);
    void ctime(timespec t);
    void mode(mode_t mode);
    void chown(uid_t user, gid_t group);
protected:
    explicit entry_t(type_t type_, size_t size_, mode_t mode_);
};

static constexpr size_t MAX_FILE_SIZE = 2UL * 1024UL * 1024UL * 1024UL;
static_assert(MAX_FILE_SIZE % memory::block::SIZE == 0);
using bmap_t = std::array<std::shared_ptr<memory::block>, MAX_FILE_SIZE / memory::block::SIZE>;
static constexpr size_t MAX_IO_SIZE = 1024UL * 1024UL;
static_assert(MAX_IO_SIZE < MAX_FILE_SIZE);

class file_t : public entry_t {
public:
    int truncate(size_t new_size);
    int read(off_t off, size_t size, char *data);
    int write(off_t off, size_t size, const char *data);
    explicit file_t();
private:
    bmap_t bmap;
    std::shared_mutex bmap_rwlock;
    void free_blocks(size_t off, const std::unique_lock<std::shared_mutex> &);
    using bmap_cb = int(&)(std::shared_ptr<memory::block> &orig_block, size_t block_idx, size_t block_off, size_t io_size, char *data, bmap_t &bmap_dup);
    size_t bmap_walk(off_t off, size_t size, char *data, bmap_cb cb, bmap_t &bmap_dup);
};

struct namei_w_lock {
    std::shared_lock<std::shared_mutex> r_lock_prev;
    std::unique_lock<std::shared_mutex> w_lock;
    dir_t *parent;
};

int namei(std::string_view path, namei_w_lock *w_lock, std::shared_ptr<entry_t> &result);

struct sv_hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    size_t operator()(const std::string &s) const { return std::hash<std::string_view>{}(s); }
};

class dir_t : public entry_t {
    friend class entry_t;
public:
    std::unordered_map<std::string, std::shared_ptr<entry_t>, sv_hash, std::equal_to<>> _children;
    std::shared_mutex children_rwlock;
    explicit dir_t();
};

class symlink_t : public entry_t {
public:
    const std::string target;
    explicit symlink_t(const std::string &target);
};
}

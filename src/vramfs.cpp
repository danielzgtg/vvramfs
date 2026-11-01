#define FUSE_USE_VERSION 30
#include <cstring>
#include <filesystem>
#include <fuse.h>
#include <iostream>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>

#include "entry.hpp"
#include "util.hpp"

using namespace std;
using namespace vram;
using enum entry::type_t;

static constexpr int filter_entry_type(const entry::entry_t &entry, underlying_type_t<entry::type_t> filter) {
    if (!(entry.type & filter)) {
        if (entry.type == TYPE_FILE) {
            if (filter & TYPE_SYMLINK) return -ENOENT;
            if (filter & TYPE_DIR) return -EISDIR;
        } else if (entry.type == TYPE_DIR) {
            if (filter & TYPE_FILE) return -ENOTDIR;
            if (filter & TYPE_SYMLINK) return -EPERM;
        } else {
            return -EPERM;
        }
    }
    return 0;
}

#define GET_ENTRY_OR_RETURN(filter) \
    shared_ptr<entry::entry_t> entry_; \
    if (const int err{entry::namei(path, nullptr, entry_)}; err) return err; \
    entry::entry_t &entry{*entry_}; \
    if (const int err{filter_entry_type(entry, filter)}; err) return err; \
    do {} while (0)

static int vram_init_impl() {
    static constexpr auto path{"/"};
    shared_ptr<entry::entry_t> entry_;
    if (const int err{entry::namei(path, nullptr, entry_)}; err) return err;
    entry::entry_t &entry{*entry_};
    if (const int err{filter_entry_type(entry, TYPE_ALL)}; err) return err;
    entry.chown(geteuid(), getegid());
    entry._size.store(0x746f6f72, memory_order_relaxed); // "root"
    return 0;
}

static void* vram_init(fuse_conn_info *conn, fuse_config *cfg) {
    cfg->direct_io = true;
    if (vram_init_impl()) abort();
    cout << "mounted." << endl;
    return nullptr;
}

static int vram_statfs(const char *, struct statvfs *vfs) {
    vfs->f_bsize = memory::DOWNLOAD_ARENA_BUF_SIZE;
    vfs->f_frsize = memory::block::SIZE;
    vfs->f_blocks = memory::_pool_size;
    vfs->f_bfree = vfs->f_bavail = memory::_pool_available.load(memory_order_relaxed);
    const int entry_count{entry::entry_count.load(memory_order_relaxed)};
    vfs->f_files = entry_count;
    vfs->f_ffree = max(0, entry::MAX_ENTRY_COUNT - entry_count);
    vfs->f_namemax = NAME_MAX;
    return 0;
}

static int vram_getattr(const char *path, struct stat *stbuf, fuse_file_info *) {
    GET_ENTRY_OR_RETURN(TYPE_ALL);
    *stbuf = {
        .st_nlink = 1,
        .st_mode = entry.type | entry._mode.load(memory_order_acquire),
        .st_uid = entry._user.load(memory_order_acquire),
        .st_gid = entry._group.load(memory_order_acquire),
        .st_size = static_cast<off_t>(entry._size.load(memory_order_relaxed)),
        .st_atim = entry._atime.load(memory_order_relaxed),
        .st_mtim = entry._mtime.load(memory_order_relaxed),
        .st_ctim = entry._ctime.load(memory_order_relaxed),
    };
    if (entry.type == TYPE_FILE) {
        stbuf->st_blksize = memory::block::SIZE;
        if (stbuf->st_size > 0) stbuf->st_blocks = 1 + (stbuf->st_size - 1) / 512; // man 2 stat
    }
    return 0;
}

static int vram_readlink(const char *path, char *buf, size_t size) {
    GET_ENTRY_OR_RETURN(TYPE_SYMLINK);
    entry.atime(util::time());
    [[assume(size > 0)]];
    const size_t end{string_view{dynamic_cast<entry::symlink_t*>(&entry)->target}.copy(buf, size - 1)};
    buf[end] = '\0';
    return 0;
}

static int vram_chmod(const char *path, mode_t mode, fuse_file_info *) {
    GET_ENTRY_OR_RETURN(TYPE_FILE | TYPE_DIR);
    entry.mode(mode);
    return 0;
}

static int vram_chown(const char *path, uid_t user, gid_t group, fuse_file_info *) {
    GET_ENTRY_OR_RETURN(TYPE_FILE | TYPE_DIR);
    entry.chown(user, group);
    return 0;
}

static int vram_utimens(const char *path, const timespec tv[2], fuse_file_info *) {
    GET_ENTRY_OR_RETURN(TYPE_FILE | TYPE_DIR);
    entry.atime(tv[0]);
    entry.mtime(tv[1]);
    return 0;
}

static int vram_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t, fuse_file_info *, fuse_readdir_flags) {
    GET_ENTRY_OR_RETURN(TYPE_DIR);
    const auto dir{dynamic_cast<entry::dir_t*>(&entry)};
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);
    const shared_lock lock{dir->children_rwlock};
    dir->atime(util::time());
    for (const auto &key : dir->_children | views::keys) {
        filler(buf, key.c_str(), nullptr, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

static void chown_from_fuse_context(entry::entry_t &entry) {
    const fuse_context *const context{fuse_get_context()};
    entry.chown(context->uid, context->gid);
}

#define INCREMENT_ENTRY_COUNT_OR_RETURN() \
    if (entry::entry_count.fetch_add(1, memory_order_relaxed) >= entry::MAX_ENTRY_COUNT) { \
        entry::entry_count.fetch_sub(1, memory_order_relaxed); \
        return -ENOSPC; \
    } \
    do {} while (0)

#define PREP_CREAT_MUT_ENTRY_OR_RETURN(exist_ok) \
    const string_view path{path_}; \
    shared_ptr<entry::entry_t> entry_; \
    entry::namei_w_lock w_lock{}; \
    const int err{entry::namei(path, &w_lock, entry_)}; \
    if (!w_lock.parent) return err; \
    if (!(exist_ok) && entry_) return -EEXIST; \
    do {} while (0)

#define LN_ENTRY(entry_) \
    w_lock.parent->_children[string{path.substr(path.rfind('/') + 1)}] = (entry_); \
    w_lock.parent->mtime(util::time()); \
    do {} while (0)

#define LN_NEW_ENTRY(entry_) \
    chown_from_fuse_context(*(entry_)); \
    LN_ENTRY((entry_))

#define UNLINK_ENTRY() \
    w_lock.parent->_children.erase(w_lock.parent->_children.find(path.substr(path.rfind('/') + 1))); \
    w_lock.parent->mtime(util::time()); \
    do {} while (0)

static int vram_create(const char *path_, mode_t mode, fuse_file_info *fi) {
    INCREMENT_ENTRY_COUNT_OR_RETURN();
    PREP_CREAT_MUT_ENTRY_OR_RETURN(/* .open handles existing files */ false);
    entry_ = make_shared<entry::file_t>(mode);
    LN_NEW_ENTRY(entry_);
    fi->fh = reinterpret_cast<uint64_t>(new shared_ptr{dynamic_pointer_cast<entry::file_t>(std::move(entry_))});
    return 0;
}

static int vram_mkdir(const char *path_, mode_t mode) {
    INCREMENT_ENTRY_COUNT_OR_RETURN();
    PREP_CREAT_MUT_ENTRY_OR_RETURN(false);
    entry_ = make_shared<entry::dir_t>(mode);
    LN_NEW_ENTRY(entry_);
    return 0;
}

static int vram_symlink(const char *target, const char *path_) {
    INCREMENT_ENTRY_COUNT_OR_RETURN();
    PREP_CREAT_MUT_ENTRY_OR_RETURN(false);
    entry_ = make_shared<entry::symlink_t>(target);
    LN_NEW_ENTRY(entry_);
    return 0;
}

static int vram_unlink_impl(const string_view path, const entry::entry_t& entry, const entry::namei_w_lock &w_lock) {
    if (!(entry.type & (TYPE_FILE | TYPE_SYMLINK))) return -EISDIR;
    UNLINK_ENTRY();
    return 0;
}

static int vram_unlink(const char *path_) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
    if (!entry_) return -ENOENT;
    return vram_unlink_impl(path, *entry_, w_lock);
}

static int vram_rmdir_impl(const string_view path, entry::entry_t& entry, const entry::namei_w_lock &w_lock) {
    if (~entry.type & TYPE_DIR) return -ENOTDIR;
    auto &dir{dynamic_cast<entry::dir_t&>(entry)};
    const shared_lock lock{dir.children_rwlock};
    if (!dir._children.empty()) return -ENOTEMPTY;
    UNLINK_ENTRY();
    return 0;
}

static int vram_rmdir(const char *path_) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
    if (!entry_) return -ENOENT;
    return vram_rmdir_impl(path, *entry_, w_lock);
}

// Approach #3: Acquire locks repeatedly, and use placeholders. This is deadlock-free, atomic, and avoids cycles.
// This is complex enough. Reimplementing namei would be involved in potentially avoiding the locks sometimes.
static mutex parent_dirs_rename_mutex{};

static int vram_rename(const char *old_path, const char *new_path, unsigned int flags) {
    if (flags & ~RENAME_NOREPLACE) return -EINVAL;
    // Kernel already handles error code for mountpoint dir and non-racy nested dir cycles
    { // Quality-of-life only improvement to return specific ENOENT instead of EINVAL error code
        const auto path{old_path};
        GET_ENTRY_OR_RETURN(TYPE_ALL);
    }
    const unique_lock parent_dirs_rename_lock{parent_dirs_rename_mutex};
    { // Add placeholder to avoid a race causing us to need to remove entries
        const auto path_{new_path};
        PREP_CREAT_MUT_ENTRY_OR_RETURN(flags & RENAME_NOREPLACE);
        if (!entry_) {
            // OK
        } else if (entry_->type & TYPE_DIR) {
            if (const int err_{vram_rmdir_impl(path, *entry_, w_lock)}) return err_;
        } else if (entry_->type & (TYPE_FILE | TYPE_SYMLINK)) {
            if (const int err_{vram_unlink_impl(path, *entry_, w_lock)}) return err_;
        } else { // TYPE_RENAME_PLACEHOLDER
            return -EPERM;
        }
        LN_ENTRY(entry::RENAME_PLACEHOLDER_INSTANCE);
    }
    {
        shared_ptr<entry::entry_t> movend;
        const int unlink_err{
            [&] {
                const auto path_{old_path};
                PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
                if (!entry_) return -ENOENT;
                if (!(entry_->type & TYPE_ALL)) return -EPERM; // TYPE_RENAME_PLACEHOLDER
                movend = std::move(entry_);
                UNLINK_ENTRY();
                return 0;
            }()
        };
        const auto path_{new_path};
        PREP_CREAT_MUT_ENTRY_OR_RETURN(true); // Should always succeed and return our placeholder as entry_
        if (unlink_err) {
            UNLINK_ENTRY();
            return unlink_err == -ENOENT ? -EINVAL : unlink_err;
        }
        LN_ENTRY(movend); // Overwrite placeholder
    }
    return 0;
}

static int vram_open(const char *path, fuse_file_info *fi) {
    GET_ENTRY_OR_RETURN(TYPE_FILE);
    auto file{dynamic_pointer_cast<entry::file_t>(std::move(entry_))};
    if (fi->flags & O_TRUNC && fi->flags & (O_WRONLY | O_RDWR)) file->truncate(0);
    fi->fh = reinterpret_cast<uint64_t>(new shared_ptr{std::move(file)});
    return 0;
}

static int vram_read(const char *path, char *buf, size_t size, off_t off, fuse_file_info *fi) {
    return (*reinterpret_cast<shared_ptr<entry::file_t>*>(fi->fh))->read(off, size, buf);
}

static int vram_write(const char *path, const char *buf, size_t size, off_t off, fuse_file_info *fi) {
    return (*reinterpret_cast<shared_ptr<entry::file_t>*>(fi->fh))->write(off, size, buf);
}

static int vram_fsync(const char *path, int, fuse_file_info *fi) {
    // Should be virtually unneeded with VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    return 0;
}

static int vram_release(const char *path, fuse_file_info *fi) {
    // Although I worry about a race between release() and read()/write(),
    // the previously-here lock_guard wouldn't've prevented all instances of it
    delete reinterpret_cast<shared_ptr<entry::file_t>*>(fi->fh);
    return 0;
}

static int vram_truncate(const char *path, off_t size, fuse_file_info *) {
    GET_ENTRY_OR_RETURN(TYPE_FILE);
    return dynamic_cast<entry::file_t&>(entry).truncate(size);
}

static constexpr fuse_operations operations{
    .getattr = vram_getattr,
    .readlink = vram_readlink,
    .mkdir = vram_mkdir,
    .unlink = vram_unlink,
    .rmdir = vram_rmdir,
    .symlink = vram_symlink,
    .rename = vram_rename,
    .chmod = vram_chmod,
    .chown = vram_chown,
    .truncate = vram_truncate,
    .open = vram_open,
    .read = vram_read,
    .write = vram_write,
    .statfs = vram_statfs,
    .release = vram_release,
    .fsync = vram_fsync,
    .readdir = vram_readdir,
    .init = vram_init,
    .create = vram_create,
    .utimens = vram_utimens,
};

static int print_help() {
    cerr << R"(usage: vvramfs <mountdir> <size> [-d <device>] [-f]

  mountdir    - directory to mount file system, must be empty
  size        - size of the disk in bytes
  -d <device> - specifies identifier of device to use
  -f          - flag that forces mounting, with a smaller size if needed

The size may be followed by one of the following multiplicative suffixes:
K=1024, KB=1000, M=1024*1024, MB=1000*1000, G=1024*1024*1024, GB=1000*1000*1000.
It's rounded up to the nearest multiple of the block size.
)";
    return 1;
}

static size_t parse_size(const string &param) {
    size_t size{};
    const auto [ptr, ec] = from_chars(param.data(), param.data() + param.size(), size);
    if (ec != errc{}) abort();
    if (const string_view rest{ptr}; rest == "K") size *= 1024UL;
    else if (rest == "KB") size *= 1000UL;
    else if (rest == "M") size *= 1024UL * 1024UL;
    else if (rest == "MB") size *= 1000UL * 1000UL;
    else if (rest == "G") size *= 1024UL * 1024UL * 1024UL;
    else if (rest == "GB") size *= 1000UL * 1000UL * 1000UL;
    return size;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 6) return print_help();
    if (argc == 4 && strcmp(argv[3], "-f") != 0) return print_help();
    if (argc == 5 && strcmp(argv[3], "-d") != 0) return print_help();
    if (argc == 6 && strcmp(argv[3], "-d") != 0 && strcmp(argv[5], "-f") != 0) return print_help();
    filesystem::create_directories(argv[1]);
    const size_t disk_size{parse_size(argv[2])};
    // bool force_allocate = (argc == 4 || argc == 6);
    if (argc == 5 || argc == 6) {
        istringstream iss{argv[4]};
        if (!(iss >> memory::device_num) || !iss.eof()) abort();
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE)) cerr << "failed to lock process pages in memory, vramfs may freeze if swapped" << endl;
    cout << "allocating vram..." << endl;
    memory::gpu_init(disk_size);
    fuse_args args = FUSE_ARGS_INIT(2, argv);
    fuse_opt_parse(&args, nullptr, nullptr, nullptr);
    fuse_opt_add_arg(&args, "-oauto_unmount"); // Properly unmount even on crash
    fuse_opt_add_arg(&args, "-odefault_permissions"); // Let FUSE and the kernel deal with permissions handling
    fuse_opt_add_arg(&args, "-f"); // GPU driver acts funky if program doesn't keep running in foreground
#ifndef NDEBUG
    fuse_opt_add_arg(&args, "-s");
#endif
    return fuse_main(args.argc, args.argv, &operations, nullptr);
}

#define FUSE_USE_VERSION 30
#include <cstring>
#include <filesystem>
#include <fuse.h>
#include <iostream>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>

#include "entry.hpp"
#include "free_interposer.hpp"
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
    return 0;
}

static void* vram_init(fuse_conn_info *conn, fuse_config *cfg) {
    cfg->direct_io = true;
    if (vram_init_impl()) abort();
    cout << "mounted." << endl;
    return nullptr;
}

static int vram_statfs(const char *, struct statvfs *vfs) {
    vfs->f_bsize = memory::block::SIZE;
    vfs->f_blocks = memory::_pool_size;
    vfs->f_bfree = vfs->f_bavail = memory::_pool_available.load(memory_order_relaxed);
    vfs->f_files = entry::entry_count.load(memory_order_relaxed);
    vfs->f_ffree = std::numeric_limits<fsfilcnt_t>::max();
    vfs->f_namemax = std::numeric_limits<unsigned long>::max();
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
    strncpy(buf, dynamic_cast<entry::symlink_t*>(&entry)->target.c_str(), size);
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

#define PREP_CREAT_MUT_ENTRY_OR_RETURN(exist_ok) \
    const string_view path{path_}; \
    shared_ptr<entry::entry_t> entry_; \
    entry::namei_w_lock w_lock; \
    const int err{entry::namei(path, &w_lock, entry_)}; \
    if (!w_lock.parent) return err; \
    if (!(exist_ok) && entry_) return -EEXIST; \
    do {} while (0)

#define LN_ENTRY() \
    chown_from_fuse_context(*entry_); \
    w_lock.parent->_children[string{path.substr(path.rfind('/') + 1)}] = entry_; \
    w_lock.parent->mtime(util::time()); \
    do {} while (0)

#define UNLINK_ENTRY() \
    w_lock.parent->_children.erase(w_lock.parent->_children.find(path.substr(path.rfind('/') + 1))); \
    w_lock.parent->mtime(util::time()); \
    do {} while (0)

static int vram_create(const char *path_, mode_t, struct fuse_file_info *fi) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(~fi->flags & O_EXCL);
    if (entry_ && entry_->type != TYPE_FILE) return -EISDIR;
    entry_ = make_shared<entry::file_t>();
    LN_ENTRY();
    fi->fh = reinterpret_cast<uint64_t>(new shared_ptr{dynamic_pointer_cast<entry::file_t>(std::move(entry_))});
    return 0;
}

static int vram_mkdir(const char *path_, mode_t) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(false);
    entry_ = make_shared<entry::dir_t>();
    LN_ENTRY();
    return 0;
}

static int vram_symlink(const char *target, const char *path_) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(false);
    entry_ = make_shared<entry::symlink_t>(target);
    LN_ENTRY();
    return 0;
}

static int vram_unlink(const char *path_) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
    if (!entry_) return -ENOENT;
    if (!(entry_->type & (TYPE_FILE | TYPE_SYMLINK))) return -EISDIR;
    UNLINK_ENTRY();
    return 0;
}

static int vram_rmdir(const char *path_) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
    if (!entry_) return -ENOENT;
    if (~entry_->type & TYPE_DIR) return -ENOTDIR;
    entry::dir_t &dir{*dynamic_cast<entry::dir_t*>(&*entry_)};
    const shared_lock lock{dir.children_rwlock};
    if (!dir._children.empty()) return -ENOTEMPTY;
    UNLINK_ENTRY();
    return 0;
}

static int vram_rename_finish(shared_ptr<entry::entry_t> &movend, const char *path_, bool no_replace) {
    PREP_CREAT_MUT_ENTRY_OR_RETURN(!no_replace);
    if (entry_ && entry_->type != TYPE_FILE) return -EISDIR;
    entry_ = std::move(movend);
    LN_ENTRY();
    return 0;
}

static int vram_rename(const char *old_path, const char *new_path, unsigned int flags) {
    if (flags & ~RENAME_NOREPLACE) return -EINVAL;
    {
        // Early check to return better error code
        // Kernel already handles error code for mountpoint dir and non-racy nested dir cycles
        const auto path_{new_path};
        PREP_CREAT_MUT_ENTRY_OR_RETURN(false);
    }
    shared_ptr<entry::entry_t> movend;
    {
        const auto path_{old_path};
        PREP_CREAT_MUT_ENTRY_OR_RETURN(true);
        if (!entry_) return -ENOENT;
        UNLINK_ENTRY();
        movend = std::move(entry_);
    }
    if (const int err{vram_rename_finish(movend, new_path, flags & RENAME_NOREPLACE)}) {
        if (vram_rename_finish(movend, old_path, true)) {
            cerr << "Dangling entry after failed rename\n";
        }
        return err == -ENOENT ? -EINVAL : err;
    }
    return 0;
    // This isn't atomic but I'd rather not duplicate namei
    // But server-side, it's safer than the previous code allowing racily moving directories into themselves
}

static int vram_open(const char *path, fuse_file_info *fi) {
    GET_ENTRY_OR_RETURN(TYPE_FILE);
    auto file{dynamic_pointer_cast<entry::file_t>(std::move(entry_))};
    if (fi->flags & O_TRUNC && fi->flags & (O_WRONLY | O_RDWR)) file->truncate(0);
    fi->fh = reinterpret_cast<uint64_t>(new shared_ptr{std::move(file)});
    return 0;
}

int vram_read_buf(const char *, fuse_bufvec **bufp, size_t size, off_t off, fuse_file_info *fi) {
    const auto buf{static_cast<fuse_bufvec *>(malloc(sizeof(fuse_bufvec)))};
    if (!buf || size > FREE_INTERPOSER_BUF_SIZE) abort();
#if 0
    // writev(2) from my mmap-ed buffer is inexplicably 2.7 GB/s without any GPU operations
    // while the same short-circuit path with malloc is 6 GB/s. Somehow adding a copying step makes it (the kernel?) faster
    const auto mem{static_cast<char*>(free_interposer_malloc2())};
#else
    const auto mem{static_cast<char*>(malloc(size))};
#endif
    if (!mem) abort();
    *buf = FUSE_BUFVEC_INIT(size);
    // printf("%p\n", mem);
    buf->buf[0].mem = mem;
    *bufp = buf;
    // printf("%s %lu %lu %lu\n", path, size, off, (size_t)mem & 0xfff);
#if 1
    const int ret{(*reinterpret_cast<shared_ptr<entry::file_t>*>(fi->fh))->read(off, size, mem)};
#else
    int ret = size;
#endif
    if (ret >= 0) buf->buf[0].size = ret;
    return ret;
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
    .write = vram_write,
    .statfs = vram_statfs,
    .release = vram_release,
    .fsync = vram_fsync,
    .readdir = vram_readdir,
    .init = vram_init,
    .create = vram_create,
    .utimens = vram_utimens,
    .read_buf = vram_read_buf,
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

void *testptr;
int main(int argc, char *argv[]) {
    free(testptr);
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
    fuse_opt_add_arg(&args, "-s");
    fuse_opt_add_arg(&args, "-oauto_unmount"); // Properly unmount even on crash
    fuse_opt_add_arg(&args, "-odefault_permissions"); // Let FUSE and the kernel deal with permissions handling
    fuse_opt_add_arg(&args, "-f"); // GPU driver acts funky if program doesn't keep running in foreground
    return fuse_main(args.argc, args.argv, &operations, nullptr);
}

#define FUSE_USE_VERSION 30
#include <cstring>
#include <filesystem>
#include <fuse.h>
#include <iostream>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>

#include "vramfs.hpp"
#include "util.hpp"

using namespace std;
using namespace vram;
using enum entry::type_t;

/// File system root that links to the rest
static shared_ptr<entry::dir_t> root_entry;

static void* vram_init(fuse_conn_info *conn, fuse_config *cfg) {
    cfg->direct_io = true;
    root_entry = entry::dir_t::make(nullptr, "");
    root_entry->chown(geteuid(), getegid());
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
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry);
    if (err != 0) return err;
    *stbuf = {};
    const size_t size{entry->_size.load(memory_order_relaxed)};
    if (entry->type == TYPE_DIR) {
        stbuf->st_mode = S_IFDIR | entry->_mode.load(memory_order_acquire);
        stbuf->st_nlink = 2;
    } else if (entry->type == TYPE_FILE) {
        stbuf->st_mode = S_IFREG | entry->_mode.load(memory_order_acquire);
        stbuf->st_nlink = 1;
        stbuf->st_blksize = memory::block::SIZE;
        if (size > 0) {
            stbuf->st_blocks = static_cast<blkcnt_t>(1 + (size - 1) / 512); // man 2 stat
        }
    } else {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
    }
    stbuf->st_uid = entry->_user.load(memory_order_acquire);
    stbuf->st_gid = entry->_group.load(memory_order_acquire);
    stbuf->st_size = static_cast<off_t>(size);
    stbuf->st_atim = entry->_atime.load(memory_order_relaxed);
    stbuf->st_mtim = entry->_mtime.load(memory_order_relaxed);
    stbuf->st_ctim = entry->_ctime.load(memory_order_relaxed);
    return 0;
}

static int vram_readlink(const char *path, char *buf, size_t size) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_SYMLINK);
    if (err != 0) return err;
    auto symlink = dynamic_pointer_cast<entry::symlink_t>(entry);
    // TODO atime
    strncpy(buf, symlink->target.c_str(), size);
    return 0;
}

static int vram_chmod(const char *path, mode_t mode, fuse_file_info *) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE | TYPE_DIR);
    if (err != 0) return err;
    entry->mode(mode);
    return 0;
}

static int vram_chown(const char *path, uid_t user, gid_t group, fuse_file_info *) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE | TYPE_DIR);
    if (err != 0) return err;
    entry->chown(user, group);
    return 0;
}

static int vram_utimens(const char *path, const timespec tv[2], fuse_file_info *) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE | TYPE_DIR);
    if (err != 0) return err;
    entry->atime(tv[0]);
    entry->mtime(tv[1]);
    return 0;
}

static int vram_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t, fuse_file_info *, fuse_readdir_flags) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_DIR);
    if (err != 0) return err;
    auto dir = dynamic_pointer_cast<entry::dir_t>(entry);
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);
    const shared_lock lock{dir->children_rwlock};
    dir->atime(util::time());
    for (const auto &key : dir->_children | views::keys) {
        filler(buf, key.c_str(), nullptr, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

[[deprecated]] static mutex unlink_mutex{};

static int vram_create(const char *path, mode_t, struct fuse_file_info *fi) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE);
    if (err == -EISDIR) return err;
    // TODO EEXIST
    else if (err == 0) entry->unlink();
    string dir, name;
    util::split_file_path(path, dir, name);
    err = root_entry->find(dir, entry, TYPE_DIR);
    if (err != 0) return err;
    // TODO acquire children_rwlock unique_lock
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);
    auto file = entry::file_t::make(parent.get(), name);
    auto context = fuse_get_context();
    file->chown(context->uid, context->gid);
    fi->fh = reinterpret_cast<uint64_t>(new file_session(file));
    return 0;
}

static int vram_mkdir(const char *path, mode_t) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry);
    if (err == 0) return -EEXIST;
    string dir, name;
    util::split_file_path(path, dir, name);
    err = root_entry->find(dir, entry, TYPE_DIR);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);
    auto new_dir = entry::dir_t::make(parent.get(), name);
    auto context = fuse_get_context();
    new_dir->chown(context->uid, context->gid);
    return 0;
}

static int vram_symlink(const char *target, const char *path) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry);
    if (err == 0) return -EEXIST;
    string dir, name;
    util::split_file_path(path, dir, name);
    err = root_entry->find(dir, entry, TYPE_DIR);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);
    auto symlink = entry::symlink_t::make(parent.get(), name, target);
    auto context = fuse_get_context();
    symlink->chown(context->uid, context->gid);
    return 0;
}

static int vram_unlink(const char *path) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE | TYPE_SYMLINK);
    if (err != 0) return err;
    entry->unlink();
    return 0;
}

static int vram_rmdir(const char *path) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_DIR);
    if (err != 0) return err;
    auto dir = dynamic_pointer_cast<entry::dir_t>(entry);
    {
        const shared_lock lock{dir->children_rwlock};
        dir->atime(util::time());
        if (!dir->_children.empty()) return -ENOTEMPTY;
    }
    dir->unlink();
    return 0;
}

static int vram_rename(const char *path, const char *new_path, unsigned int) {
    lock_guard<mutex> local_lock{unlink_mutex};
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry);
    if (err != 0) return err;
    string dir, new_name;
    util::split_file_path(new_path, dir, new_name);
    shared_ptr<entry::entry_t> parent_entry;
    err = root_entry->find(dir, parent_entry, TYPE_DIR);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(parent_entry);
    shared_ptr<entry::entry_t> dest_entry;
    err = root_entry->find(new_path, dest_entry);
    // TODO RENAME_NOREPLACE
    if (err == 0) dest_entry->unlink();
    entry->move(parent.get(), new_name);
    return 0;
}

static int vram_open(const char *path, fuse_file_info *fi) {
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE);
    if (err != 0) return err;
    auto file = dynamic_pointer_cast<entry::file_t>(entry);
    if (fi->flags & O_TRUNC && fi->flags & (O_WRONLY | O_RDWR)) file->truncate(0);
    fi->fh = reinterpret_cast<uint64_t>(new file_session(file));
    return 0;
}

static int vram_read(const char *path, char *buf, size_t size, off_t off, fuse_file_info *fi) {
    // printf("%s %lu %lu %lu\n", path, size, off, (size_t)buf & 0xfff);
    shared_ptr<entry::file_t> file{reinterpret_cast<file_session*>(fi->fh)->file};
    return file->read(off, size, buf);
}

static int vram_write(const char *path, const char *buf, size_t size, off_t off, fuse_file_info *fi) {
    shared_ptr<entry::file_t> file{reinterpret_cast<file_session*>(fi->fh)->file};
    return file->write(off, size, buf);
}

static int vram_fsync(const char *path, int, fuse_file_info *fi) {
    // Unneeded with VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    return 0;
}

static int vram_release(const char *path, fuse_file_info *fi) {
    // Although I worry about a race between release() and read()/write(),
    // the previously-here lock_guard wouldn't've prevented all instances of it
    delete reinterpret_cast<file_session*>(fi->fh);
    fi->fh = 0;
    return 0;
}

static int vram_truncate(const char *path, off_t size, fuse_file_info *) {
    shared_ptr<entry::file_t> file;
    shared_ptr<entry::entry_t> entry;
    int err = root_entry->find(path, entry, TYPE_FILE);
    if (err != 0) return err;
    file = dynamic_pointer_cast<entry::file_t>(entry);
    return file->truncate(size);
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
    if (argc == 5 || argc == 6) memory::device_num = atoi(argv[4]);
    if (mlockall(MCL_CURRENT | MCL_FUTURE)) cerr << "failed to lock process pages in memory, vramfs may freeze if swapped" << endl;
    cout << "allocating vram..." << endl;
    memory::gpu_init(disk_size);
    fuse_args args = FUSE_ARGS_INIT(2, argv);
    fuse_opt_parse(&args, nullptr, nullptr, nullptr);
    fuse_opt_add_arg(&args, "-oauto_unmount"); // Properly unmount even on crash
    fuse_opt_add_arg(&args, "-odefault_permissions"); // Let FUSE and the kernel deal with permissions handling
    fuse_opt_add_arg(&args, "-f"); // GPU driver acts funky if program doesn't keep running in foreground
    return fuse_main(args.argc, args.argv, &operations, nullptr);
}

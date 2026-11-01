#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram::entry {
atomic_int entry_count = 0;

entry_t::entry_t(const type_t type_, const size_t size_, const mode_t mode_)
    : type{type_}, _size{size_}, _mode{mode_ & 0777}, btime{util::time()}, _atime{btime}, _mtime{btime}, _ctime{btime} {}

entry_t::~entry_t() {
    // fetch_add is in INCREMENT_ENTRY_COUNT_OR_RETURN()
    entry_count.fetch_sub(1, memory_order_relaxed);
}

void entry_t::atime(timespec t) {
    _atime.store(t, memory_order_relaxed);
    ctime(util::time());
}

void entry_t::mtime(timespec t) {
    _mtime.store(t, memory_order_relaxed);
    ctime(util::time());
}

void entry_t::ctime(timespec t) {
    _ctime.store(t, memory_order_relaxed);
}

void entry_t::mode(mode_t mode) {
    _mode.store(mode & 0777, memory_order_release);
    ctime(util::time());
}

void entry_t::chown(uid_t user, gid_t group) {
    if (~user) _user.store(user, memory_order_release);
    if (~group) _group.store(group, memory_order_release);
    ctime(util::time());
}
}

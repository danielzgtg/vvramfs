#include <atomic>

#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram {
    namespace entry {
        atomic_int entry_count = 0;

        entry_t::entry_t(type_t type_, size_t size_, mode_t mode_)
            : type{type_}, _size{size_}, _mode{mode_ & 0777}, btime{util::time()}, _atime{btime}, _mtime{btime}, _ctime{btime} {
            entry_count.fetch_add(1, memory_order_relaxed);
        }

        entry_t::~entry_t() {
            entry_count.fetch_sub(1, memory_order_relaxed);
        }

        void entry_t::link(dir_t* parent, const string& name) {
            _parent = parent;
            _name = name;
            if (parent) {
                const unique_lock lock{parent->children_rwlock};
                parent->_children[name] = shared_from_this();
                parent->mtime(util::time());
            }
            ctime(util::time());
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
            _user.store(user, memory_order_release);
            _group.store(group, memory_order_release);
            ctime(util::time());
        }

        void entry_t::unlink() {
            if (_parent) {
                const unique_lock lock{_parent->children_rwlock};
                _parent->_children.erase(_name);
                _parent->mtime(util::time());
                _parent = nullptr;
            }
        }

        void entry_t::move(dir_t* new_parent, const string& new_name) {
            unlink();
            link(new_parent, new_name);
        }
    }
}

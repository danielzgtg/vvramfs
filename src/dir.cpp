#include <ranges>
#include <utility>

#include "entry.hpp"

using namespace std;

namespace vram::entry {
dir_t::dir_t(mode_t mode) : entry_t{TYPE_DIR, 4096, mode} {}

dir_t::~dir_t() {
    if (_size.load(memory_order_relaxed) == 0x746f6f72) { // mountpoint root dir
        shared_lock lock{children_rwlock};
        for (const auto &child : _children | views::values) {
            new shared_ptr{child}; // Must skip destructor at shutdown to avoid race with GPU destruction
        }
    }
}

static const auto root_entry{make_shared<dir_t>(0755)};

int namei(string_view path, namei_w_lock *w_lock, shared_ptr<entry_t> &result) {
    path.remove_prefix(1);
    if (path.empty()) {
        result = root_entry;
        return 0;
    }
    dir_t *parent{&*root_entry};
    shared_lock<shared_mutex> r_lock_prev{}; // replaces shared_ptr
    shared_lock<shared_mutex> r_lock_cur{}; // guards _children
    for (;;) {
        r_lock_prev = std::move(r_lock_cur);
        const size_t sep_i{path.find('/')};
        string_view current_component;
        if (!~sep_i) {
            current_component = path;
            if (w_lock) {
                w_lock->w_lock = unique_lock{parent->children_rwlock};
                // Only empty directories can be removed, and that check will wait for w_lock, so don't need r_lock_prev
                r_lock_prev = {};
                w_lock->parent = parent;
            } else {
                r_lock_cur = shared_lock{parent->children_rwlock};
            }
        } else {
            current_component = path.substr(0, sep_i);
            path.remove_prefix(sep_i + 1);
            r_lock_cur = shared_lock{parent->children_rwlock};
        }
        const auto it = parent->_children.find(current_component);
        if (it == parent->_children.end()) return -ENOENT;
        const shared_ptr<entry_t> &current = it->second;
        if (!~sep_i) {
            result = current;
            return 0;
        }
        parent = dynamic_cast<dir_t*>(&*current);
        if (!parent) return -ENOTDIR;
    }
}
}

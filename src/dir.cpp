#include <utility>

#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram::entry {
dir_t::dir_t() : entry_t{TYPE_DIR, 4096, 0755} {}

static const shared_ptr<dir_t> root_entry{make_shared<dir_t>()};

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
                if (parent->_children.empty()) w_lock->r_lock_prev = std::move(r_lock_prev);
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

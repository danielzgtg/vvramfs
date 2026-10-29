#include <sstream>

#include "entry.hpp"

using namespace std;

namespace vram::entry {
shared_ptr<dir_t> dir_t::make(dir_t* parent, const string& name) {
    auto dir = shared_ptr<dir_t>(new dir_t());
    dir->link(parent, name);
    return dir;
}

dir_t::dir_t() : entry_t{TYPE_DIR, 4096, 0755} {}

int dir_t::find(const string& path, shared_ptr<entry_t>& entry, int filter) const {
    if ((filter & TYPE_ALL) == 0) return -ENOENT;
    entry = const_pointer_cast<entry_t>(shared_from_this());
    stringstream stream(path.substr(1));
    string part;
    while (getline(stream, part, '/')) {
        if (entry->type != TYPE_DIR) return -ENOTDIR;
        auto dir = dynamic_pointer_cast<dir_t>(entry);
        const shared_lock lock{dir->children_rwlock};
        auto it = dir->_children.find(part);
        if (it != dir->_children.end()) {
            entry = it->second;
        } else {
            return -ENOENT;
        }
    }
    if (!(entry->type & filter)) {
        if (entry->type == TYPE_FILE) {
            if (filter & TYPE_SYMLINK) return -ENOENT;
            if (filter & TYPE_DIR) return -EISDIR;
        } else if (entry->type == TYPE_DIR) {
            if (filter & TYPE_FILE) return -ENOTDIR;
            if (filter & TYPE_SYMLINK) return -EPERM;
        } else {
            return -EPERM;
        }
    }
    return 0;
}
}

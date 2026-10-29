#include "entry.hpp"
#include "util.hpp"

#include <sstream>

using namespace std;

namespace vram {
    namespace entry {
        shared_ptr<dir_t> dir_t::make(dir_t* parent, const string& name) {
            auto dir = shared_ptr<dir_t>(new dir_t());
            dir->link(parent, name);
            return dir;
        }

        dir_t::dir_t() : entry_t{TYPE_DIR, 4096, 0755} {}

        int dir_t::find(const string& path, shared_ptr<entry_t>& entry, int filter) const {
            // If filter is empty, no entry will ever match
            if ((filter & TYPE_ALL) == 0) return -ENOENT;
            // Traverse file system by hierarchically, starting from this entry
            entry = std::const_pointer_cast<entry_t>(shared_from_this());
            std::stringstream stream(path.substr(1));
            string part;
            // If the path is empty, assume the root directory
            while (getline(stream, part, '/')) {
                // If current entry isn't a directory, abort
                if (entry->type != TYPE_DIR) return -ENOTDIR;
                // Navigate to next entry
                auto dir = std::dynamic_pointer_cast<dir_t>(entry);
                const shared_lock lock{dir->children_rwlock};
                auto it = dir->_children.find(part);
                if (it != dir->_children.end()) {
                    entry = it->second;
                } else {
                    return -ENOENT;
                }
            }
            // If an undesired type of entry was found, return an appropriate error
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
}

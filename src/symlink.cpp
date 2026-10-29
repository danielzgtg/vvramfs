#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram::entry {
shared_ptr<symlink_t> symlink_t::make(dir_t *parent, const string &name, const string &target) {
    auto symlink = shared_ptr<symlink_t>(new symlink_t(target));
    symlink->link(parent, name);
    return symlink;
}

symlink_t::symlink_t(const string &target) : entry_t{TYPE_SYMLINK, target.size(), 0777}, target{target} {
}
}

#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram::entry {
symlink_t::symlink_t(const string &target) : entry_t{TYPE_SYMLINK, target.size(), 0777}, target{target} {
}
}

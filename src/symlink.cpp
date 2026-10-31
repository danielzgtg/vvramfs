#include "entry.hpp"
#include "util.hpp"

using namespace std;

namespace vram::entry {
symlink_t::symlink_t(const string &target) : entry_t{TYPE_SYMLINK, target.size(), 0777}, target{target} {
}

rename_placeholder_t::rename_placeholder_t() : entry_t{TYPE_RENAME_PLACEHOLDER, 0, 0} {}

const shared_ptr<entry_t> RENAME_PLACEHOLDER_INSTANCE{make_shared<rename_placeholder_t>()};
}

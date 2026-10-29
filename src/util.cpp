#include "util.hpp"

using namespace std;

namespace vram::util {
timespec time() {
    timespec tv{};
    clock_gettime(CLOCK_REALTIME_COARSE, &tv);
    return tv;
}

void split_file_path(const string& path, string& dir, string& file) {
    size_t p = path.rfind('/');
    if (p == string::npos) {
        dir = "";
        file = path;
    } else {
        dir = path.substr(0, p);
        file = path.substr(p + 1);
    }
    if (dir.empty()) dir = "/";
}
}

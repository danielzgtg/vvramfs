#include "util.hpp"

using namespace std;

namespace vram::util {
timespec time() {
    timespec tv{};
    clock_gettime(CLOCK_REALTIME_COARSE, &tv);
    return tv;
}
}

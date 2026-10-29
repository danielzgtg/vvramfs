#pragma once

#include "entry.hpp"

namespace vram {
    // Data persistent in an open() and release() session
    struct file_session {
        shared_ptr<entry::file_t> file;

        file_session(shared_ptr<entry::file_t> file) : file(file) {}
    };
}

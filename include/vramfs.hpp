#pragma once

#include "entry.hpp"

namespace vram {
    // Data persistent in an open() and release() session
    struct file_session {
        entry::file_ref file;

        file_session(entry::file_ref file) : file(file) {}
    };
}

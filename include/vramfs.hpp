#pragma once

#include "entry.hpp"

namespace vram {
/// Data persistent in an open() and release() session
struct file_session {
    std::shared_ptr<entry::file_t> file;

    explicit file_session(std::shared_ptr<entry::file_t> file) : file(file) {}
};
}

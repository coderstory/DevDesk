// src/module_memory/engine/telemem_process_posix.cpp
// POSIX/macOS 布局：venv 解释器在 bin/python3
#include "telemem_process.h"

#include <filesystem>

namespace module::memory::engine {

std::filesystem::path venvPythonExe(const std::filesystem::path& venvDir) {
    return venvDir / "bin" / "python3";
}

} // namespace module::memory::engine

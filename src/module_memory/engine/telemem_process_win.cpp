// src/module_memory/engine/telemem_process_win.cpp
// Windows 布局：venv 解释器在 Scripts\python.exe
#include "telemem_process.h"

#include <filesystem>

namespace module::memory::engine {

std::filesystem::path venvPythonExe(const std::filesystem::path& venvDir) {
    return venvDir / "Scripts" / "python.exe";
}

} // namespace module::memory::engine

#pragma once

#include <string>

namespace YamlConfig {
    // Read a value at a dotted key path (e.g. "asr.doubao.app_key")
    std::string read(const std::string& yaml, const std::string& keyPath);

    // Write a value at a dotted key path; creates missing sections
    std::string write(const std::string& yaml, const std::string& keyPath, const std::string& value);
}

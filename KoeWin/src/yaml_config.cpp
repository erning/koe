#include "yaml_config.h"
#include <vector>
#include <sstream>

static std::vector<std::string> splitKeyPath(const std::string& keyPath) {
    std::vector<std::string> parts;
    std::istringstream ss(keyPath);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Remove trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static std::string joinLines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

static int indentLevel(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') n++;
        else break;
    }
    return n;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Extract the key from a trimmed YAML line (everything before first ':')
static std::string extractKey(const std::string& trimmed) {
    auto pos = trimmed.find(':');
    if (pos == std::string::npos) return "";
    return trim(trimmed.substr(0, pos));
}

// Extract the value from a trimmed YAML line (everything after first ':')
static std::string extractValue(const std::string& trimmed) {
    auto pos = trimmed.find(':');
    if (pos == std::string::npos) return "";
    std::string val = trim(trimmed.substr(pos + 1));

    // Handle quoted strings
    if (val.size() >= 2 && val.front() == '"') {
        auto closeQuote = val.find('"', 1);
        if (closeQuote != std::string::npos) {
            val = val.substr(0, closeQuote + 1);
        }
    } else {
        // Strip inline comments
        auto comment = val.find(" #");
        if (comment != std::string::npos) {
            val = trim(val.substr(0, comment));
        }
    }

    // Remove surrounding quotes
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
    }
    return val;
}

static bool needsQuoting(const std::string& value) {
    if (value.empty()) return true;
    for (char c : value) {
        if (c == ' ' || c == '#' || c == ':' || c == '"' || c == '$' || c == '@') return true;
    }
    if (value.find("://") != std::string::npos) return true;
    return false;
}

std::string YamlConfig::read(const std::string& yaml, const std::string& keyPath) {
    auto parts = splitKeyPath(keyPath);
    if (parts.empty()) return "";
    auto lines = splitLines(yaml);

    int matchedDepth = 0;
    int requiredIndent[16] = {};

    for (const auto& line : lines) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int indent = indentLevel(line);

        // Check if we've left a matched section
        while (matchedDepth > 0 && indent < requiredIndent[matchedDepth - 1] + 1) {
            matchedDepth--;
        }

        std::string lineKey = extractKey(trimmed);
        if (lineKey.empty()) continue;

        if (matchedDepth >= static_cast<int>(parts.size())) continue;
        const auto& expectedKey = parts[matchedDepth];

        if (lineKey == expectedKey) {
            if (matchedDepth == static_cast<int>(parts.size()) - 1) {
                return extractValue(trimmed);
            } else {
                requiredIndent[matchedDepth] = indent;
                matchedDepth++;
            }
        }
    }
    return "";
}

std::string YamlConfig::write(const std::string& yaml, const std::string& keyPath, const std::string& value) {
    auto parts = splitKeyPath(keyPath);
    if (parts.empty()) return yaml;

    const auto& key = parts.back();
    int sectionCount = static_cast<int>(parts.size()) - 1;

    // Quote if necessary
    std::string quotedValue = needsQuoting(value) ? ("\"" + value + "\"") : value;

    auto lines = splitLines(yaml);

    // Build indent string for the leaf key
    std::string indent(sectionCount * 2, ' ');

    // First pass: try to find and replace existing key
    int matchedDepth = 0;
    int requiredIndent[16] = {};

    for (size_t i = 0; i < lines.size(); i++) {
        std::string trimmed = trim(lines[i]);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int lineIndent = indentLevel(lines[i]);
        while (matchedDepth > 0 && lineIndent < requiredIndent[matchedDepth - 1] + 1) {
            matchedDepth--;
        }

        std::string lineKey = extractKey(trimmed);
        if (lineKey.empty()) continue;

        if (matchedDepth < sectionCount) {
            if (lineKey == parts[matchedDepth]) {
                requiredIndent[matchedDepth] = lineIndent;
                matchedDepth++;
            }
        } else if (matchedDepth == sectionCount) {
            if (lineKey == key) {
                // Replace this line
                lines[i] = indent + key + ": " + quotedValue;
                return joinLines(lines);
            }
        }
    }

    // Key not found — find insertion point and create missing sections
    matchedDepth = 0;
    int insertIdx = static_cast<int>(lines.size());
    memset(requiredIndent, 0, sizeof(requiredIndent));

    for (size_t i = 0; i < lines.size(); i++) {
        std::string trimmed = trim(lines[i]);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int lineIndent = indentLevel(lines[i]);
        while (matchedDepth > 0 && lineIndent < requiredIndent[matchedDepth - 1] + 1) {
            matchedDepth--;
        }

        std::string lineKey = extractKey(trimmed);
        if (lineKey.empty()) continue;

        if (matchedDepth < sectionCount) {
            if (lineKey == parts[matchedDepth]) {
                requiredIndent[matchedDepth] = lineIndent;
                matchedDepth++;

                if (matchedDepth == sectionCount) {
                    // Found all parent sections — find end of section
                    insertIdx = static_cast<int>(i) + 1;
                    while (insertIdx < static_cast<int>(lines.size())) {
                        std::string nextTrimmed = trim(lines[insertIdx]);
                        if (!nextTrimmed.empty() && nextTrimmed[0] != '#') {
                            int nextIndent = indentLevel(lines[insertIdx]);
                            if (nextIndent <= lineIndent) break;
                        }
                        insertIdx++;
                    }
                }
            }
        }
    }

    // Create missing parent sections
    for (int d = matchedDepth; d < sectionCount; d++) {
        std::string secIndent(d * 2, ' ');
        std::string secLine = secIndent + parts[d] + ":";
        lines.insert(lines.begin() + insertIdx, secLine);
        insertIdx++;
    }

    // Insert the leaf key
    std::string newLine = indent + key + ": " + quotedValue;
    lines.insert(lines.begin() + insertIdx, newLine);

    return joinLines(lines);
}

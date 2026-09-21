#pragma once

#include <string>

// "name 12" / "name{class=\"2xx\"} 3" の値を拾う。無ければ -1。
inline long long value_of(const std::string &body, const std::string &name) {
    std::size_t pos = 0;
    while ((pos = body.find(name, pos)) != std::string::npos) {
        const bool at_line_start = pos == 0 || body[pos - 1] == '\n';
        const auto eol = body.find('\n', pos);
        const auto line = body.substr(pos, eol - pos);
        if (at_line_start) {
            const auto sp = line.rfind(' ');
            if (sp != std::string::npos && line.compare(0, name.size(), name) == 0) {
                return std::stoll(line.substr(sp + 1));
            }
        }
        pos += name.size();
    }
    return -1;
}

#include "log_sanitize.h"

#include <cctype>
#include <cstring>

namespace {

void replaceAll(std::string& s, const char* from, const char* to) {
    const std::size_t fromLen = std::strlen(from);
    if (fromLen == 0)
        return;
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;)
        s.replace(pos, fromLen, to);
}

void stripWindowsPaths(std::string& s) {
    for (std::size_t i = 1; i + 1 < s.size();) {
        const char prev = s[i - 1];
        if (std::isalpha(static_cast<unsigned char>(prev)) && s[i] == ':' &&
            (s[i + 1] == '\\' || s[i + 1] == '/')) {
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != ' ' && s[j] != '\t')
                ++j;
            s.erase(i - 1, j - (i - 1));
            if (i > 1)
                --i;
            continue;
        }
        ++i;
    }
}

} // namespace

std::string sanitizeUiLogLine(std::string line) {
    replaceAll(line, "\xE2\x80\xA6", "...");
    replaceAll(line, "\xE2\x80\x94", "-");
    replaceAll(line, "\xE2\x80\x93", "-");
    replaceAll(line, "\xC2\xB7", "-");
    replaceAll(line, "\xc3\xa2\xe2\x82\xac\xc2\xa6", "...");
    replaceAll(line, "\xc3\xa2\xe2\x82\xac\xe2\x80\x9d", "-");
    replaceAll(line, "\xc3\xa2\xe2\x80\x93", "-");
    stripWindowsPaths(line);

    if (line.find("[debug]") != std::string::npos)
        return {};
    if (line.find("path=") != std::string::npos)
        return {};
    if (line.find("log=") != std::string::npos)
        return {};
    if (line.find("full log:") != std::string::npos)
        return {};

    return line;
}

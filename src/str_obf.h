#pragma once
#include <string>
#include <cstddef>

inline std::string xorDecode(const char* data, std::size_t len, char key) {
    std::string r(data, len);
    for (auto& c : r) c ^= key;
    return r;
}

inline std::wstring xorDecodeW(const wchar_t* data, std::size_t len, wchar_t key) {
    std::wstring r(data, len);
    for (auto& c : r) c ^= key;
    return r;
}

#define OBFW(str, key) xorDecodeW(L##str, sizeof(L##str)/sizeof(wchar_t)-1, key)
#define OBF(str, key)  xorDecode(str, sizeof(str)-1, key)

#include "payload_crypto.h"

#include <Windows.h>
#include <bcrypt.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr std::uint32_t kMagicCrme = 0x454D5243u; // CRME

bool base64Decode(const std::string& in, std::vector<std::uint8_t>& out) {
    out.clear();
    if (in.empty())
        return false;

    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::vector<std::uint8_t> buf;
    buf.reserve(in.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ')
            continue;
        const int8_t d = table[c];
        if (d < 0)
            return false;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            buf.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    out = std::move(buf);
    return out.size() == 32;
}

struct BcryptAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~BcryptAlg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct BcryptKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    std::vector<std::uint8_t> obj;
    ~BcryptKey() { if (h) BCryptDestroyKey(h); }
};

bool aesGcmDecrypt(const std::uint8_t* key, const std::uint8_t* nonce,
    const std::uint8_t* cipher, std::size_t cipherLen,
    const std::uint8_t* tag, std::vector<std::uint8_t>& plain) {
    BcryptAlg alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0)
        return false;

    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) < 0)
        return false;

    DWORD objLen = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
            sizeof(objLen), &cb, 0) < 0)
        return false;

    BcryptKey keyHandle;
    keyHandle.obj.resize(objLen);
    if (BCryptGenerateSymmetricKey(alg.h, &keyHandle.h, keyHandle.obj.data(), objLen,
            const_cast<PUCHAR>(key), 32, 0) < 0)
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(nonce);
    authInfo.cbNonce = 12;
    authInfo.pbTag = const_cast<PUCHAR>(tag);
    authInfo.cbTag = 16;

    plain.assign(cipherLen, 0);
    ULONG plainLen = 0;
    const NTSTATUS st = BCryptDecrypt(keyHandle.h,
        const_cast<PUCHAR>(cipher), static_cast<ULONG>(cipherLen),
        &authInfo, nullptr, 0, plain.data(), static_cast<ULONG>(plain.size()),
        &plainLen, 0);
    if (st < 0)
        return false;
    plain.resize(plainLen);
    return true;
}

bool base64DecodeAny(const std::string& in, std::vector<std::uint8_t>& out) {
    out.clear();
    if (in.empty())
        return false;

    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::vector<std::uint8_t> buf;
    buf.reserve(in.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ')
            continue;
        const int8_t d = table[c];
        if (d < 0)
            return false;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            buf.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    out = std::move(buf);
    return !out.empty();
}

} // namespace

bool payloadDecryptCrme(const std::uint8_t* data, std::size_t size,
    const std::string& payloadKeyBase64, std::vector<std::uint8_t>& outPlain,
    std::string& error) {
    outPlain.clear();
    if (size < 12 + 16 + 12 + 8) {
        error = "Encrypted payload is too small.";
        return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::memcpy(&magic, data, 4);
    std::memcpy(&version, data + 4, 4);
    if (magic != 0x454D5243u || version != 1) {
        error = "Encrypted payload header invalid.";
        return false;
    }

    const auto* nonce = data + 8;
    const auto* cipher = data + 20;
    const std::size_t cipherLen = size - 20 - 16;
    const auto* tag = data + size - 16;
    if (cipherLen == 0) {
        error = "Encrypted payload empty.";
        return false;
    }

    std::vector<std::uint8_t> key;
    if (!base64Decode(payloadKeyBase64, key)) {
        error = "Missing or invalid payload unlock key from server.";
        return false;
    }

    if (!aesGcmDecrypt(key.data(), nonce, cipher, cipherLen, tag, outPlain)) {
        error = "Payload decryption failed (wrong key or corrupt data).";
        return false;
    }
    return true;
}

bool payloadDecryptCrmeBytes(const std::uint8_t* data, std::size_t size,
    const std::uint8_t* key32, std::vector<std::uint8_t>& outPlain, std::string& error) {
    outPlain.clear();
    if (size < 12 + 16 + 12 + 8) {
        error = "Encrypted payload is too small.";
        return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::memcpy(&magic, data, 4);
    std::memcpy(&version, data + 4, 4);
    if (magic != 0x454D5243u || version != 1) {
        error = "Encrypted payload header invalid.";
        return false;
    }

    const auto* nonce = data + 8;
    const auto* cipher = data + 20;
    const std::size_t cipherLen = size - 20 - 16;
    const auto* tag = data + size - 16;
    if (cipherLen == 0) {
        error = "Encrypted payload empty.";
        return false;
    }

    if (!aesGcmDecrypt(key32, nonce, cipher, cipherLen, tag, outPlain)) {
        error = "Payload decryption failed (wrong key or corrupt data).";
        return false;
    }
    return true;
}

bool payloadUnwrapDek(const std::string& sessionKeyBase64, const std::string& dekWrapBase64,
    std::vector<std::uint8_t>& outDek, std::string& error) {
    outDek.clear();
    std::vector<std::uint8_t> sessionKey;
    if (!base64Decode(sessionKeyBase64, sessionKey)) {
        error = "Invalid session payload key.";
        return false;
    }

    std::vector<std::uint8_t> wrapped;
    if (!base64DecodeAny(dekWrapBase64, wrapped) || wrapped.size() < 12 + 16 + 1) {
        error = "Invalid dek_wrap from server.";
        return false;
    }

    const auto* nonce = wrapped.data();
    const std::size_t bodyLen = wrapped.size() - 12;
    const auto* cipher = wrapped.data() + 12;
    const auto* tag = wrapped.data() + wrapped.size() - 16;
    const std::size_t cipherLen = bodyLen - 16;

    if (!aesGcmDecrypt(sessionKey.data(), nonce, cipher, cipherLen, tag, outDek)) {
        error = "Failed to unwrap overlay key.";
        return false;
    }
    if (outDek.size() != 32) {
        error = "Unwrapped overlay key has wrong size.";
        outDek.clear();
        return false;
    }
    return true;
}

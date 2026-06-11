#include "launch_handshake.h"

#include <Windows.h>
#include <processthreadsapi.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <chrono>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr char kBuildPepper[] = "crymore.pw/launch/v1";

std::uint64_t unixNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool hmacSha256(const std::uint8_t* key, std::size_t keyLen,
    const std::uint8_t* data, std::size_t dataLen,
    std::uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return false;

    DWORD objLen = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
            sizeof(objLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    std::vector<std::uint8_t> obj(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen,
            const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);
    const bool ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

void deriveMacKeyFromHandshake(const LaunchHandshake& hs, std::uint8_t key[32]) {
    std::string material = std::string(hs.session_hint) + hs.hwid_hex;
    hmacSha256(reinterpret_cast<const std::uint8_t*>(kBuildPepper), sizeof(kBuildPepper) - 1,
        reinterpret_cast<const std::uint8_t*>(material.data()), material.size(), key);
}

void fillRandom(std::uint8_t* buf, std::size_t len) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (std::size_t i = 0; i < len; ++i)
        buf[i] = static_cast<std::uint8_t>(dist(gen));
}

void copyField(char* dst, std::size_t cap, const std::string& src) {
    if (cap == 0)
        return;
    std::memset(dst, 0, cap);
    std::strncpy(dst, src.c_str(), cap - 1);
}

std::string sha256HexSimple(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::uint8_t digest[32]{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};

    DWORD objLen = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
            sizeof(objLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    std::vector<std::uint8_t> obj(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), 0);
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

bool computeMac(const LaunchHandshake& hs, std::uint8_t mac[32]) {
    std::uint8_t key[32]{};
    deriveMacKeyFromHandshake(hs, key);

    std::vector<std::uint8_t> msg;
    msg.insert(msg.end(), hs.nonce, hs.nonce + sizeof(hs.nonce));
    msg.insert(msg.end(), reinterpret_cast<const std::uint8_t*>(&hs.expiry_unix),
        reinterpret_cast<const std::uint8_t*>(&hs.expiry_unix) + sizeof(hs.expiry_unix));
    msg.insert(msg.end(), reinterpret_cast<const std::uint8_t*>(&hs.parent_pid),
        reinterpret_cast<const std::uint8_t*>(&hs.parent_pid) + sizeof(hs.parent_pid));
    msg.insert(msg.end(), reinterpret_cast<const std::uint8_t*>(&hs.loader_pid),
        reinterpret_cast<const std::uint8_t*>(&hs.loader_pid) + sizeof(hs.loader_pid));
    const auto userLen = std::strlen(hs.username);
    msg.insert(msg.end(), hs.username, hs.username + userLen);
    const auto hwidLen = std::strlen(hs.hwid_hex);
    msg.insert(msg.end(), hs.hwid_hex, hs.hwid_hex + hwidLen);
    const auto planLen = std::strlen(hs.plan);
    msg.insert(msg.end(), hs.plan, hs.plan + planLen);
    msg.insert(msg.end(), reinterpret_cast<const std::uint8_t*>(&hs.sub_expires_unix),
        reinterpret_cast<const std::uint8_t*>(&hs.sub_expires_unix) + sizeof(hs.sub_expires_unix));
    const auto avatarLen = std::strlen(hs.avatar_url);
    msg.insert(msg.end(), hs.avatar_url, hs.avatar_url + avatarLen);

    return hmacSha256(key, sizeof(key), msg.data(), msg.size(), mac);
}

DWORD getParentProcessId(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD parent = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}

bool isLoaderParent(DWORD parentPid) {
    if (parentPid == 0)
        return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != parentPid)
                continue;
            const std::wstring name = pe.szExeFile;
            if (name.find(L"Crymore.Loader") != std::wstring::npos ||
                name.find(L"crymore_loader") != std::wstring::npos ||
                name.find(L"crymore_core") != std::wstring::npos) {
                ok = true;
            }
            break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

AuthSession sessionFromHandshake(const LaunchHandshake& hs) {
    AuthSession s{};
    s.valid = true;
    s.username = hs.username;
    s.hwid = hs.hwid_hex;
    s.accessToken = hs.session_hint;
    return s;
}

} // namespace

bool launchBuildHandshake(const AuthSession& session, LaunchHandshake& out, std::string& error) {
    if (!session.valid || session.accessToken.empty()) {
        error = "No valid session for launch.";
        return false;
    }

    out = {};
    out.magic = 0x4352594Du;
    out.version = 2;
    out.expiry_unix = unixNow() + 90;
    out.parent_pid = GetCurrentProcessId();
    out.loader_pid = out.parent_pid;
    fillRandom(out.nonce, sizeof(out.nonce));
    copyField(out.username, sizeof(out.username), session.username);
    copyField(out.hwid_hex, sizeof(out.hwid_hex), session.hwid);

    const std::string hint = sha256HexSimple(session.accessToken);
    copyField(out.session_hint, sizeof(out.session_hint), hint.substr(0, 32));

    copyField(out.plan, sizeof(out.plan), session.plan.empty() ? "free" : session.plan);
    out.sub_expires_unix = static_cast<std::uint64_t>(session.subscriptionExpiresUnix);
    copyField(out.avatar_url, sizeof(out.avatar_url), session.avatarUrl);

    if (!computeMac(out, out.mac)) {
        error = "Failed to sign launch handshake.";
        return false;
    }
    return true;
}

#ifdef LOADER_BUILD

bool launchSpawnWithHandshake(const std::wstring& exePath,
    const std::wstring& workingDir,
    const AuthSession& session,
    unsigned long& childPid,
    std::string& error) {
    LaunchHandshake hs{};
    if (!launchBuildHandshake(session, hs, error))
        return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        error = "CreatePipe failed.";
        return false;
    }

    SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNullOut = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (hNullOut == INVALID_HANDLE_VALUE)
        hNullOut = GetStdHandle(STD_OUTPUT_HANDLE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = 0x00000100u | 0x00000001u; // USESTDHANDLES | USESHOWWINDOW
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hRead;
    si.hStdOutput = hNullOut;
    si.hStdError = hNullOut;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exePath + L"\"";

    std::vector<wchar_t> envBlock;
    if (!session.accessToken.empty()) {
        if (wchar_t* parentEnv = GetEnvironmentStringsW()) {
            for (const wchar_t* p = parentEnv; *p; ) {
                const wchar_t* start = p;
                p += wcslen(p) + 1;
                std::wstring entry(start);
                if (entry.rfind(L"CRYMORE_ACCESS_TOKEN=", 0) == 0)
                    continue;
                envBlock.insert(envBlock.end(), entry.begin(), entry.end());
                envBlock.push_back(L'\0');
            }
            FreeEnvironmentStringsW(parentEnv);
        }
        std::wstring tokenLine = L"CRYMORE_ACCESS_TOKEN=";
        for (unsigned char c : session.accessToken)
            tokenLine.push_back(static_cast<wchar_t>(c));
        envBlock.insert(envBlock.end(), tokenLine.begin(), tokenLine.end());
        envBlock.push_back(L'\0');
        envBlock.push_back(L'\0');
    }

    LPVOID envPtr = envBlock.empty() ? nullptr : envBlock.data();
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    const BOOL created = CreateProcessW(
        exePath.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
        envPtr, workingDir.c_str(), &si, &pi);

    CloseHandle(hRead);

    if (!created) {
        CloseHandle(hWrite);
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ").";
        return false;
    }

    DWORD written = 0;
    const BOOL okWrite = WriteFile(hWrite, &hs, sizeof(hs), &written, nullptr);
    CloseHandle(hWrite);

    CloseHandle(pi.hThread);
    childPid = pi.dwProcessId;
    CloseHandle(pi.hProcess);

    if (!okWrite || written != sizeof(hs)) {
        error = "Failed to write launch handshake to child.";
        return false;
    }

    return true;
}

#endif // LOADER_BUILD

bool launchAllowStandalone() {
#ifdef OVERLAY_ALLOW_STANDALONE
    if (GetEnvironmentVariableA("CRYMORE_DEV", nullptr, 0) > 0)
        return true;
#endif
    return false;
}

bool launchVerifyFromStdin(LaunchHandshake& out, std::string& error) {
    if (launchAllowStandalone()) {
        error.clear();
        return true;
    }

#ifdef CRYMORE_REQUIRE_LAUNCH_GATE
    const bool required = true;
#else
    const bool required = false;
#endif

    if (!required)
        return true;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) {
        error = "Launch gate: no stdin pipe (start via loader).";
        return false;
    }

    DWORD read = 0;
    if (!ReadFile(hIn, &out, sizeof(out), &read, nullptr) || read != sizeof(out)) {
        error = "Launch gate: handshake read failed.";
        return false;
    }

    if (out.magic != 0x4352594Du || (out.version != 1 && out.version != 2)) {
        error = "Launch gate: invalid handshake header.";
        return false;
    }

    if (out.expiry_unix < unixNow()) {
        error = "Launch gate: handshake expired.";
        return false;
    }

    const DWORD parent = getParentProcessId(GetCurrentProcessId());
    if (parent == 0 || parent != out.parent_pid) {
        error = "Launch gate: parent process mismatch.";
        return false;
    }

    if (!isLoaderParent(parent)) {
        error = "Launch gate: parent is not the loader.";
        return false;
    }

    AuthSession session = sessionFromHandshake(out);

    std::uint8_t expected[32]{};
    if (!computeMac(out, expected)) {
        error = "Launch gate: MAC computation failed.";
        return false;
    }

    if (std::memcmp(expected, out.mac, sizeof(expected)) != 0) {
        error = "Launch gate: handshake signature invalid.";
        return false;
    }

    error.clear();
    return true;
}

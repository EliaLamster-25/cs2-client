#pragma once

#include "auth.h"

#include <Windows.h>
#include <cstdint>
#include <string>

/// Wire format passed from loader → overlay via inherited stdin pipe.
#pragma pack(push, 1)
struct LaunchHandshake {
    std::uint32_t magic = 0x4352594Du; // 'CRYM'
    std::uint32_t version = 2;
    std::uint64_t expiry_unix = 0;
    std::uint32_t parent_pid = 0;
    std::uint32_t loader_pid = 0;
    std::uint8_t  nonce[16]{};
    std::uint8_t  mac[32]{};           // HMAC-SHA256
    char          username[48]{};
    char          hwid_hex[65]{};
    char          session_hint[33]{};    // first 32 hex chars of token hash (audit)
    char          plan[16]{};
    std::uint64_t sub_expires_unix = 0;
    char          avatar_url[129]{};
};
#pragma pack(pop)

static_assert(sizeof(LaunchHandshake) == 371, "LaunchHandshake size changed — bump version");

/// Build handshake for an authenticated session (valid ~90 seconds).
bool launchBuildHandshake(const AuthSession& session, LaunchHandshake& out, std::string& error);

#ifdef LOADER_BUILD
/// Spawn overlay with handshake on inherited stdin. Returns child PID on success.
bool launchSpawnWithHandshake(const std::wstring& exePath,
    const std::wstring& workingDir,
    const AuthSession& session,
    unsigned long& childPid,
    std::string& error);
#endif

/// Overlay entry: read + verify handshake from stdin. Returns false if invalid.
bool launchVerifyFromStdin(LaunchHandshake& out, std::string& error);

/// True when overlay may run without a loader handshake (dev builds only).
bool launchAllowStandalone();

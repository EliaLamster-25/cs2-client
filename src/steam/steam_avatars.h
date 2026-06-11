#pragma once

#include <cstdint>
#include <d3d11.h>
#include <mutex>
#include <unordered_map>
#include <wrl/client.h>

/// Loads Steam profile avatars via steam_api64.dll (with steam_appid.txt) and caches
/// them as D3D11 textures. Bot avatars load from t_bot.jpeg / ct_bot.jpeg beside the exe.
class SteamAvatars {
public:
    static SteamAvatars& instance();

    bool init(ID3D11Device* device);
    void shutdown();
    void tick();

    /// Queue avatar fetch for a SteamID64 (no-op for 0).
    void request(std::uint64_t steamId);

    /// Cached avatar texture, or nullptr while loading / unavailable.
    ID3D11ShaderResourceView* get(std::uint64_t steamId);

    /// Bot profile picture for team 2 (T) or 3 (CT); falls back to T then CT.
    ID3D11ShaderResourceView* getBot(int teamNum);

    /// Resolve avatar for a player row (requests Steam avatars if needed).
    ID3D11ShaderResourceView* resolve(bool isBot, int teamNum, std::uint64_t steamId);

private:
    SteamAvatars() = default;
    bool loadBotAvatars();

    enum class EntryState : std::uint8_t {
        Idle,
        Loading,
        Ready,
        Failed,
    };

    struct Entry {
        EntryState state = EntryState::Idle;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        std::uint64_t requestedMs = 0;
        std::uint64_t nextHttpMs = 0;
        bool httpQueued = false;
    };

    ID3D11Device* m_device = nullptr;
    std::mutex m_mu;
    std::unordered_map<std::uint64_t, Entry> m_cache;
    std::uint64_t m_lastHttpMs = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_botTSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_botCtSrv;
};

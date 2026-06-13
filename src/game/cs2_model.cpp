#include "game/cs2_model.h"

#include "memory/rpm.h"
#include "offsets/netvars.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {

bool isLikelyPtr(std::uintptr_t p) {
    return p >= 0x10000ULL && p <= 0x00007FFFFFFEFFFFULL;
}

std::string toLower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool looksLikeModelPath(std::string_view s) {
    if (s.size() < 12 || s.size() > 520)
        return false;
    if (s.find('/') == std::string_view::npos && s.find('\\') == std::string_view::npos)
        return false;
    return s.find("vmdl") != std::string_view::npos
        || s.find("models") != std::string_view::npos
        || s.find("characters") != std::string_view::npos
        || s.find("agents") != std::string_view::npos
        || s.find("ctm_") != std::string_view::npos
        || s.find("tm_") != std::string_view::npos;
}

bool pathIsPlausibleAgent(std::string_view s) {
    if (s.find("/weapons/") != std::string_view::npos || s.find("\\weapons\\") != std::string_view::npos)
        return false;
    if (s.find("weapon_") != std::string_view::npos)
        return false;
    return s.find("ctm_") != std::string_view::npos
        || s.find("tm_") != std::string_view::npos
        || s.find("agents/models/") != std::string_view::npos
        || s.find("characters/models/") != std::string_view::npos;
}

std::string normalizeKey(std::string key) {
    if (const auto vmdl = key.find(".vmdl"); vmdl != std::string::npos)
        key.resize(vmdl);
    else if (const auto dot = key.find('.'); dot != std::string::npos)
        key.resize(dot);
    return key;
}

bool tryReadCModelPath(const Process& proc, std::uintptr_t sceneNode, std::string& out) {
    const auto modelHandle = mem::read<std::uintptr_t>(proc, sceneNode + 0x210);
    if (!isLikelyPtr(modelHandle))
        return false;
    const auto cmodel = mem::read<std::uintptr_t>(proc, modelHandle);
    if (!isLikelyPtr(cmodel))
        return false;

    for (std::uintptr_t off = 0x8; off < 0x180; off += 8) {
        const auto p = mem::read<std::uintptr_t>(proc, cmodel + off);
        if (!isLikelyPtr(p))
            continue;
        auto s = mem::readString(proc, p, 384);
        if (s.size() < 20 || s.size() > 420)
            continue;
        const auto sl = toLower(s);
        if (!looksLikeModelPath(sl))
            continue;
        out = std::move(s);
        return true;
    }
    return false;
}

void pushUniquePath(std::vector<std::string>& out, std::string s) {
    if (s.size() < 8)
        return;
    for (const auto& existing : out) {
        if (toLower(existing) == toLower(s))
            return;
    }
    out.push_back(std::move(s));
}

} // namespace

std::string modelKeyFromPath(std::string_view path) {
    if (path.empty())
        return {};

    const std::string lowered = toLower(std::string(path));

    auto looksLikeAgentKey = [](std::string_view key) {
        return key.starts_with("ctm_") || key.starts_with("tm_") || key.starts_with("ct_");
    };

    std::string bestStem;
    std::size_t bestLen = 0;
    for (std::size_t i = 0; i < lowered.size(); ++i) {
        const bool ctm = lowered.size() - i >= 4 && lowered.compare(i, 4, "ctm_") == 0;
        const bool tm = !ctm && lowered.size() - i >= 3 && lowered.compare(i, 3, "tm_") == 0;
        if (!ctm && !tm)
            continue;

        std::size_t end = i;
        while (end < lowered.size()) {
            const unsigned char ch = (unsigned char)lowered[end];
            if (!std::isalnum(ch) && lowered[end] != '_')
                break;
            ++end;
        }

        auto stem = normalizeKey(lowered.substr(i, end - i));
        if (looksLikeAgentKey(stem) && stem.size() >= 5 && stem.size() > bestLen) {
            bestLen = stem.size();
            bestStem = std::move(stem);
        }
        i = end > i ? end - 1 : end;
    }

    if (!bestStem.empty())
        return bestStem;

    const auto slash = lowered.find_last_of("/\\");
    const auto start = (slash == std::string::npos) ? 0 : slash + 1;
    return normalizeKey(lowered.substr(start));
}

std::string readPlayerModelKey(const Process& proc, std::uintptr_t pawn) {
    if (!isLikelyPtr(pawn))
        return {};

    const auto sceneNode = mem::read<std::uintptr_t>(
        proc, pawn + netvars::pawn::m_pGameSceneNode);
    if (!isLikelyPtr(sceneNode))
        return {};

    std::vector<std::string> candidates;
    std::string cmodelPath;
    if (tryReadCModelPath(proc, sceneNode, cmodelPath))
        pushUniquePath(candidates, std::move(cmodelPath));

    constexpr std::uintptr_t kModelStateOff = 0x150;
    constexpr std::uintptr_t kModelNameOff = 0xA8;
    const auto symbolPtr = mem::read<std::uintptr_t>(
        proc, sceneNode + kModelStateOff + kModelNameOff);
    if (isLikelyPtr(symbolPtr)) {
        auto primary = mem::readString(proc, symbolPtr, 192);
        if (!primary.empty())
            pushUniquePath(candidates, std::move(primary));
    }

    for (std::uintptr_t off = 0x18; off < 0x380; off += 8) {
        if (off == kModelNameOff)
            continue;
        const auto candPtr = mem::read<std::uintptr_t>(
            proc, sceneNode + kModelStateOff + off);
        if (!isLikelyPtr(candPtr))
            continue;
        auto cand = mem::readString(proc, candPtr, 320);
        if (cand.size() < 12)
            continue;
        const auto lo = toLower(cand);
        if (!looksLikeModelPath(lo))
            continue;
        pushUniquePath(candidates, std::move(cand));
    }

    for (const auto& path : candidates) {
        const auto lo = toLower(path);
        if (!pathIsPlausibleAgent(lo))
            continue;
        const auto key = modelKeyFromPath(lo);
        if (!key.empty())
            return key;
    }

    return {};
}

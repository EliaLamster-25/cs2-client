#include "weapon_group.h"
#include "pawn_services.h"
#include "memory/rpm.h"
#include "offsets/netvars.h"
#include "offsets/offsets.h"

#include <cctype>
#include <cstring>

AimWeaponGroup classifyWeaponGroup(const char* weaponName) {
    if (!weaponName || !*weaponName)
        return AimWeaponGroup::Other;

    char lower[64]{};
    std::size_t i = 0;
    for (; weaponName[i] && i < (sizeof(lower) - 1); ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(weaponName[i])));
    lower[i] = '\0';

    auto has = [&](const char* token) { return std::strstr(lower, token) != nullptr; };

    if (has("awp") || has("ssg08") || has("scar20") || has("g3sg1"))
        return AimWeaponGroup::Snipers;

    if (has("ak47") || has("m4a1") || has("m4a1_silencer") || has("famas")
        || has("galilar") || has("aug") || has("sg556"))
        return AimWeaponGroup::Rifles;

    if (has("mac10") || has("mp9") || has("mp7") || has("mp5sd")
        || has("ump45") || has("p90") || has("bizon"))
        return AimWeaponGroup::Smg;

    if (has("nova") || has("xm1014") || has("mag7") || has("sawedoff")
        || has("m249") || has("negev"))
        return AimWeaponGroup::Heavy;

    if (has("deagle") || has("elite") || has("fiveseven") || has("glock")
        || has("hkp2000") || has("p250") || has("tec9") || has("cz75a")
        || has("revolver") || has("usp_silencer") || has("pistol"))
        return AimWeaponGroup::Pistols;

    return AimWeaponGroup::Other;
}

static bool readWeaponDesignerName(const Process& proc, std::uintptr_t entityList,
                                     std::uint32_t weapHandle, char* out, std::size_t outSize) {
    if (!entityList || !weapHandle || weapHandle == 0xFFFFFFFFu || !out || outSize == 0)
        return false;

    auto weapChunk = mem::read<std::uintptr_t>(proc, entityList + 0x10);
    if (!pawn_services::isLikelyPtr(weapChunk)
        || ((weapHandle & 0x7FFF) >> 9) != 0) {
        weapChunk = mem::read<std::uintptr_t>(
            proc, entityList + 0x10 + 0x8 * ((weapHandle & 0x7FFF) >> 9));
    }
    if (!pawn_services::isLikelyPtr(weapChunk))
        return false;

    const std::uintptr_t weapIdentity = weapChunk + std::uintptr_t(0x70) * (weapHandle & 0x1FF);
    const auto namePtr = mem::read<std::uintptr_t>(
        proc, weapIdentity + netvars::grenade::m_designerName);
    if (!pawn_services::isLikelyPtr(namePtr))
        return false;

    mem::readArray(proc, namePtr, out, static_cast<std::uint32_t>(outSize - 1));
    out[outSize - 1] = '\0';
    return out[0] != '\0';
}

AimWeaponGroup resolveActiveWeaponGroup(const Process& proc, std::uintptr_t clientBase, std::uintptr_t localPawn) {
    if (!clientBase || !localPawn)
        return AimWeaponGroup::Other;

    const std::uintptr_t entityList = mem::read<std::uintptr_t>(
        proc, clientBase + offsets::client::dwEntityList);
    if (!entityList)
        return AimWeaponGroup::Other;

    const auto weapSvcPtr = pawn_services::readWeaponServices(proc, localPawn);
    if (!weapSvcPtr)
        return AimWeaponGroup::Other;

    const auto weapHandle = mem::read<std::uint32_t>(
        proc, weapSvcPtr + netvars::weapon_services::m_hActiveWeapon);
    if (!weapHandle || weapHandle == 0xFFFFFFFFu)
        return AimWeaponGroup::Other;

    char weaponName[64]{};
    if (!readWeaponDesignerName(proc, entityList, weapHandle, weaponName, sizeof(weaponName)))
        return AimWeaponGroup::Other;
    return classifyWeaponGroup(weaponName);
}

const char* aimWeaponGroupLabel(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Pistols: return "Pistols";
    case AimWeaponGroup::Heavy:   return "Heavy";
    case AimWeaponGroup::Smg:     return "SMG";
    case AimWeaponGroup::Rifles:  return "Rifles";
    case AimWeaponGroup::Snipers: return "Snipers";
    default:                      return "Other";
    }
}

const char* aimWeaponGroupEquipHint(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Pistols: return "Glock, USP, Deagle, etc.";
    case AimWeaponGroup::Heavy:   return "Nova, XM1014, M249, Negev, etc.";
    case AimWeaponGroup::Smg:     return "MP9, MP7, P90, UMP, etc.";
    case AimWeaponGroup::Rifles:  return "AK-47, M4A1, FAMAS, etc.";
    case AimWeaponGroup::Snipers: return "AWP, SSG 08, SCAR-20, etc.";
    default:                      return "any weapon in this class";
    }
}

bool weaponGroupSupportsRcs(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Heavy:
    case AimWeaponGroup::Smg:
    case AimWeaponGroup::Rifles:
        return true;
    default:
        return false;
    }
}

AimGroupConfig defaultAimGroupConfig(AimWeaponGroup group) {
    AimGroupConfig cfg;
    if (!weaponGroupSupportsRcs(group))
        cfg.rcsEnabled = false;
    return cfg;
}

int readActiveWeaponClip(const Process& proc, std::uintptr_t clientBase, std::uintptr_t localPawn) {
    if (!clientBase || !localPawn)
        return -1;

    const std::uintptr_t entityList = mem::read<std::uintptr_t>(
        proc, clientBase + offsets::client::dwEntityList);
    if (!entityList)
        return -1;

    const auto weapSvcPtr = pawn_services::readWeaponServices(proc, localPawn);
    if (!weapSvcPtr)
        return -1;

    const auto weapHandle = mem::read<std::uint32_t>(
        proc, weapSvcPtr + netvars::weapon_services::m_hActiveWeapon);
    if (!weapHandle || weapHandle == 0xFFFFFFFFu)
        return -1;

    auto weapChunk = mem::read<std::uintptr_t>(proc, entityList + 0x10);
    if (!pawn_services::isLikelyPtr(weapChunk)
        || ((weapHandle & 0x7FFF) >> 9) != 0) {
        weapChunk = mem::read<std::uintptr_t>(
            proc, entityList + 0x10 + 0x8 * ((weapHandle & 0x7FFF) >> 9));
    }
    if (!pawn_services::isLikelyPtr(weapChunk))
        return -1;

    const std::uintptr_t weapIdentity = weapChunk + std::uintptr_t(0x70) * (weapHandle & 0x1FF);
    const auto weaponEntity = mem::read<std::uintptr_t>(proc, weapIdentity);
    if (!pawn_services::isLikelyPtr(weaponEntity))
        return -1;

    const int clip = mem::read<int>(proc, weaponEntity + netvars::weapon::m_iClip1);
    if (clip < 0 || clip > 500)
        return -1;
    return clip;
}

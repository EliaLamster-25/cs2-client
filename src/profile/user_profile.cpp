#include "user_profile.h"

#include "launch_handshake.h"

#include <chrono>
#include <cstring>

namespace {

UserProfile g_profile{};

} // namespace

void userProfileApplyFromHandshake(const LaunchHandshake& hs) {
    g_profile.username = hs.username;
    g_profile.plan = hs.plan[0] ? hs.plan : "free";
    g_profile.subExpiresUnix = hs.sub_expires_unix;
    g_profile.avatarUrl = hs.avatar_url;
}

const UserProfile& userProfileGet() {
    return g_profile;
}

int userProfileDaysRemaining() {
    if (g_profile.subExpiresUnix == 0)
        return 0;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (g_profile.subExpiresUnix <= now)
        return 0;
    return static_cast<int>((g_profile.subExpiresUnix - now + 86399) / 86400);
}

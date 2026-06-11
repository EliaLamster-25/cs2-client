#pragma once

#include <cstdint>
#include <string>

struct LaunchHandshake;

struct UserProfile {
    std::string username;
    std::string plan;
    std::uint64_t subExpiresUnix = 0;
    std::string avatarUrl;
};

void userProfileApplyFromHandshake(const LaunchHandshake& hs);
const UserProfile& userProfileGet();
int userProfileDaysRemaining();

#pragma once

#include <string>
#include <vector>

struct ProtectReport {
    bool ok = true;
    std::vector<std::string> warnings;
};

ProtectReport runProtectionChecks();
bool protectQuickCheck(std::string& reason);

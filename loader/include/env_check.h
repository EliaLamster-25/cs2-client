#pragma once

#include <string>
#include <vector>

struct EnvCheckResult {
    bool ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

EnvCheckResult runEnvironmentChecks();

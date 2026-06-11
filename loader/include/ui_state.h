#pragma once

#include <string>
#include <vector>

struct LoaderUiState {
    bool rememberMe = false;
    bool showPassword = false;
    bool busy = false;
    bool loggedIn = false;
    bool envOk = false;
    std::string statusLine;
    std::vector<std::string> logLines;
};

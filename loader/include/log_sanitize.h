#pragma once

#include <string>

/** Strip file paths and normalize punctuation for on-screen loader logs. */
std::string sanitizeUiLogLine(std::string line);

#pragma once

#include <string>

/** Terminate CS2, Steam, and common Steam child processes. */
bool steamShutdownAll(std::string& detail);

/** Start Steam if needed, then queue CS2 (app 730). Waits until cs2.exe is running. */
bool steamLaunchGamePipeline(std::string& detail);

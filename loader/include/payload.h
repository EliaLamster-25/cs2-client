#pragma once

#include "auth.h"

#include <Windows.h>
#include <string>

struct PayloadLaunchResult {
    bool ok = false;
    std::string error;
    std::wstring launchedPath;
    std::string launchedPathNarrow;
    unsigned long childPid = 0;
};

void payloadSetModuleHandle(HMODULE module);

PayloadLaunchResult launchEmbeddedPayload(const AuthSession& session);
PayloadLaunchResult launchSiblingOverlay(const AuthSession& session);
PayloadLaunchResult launchServerPayload(const AuthSession& session);

/** Background download of encrypted overlay after login (session-bound metadata). */
void payloadPrefetchServerOverlay(const AuthSession& session);
void payloadClearOverlayCache();
bool payloadOverlayCachePrefetching();
bool payloadOverlayCacheReady();

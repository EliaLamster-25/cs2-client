#include "config.h"

#include <atomic>

OverlayConfig g_cfg;
std::atomic<bool> g_requestShutdown{ false };

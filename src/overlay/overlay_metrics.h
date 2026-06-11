#pragma once

namespace overlay_metrics {
void onOverlayFrameEnd();
void onEspRenderBegin();
void onEspRenderEnd();

void setEntityDataAgeMs(float ms);

float overlayFps();
float espFrameMs();
float entityDataAgeMs();
}

#include "icons.h"
#include "overlay/renderer.h"
#include <cmath>

// Helper: draw two lines crossing at center (plus sign)
static void plus(Renderer& r, float cx, float cy, float half, float t, unsigned int c) {
    r.drawLine(cx - half, cy, cx + half, cy, c, t);
    r.drawLine(cx, cy - half, cx, cy + half, c, t);
}

// ── Tab Icons ────────────────────────────────────────────────────────────────

void Icons::aimbot(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float s = sz * 0.35f;
    r.drawCircle(cx, cy, s, color, 1.25f);
    plus(r, cx, cy, s * 0.7f, 1.25f, color);
}

void Icons::visuals(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float rx = sz * 0.4f, ry = sz * 0.22f;
    int segs = 16;
    // Top arc
    float lx = cx - rx, ly = cy;
    for (int i = 1; i <= segs; ++i) {
        float a = 3.14159265f * i / segs;
        float px = cx - rx * cosf(a), py = cy - ry * sinf(a);
        r.drawLine(lx, ly, px, py, color, 1.2f);
        lx = px; ly = py;
    }
    // Bottom arc
    lx = cx + rx; ly = cy;
    for (int i = 1; i <= segs; ++i) {
        float a = 3.14159265f * i / segs;
        float px = cx + rx * cosf(a), py = cy + ry * sinf(a);
        r.drawLine(lx, ly, px, py, color, 1.2f);
        lx = px; ly = py;
    }
    r.drawFilledCircle(cx, cy, sz * 0.07f, color);
}

void Icons::players(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f;
    float top = y + sz * 0.18f;
    float w = sz * 0.28f;
    float h = sz * 0.12f;

    for (int i = 0; i < 3; ++i) {
        float cy = top + i * sz * 0.2f;
        r.drawLine(cx, cy, cx + w, cy + h, color, 1.15f);
        r.drawLine(cx + w, cy + h, cx, cy + h * 2.f, color, 1.15f);
        r.drawLine(cx, cy + h * 2.f, cx - w, cy + h, color, 1.15f);
        r.drawLine(cx - w, cy + h, cx, cy, color, 1.15f);
    }
}

void Icons::misc(Renderer& r, float x, float y, float sz, unsigned int color) {
    float left = x + sz * 0.16f;
    float right = x + sz * 0.84f;
    float y0 = y + sz * 0.28f;
    float gap = sz * 0.22f;
    float t = 1.2f;
    r.drawLine(left, y0, right, y0, color, t);
    r.drawLine(left, y0 + gap, right, y0 + gap, color, t);
    r.drawLine(left, y0 + gap * 2.f, right, y0 + gap * 2.f, color, t);
    r.drawFilledCircle(x + sz * 0.38f, y0, sz * 0.07f, color);
    r.drawFilledCircle(x + sz * 0.62f, y0 + gap, sz * 0.07f, color);
    r.drawFilledCircle(x + sz * 0.30f, y0 + gap * 2.f, sz * 0.07f, color);
}

void Icons::crosshair(Renderer& r, float x, float y, float sz, unsigned int color) {
    float t = 1.25f;
    float in = sz * 0.25f, out = sz * 0.45f;
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    // Top-left
    r.drawLine(cx - out, cy - in, cx - out, cy - out, color, t);
    r.drawLine(cx - out, cy - out, cx - in, cy - out, color, t);
    // Top-right
    r.drawLine(cx + in, cy - out, cx + out, cy - out, color, t);
    r.drawLine(cx + out, cy - out, cx + out, cy - in, color, t);
    // Bottom-left
    r.drawLine(cx - out, cy + in, cx - out, cy + out, color, t);
    r.drawLine(cx - out, cy + out, cx - in, cy + out, color, t);
    // Bottom-right
    r.drawLine(cx + in, cy + out, cx + out, cy + out, color, t);
    r.drawLine(cx + out, cy + out, cx + out, cy + in, color, t);
}

void Icons::radar(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    r.drawCircle(cx, cy, sz * 0.35f, color, 1.1f);
    r.drawCircle(cx, cy, sz * 0.18f, color, 1.1f);
    // Scan line sweeping down-right
    float a = 0.9f;
    float sx = cx + cosf(a) * sz * 0.4f, sy = cy + sinf(a) * sz * 0.4f;
    r.drawLine(cx, cy, sx, sy, color, 1.2f);
}

void Icons::camera(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float bw = sz * 0.5f, bh = sz * 0.3f;
    // Body (rounded rectangle approximation)
    r.drawRoundedRect(cx - bw, cy - bh + sz * 0.05f, bw * 2, bh * 2, color, 3.f, 1.2f);
    // Flash button on top
    r.drawFilledRect(cx + bw * 0.3f, cy - bh + sz * 0.02f, sz * 0.08f, sz * 0.05f, color);
    // Lens
    r.drawCircle(cx, cy, sz * 0.13f, color, 1.2f);
}

void Icons::configs(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float ir = sz * 0.22f, or_ = sz * 0.38f;
    r.drawCircle(cx, cy, ir, color, 1.25f);
    // 6 teeth
    for (int i = 0; i < 6; ++i) {
        float a = 3.14159265f * i / 3;
        float tx = cx + cosf(a) * or_, ty = cy + sinf(a) * or_;
        r.drawFilledRect(tx - sz * 0.04f, ty - sz * 0.04f, sz * 0.08f, sz * 0.08f, color);
    }
}

void Icons::extra(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    plus(r, cx, cy, sz * 0.28f, 1.25f, color);
}

// ── Action Icons ─────────────────────────────────────────────────────────────

void Icons::trash(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float bw = sz * 0.22f, bh = sz * 0.4f;
    // Body
    r.drawRoundedRect(cx - bw, cy - bh * 0.25f, bw * 2, bh * 1.25f, color, 2.f, 1.2f);
    // Lid
    r.drawLine(cx - bw * 1.2f, cy - bh * 0.25f, cx + bw * 1.2f, cy - bh * 0.25f, color, 1.2f);
    // Handle
    r.drawLine(cx - bw * 0.5f, cy - bh * 0.25f, cx - bw * 0.5f, cy - bh * 0.45f, color, 1.2f);
    r.drawLine(cx + bw * 0.5f, cy - bh * 0.25f, cx + bw * 0.5f, cy - bh * 0.45f, color, 1.2f);
    r.drawLine(cx - bw * 0.5f, cy - bh * 0.45f, cx + bw * 0.5f, cy - bh * 0.45f, color, 1.2f);
}

void Icons::edit(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float s = sz * 0.25f;
    // Angled line for pencil tip
    r.drawLine(cx - s, cy + s, cx + s * 0.5f, cy - s * 0.8f, color, 1.2f);
    r.drawLine(cx + s * 0.5f, cy - s * 0.8f, cx + s, cy - s * 0.3f, color, 1.2f);
    // Eraser end
    r.drawLine(cx - s, cy + s, cx - s * 0.5f, cy + s * 0.5f, color, 1.2f);
    // Arrow point
    r.drawLine(cx - s * 0.1f, cy + s * 0.1f, cx + s * 0.8f, cy - s * 0.6f, color, 1.2f);
}

void Icons::play(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float s = sz * 0.3f;
    // Right-pointing triangle as three lines
    r.drawLine(cx - s * 0.7f, cy - s, cx + s * 0.5f, cy, color, 1.25f);
    r.drawLine(cx + s * 0.5f, cy, cx - s * 0.7f, cy + s, color, 1.25f);
    r.drawLine(cx - s * 0.7f, cy + s, cx - s * 0.7f, cy - s, color, 1.25f);
}

void Icons::page(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float w = sz * 0.3f, h = sz * 0.4f;
    // Document body
    r.drawLine(cx - w, cy - h, cx + w, cy - h, color, 1.1f);
    r.drawLine(cx + w, cy - h, cx + w, cy + h, color, 1.1f);
    r.drawLine(cx + w, cy + h, cx - w, cy + h, color, 1.1f);
    r.drawLine(cx - w, cy + h, cx - w, cy - h, color, 1.1f);
    // Folded corner
    r.drawLine(cx + w - sz * 0.08f, cy - h, cx + w, cy - h + sz * 0.08f, color, 1.1f);
    // Lines
    r.drawLine(cx - w * 0.4f, cy - h * 0.3f, cx + w * 0.6f, cy - h * 0.3f, color, 1.f);
    r.drawLine(cx - w * 0.4f, cy, cx + w * 0.6f, cy, color, 1.f);
    r.drawLine(cx - w * 0.4f, cy + h * 0.3f, cx + w * 0.4f, cy + h * 0.3f, color, 1.f);
}

void Icons::save(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float w = sz * 0.3f, h = sz * 0.38f;
    r.drawRoundedRect(cx - w, cy - h, w * 2, h * 2, color, 2.f, 1.2f);
    // Label area
    r.drawFilledRect(cx - w * 0.5f, cy - h * 0.2f, w, h * 0.5f, color);
    // Write-protect tab
    r.drawFilledRect(cx + w + sz * 0.03f, cy - h * 0.3f, sz * 0.04f, sz * 0.1f, color);
}

void Icons::search(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    // Magnifying glass: circle + handle
    float rs = sz * 0.28f;
    r.drawCircle(cx - rs * 0.15f, cy - rs * 0.15f, rs, color, 1.2f);
    // Handle (angled line from bottom-right of circle to further right-down)
    float ax = cx - rs * 0.15f + rs * 0.707f;
    float ay = cy - rs * 0.15f + rs * 0.707f;
    float bx = ax + rs * 0.55f;
    float by = ay + rs * 0.55f;
    r.drawLine(ax, ay, bx, by, color, 1.2f);
}

void Icons::refresh(Renderer& r, float x, float y, float sz, unsigned int color) {
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
    float rs = sz * 0.30f;
    const float kPi = 3.14159265f;
    const int segs = 18;
    float startA = -kPi / 4.f;
    float arcLen = kPi * 5.f / 3.f;

    float lx = cx + cosf(startA) * rs;
    float ly = cy + sinf(startA) * rs;
    for (int i = 1; i <= segs; ++i) {
        float a = startA + arcLen * i / segs;
        float nx = cx + cosf(a) * rs;
        float ny = cy + sinf(a) * rs;
        r.drawLine(lx, ly, nx, ny, color, 1.2f);
        lx = nx;
        ly = ny;
    }

    // Arrowhead at arc end, aligned with clockwise tangent.
    float endA = startA + arcLen;
    float tanX = -sinf(endA);
    float tanY = cosf(endA);
    float al = sz * 0.14f, pw = al * 0.55f;
    r.drawLine(lx, ly, lx - tanX * al - tanY * pw, ly - tanY * al + tanX * pw, color, 1.2f);
    r.drawLine(lx, ly, lx - tanX * al + tanY * pw, ly - tanY * al - tanX * pw, color, 1.2f);
}

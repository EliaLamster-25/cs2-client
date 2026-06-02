#pragma once

class Renderer;

namespace Icons {

void aimbot   (Renderer& r, float x, float y, float sz, unsigned int color);
void visuals  (Renderer& r, float x, float y, float sz, unsigned int color);
void players  (Renderer& r, float x, float y, float sz, unsigned int color);
void misc     (Renderer& r, float x, float y, float sz, unsigned int color);
void crosshair(Renderer& r, float x, float y, float sz, unsigned int color);
void radar    (Renderer& r, float x, float y, float sz, unsigned int color);
void camera   (Renderer& r, float x, float y, float sz, unsigned int color);
void configs  (Renderer& r, float x, float y, float sz, unsigned int color);
void extra    (Renderer& r, float x, float y, float sz, unsigned int color);

void refresh  (Renderer& r, float x, float y, float sz, unsigned int color);
void trash    (Renderer& r, float x, float y, float sz, unsigned int color);
void edit     (Renderer& r, float x, float y, float sz, unsigned int color);
void play     (Renderer& r, float x, float y, float sz, unsigned int color);
void page     (Renderer& r, float x, float y, float sz, unsigned int color);
void save     (Renderer& r, float x, float y, float sz, unsigned int color);
void search   (Renderer& r, float x, float y, float sz, unsigned int color);

}

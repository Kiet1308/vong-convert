#pragma once
#include "common.h"

// Full-screen crop view: the picture (or a video's first frame) over a dimmed, blurred
// desktop, with a draggable crop frame and aspect-ratio presets.
namespace crop {
void Open(const std::wstring& file, POINT anchor);
bool Active();
bool Tick(float dt);  // true while it needs more frames
void Render();
void Close();
// Times the backdrop, preview and drawing for `file` offscreen (--bench).
std::wstring Bench(const std::wstring& file);
// Renders the view for `file` into a PNG without opening a window (--crop-test).
bool RenderTest(const std::wstring& file, const std::wstring& out, int preset);
}  // namespace crop

// Implemented in main.cpp: runs the crop and shows its progress bubble at `center`.
void OnCropConfirmed(const std::wstring& file, int x, int y, int w, int h, POINT center, float dpi);

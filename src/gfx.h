#pragma once
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <wincodec.h>

#include <atomic>
#include <map>
#include <memory>
#include <tuple>

#include "common.h"

// Colors for the wheel and bubbles, matching the prototype's tokens.
struct Theme {
  bool dark = false;
  D2D1_COLOR_F accent, onAccent, fg, fg2, fg3;
  D2D1_COLOR_F glass, glassHub, glassHi, glassLine, glassEdge;
  D2D1_COLOR_F solid, solidHub, seg, sub, key, keyLine, shadow, danger;
  float segOpacity = 0.06f, shadowOpacity = 0.24f;
};
Theme LoadTheme(int force = -1);  // follows the Windows light/dark setting and accent palette; force 0 light, 1 dark
D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a);

struct FormatKey {
  float size;
  int weight;
  bool display, wrap, ellipsis;
  int align;
  bool operator<(const FormatKey& o) const {
    return std::tie(size, weight, display, wrap, ellipsis, align) <
           std::tie(o.size, o.weight, o.display, o.wrap, o.ellipsis, o.align);
  }
};

class Gfx {
 public:
  // Creates the devices on first use (main.cpp warms them up shortly after launch).
  // Shutdown releases them; the next Ensure rebuilds, e.g. after the GPU was reset.
  bool Ensure();
  void Shutdown();
  // Hands cached GPU memory back (D2D's texture cache, driver scratch buffers) while nothing is on screen.
  void Idle();
  bool Ready() const { return dc != nullptr; }
  int generation = 0;  // bumps on every Init, so surfaces know to rebuild
  bool lost = false;   // set when drawing reports a removed or reset device
  // During a frame every window draws first and the composition is committed once at the end.
  bool deferCommit = false, commitPending = false;
  void CommitIfPending();

  IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, bool display = false, bool wrap = false,
                            bool ellipsis = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER);
  ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, IDWriteTextFormat* format, float maxWidth,
                                   float maxHeight = 1000.f);
  ID2D1PathGeometry* Icon(const char* svg);
  ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color, float opacity = 1.f);

  ComPtr<ID3D11Device> d3d;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<ID2D1Factory1> factory;
  ComPtr<ID2D1Device> device;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<ID2D1StrokeStyle> round;  // round caps and joins
  std::wstring textFont = L"Segoe UI", displayFont = L"Segoe UI";

 private:
  bool Init();
  bool InitDrawing();
  void ReleaseDrawing();
  std::map<FormatKey, ComPtr<IDWriteTextFormat>> formats_;
  std::map<const char*, ComPtr<ID2D1PathGeometry>> icons_;
  ComPtr<ID2D1SolidColorBrush> brush_;
};

extern Gfx g_gfx;

// DirectComposition content for one window: one visual holding one surface.
class CompSurface {
 public:
  void Attach(HWND hwnd);
  // Makes sure the composition target and a surface of this size exist, creating
  // the graphics devices if needed.
  bool Ensure(UINT width, UINT height);
  // Starts drawing at the given DPI; returns the device context with the surface's
  // offset already applied (see Base()), or nullptr on failure.
  ID2D1DeviceContext* BeginDraw(float dpi);
  void EndDraw();
  void Clear();
  const D2D1_MATRIX_3X2_F& Base() const { return base_; }
  void Reset();

 private:
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> visual_;
  ComPtr<IDCompositionSurface> surface_;
  ComPtr<ID2D1Bitmap1> bitmap_;
  HWND hwnd_ = nullptr;
  int gen_ = -1;
  UINT w_ = 0, h_ = 0;
  D2D1_MATRIX_3X2_F base_ = D2D1::Matrix3x2F::Identity();
};

// Frosted glass: a copy of what is on screen, blurred and a little more saturated.
struct ScreenGrab {
  RECT rect{};
  int downscale = 1, w = 0, h = 0;
  std::vector<BYTE> pixels;
  std::atomic<bool> done{false};
  bool ok = false;
};
// Copies a screen rectangle on a worker thread, since GDI has to wait for the compositor (~10 ms).
// `notify` receives `message` when the pixels are in. Give our own window WDA_EXCLUDEFROMCAPTURE
// first so it is left out of the copy.
std::shared_ptr<ScreenGrab> GrabScreenAsync(const RECT& r, int downscale, HWND notify = nullptr, UINT message = 0);
ComPtr<ID2D1Bitmap1> BlurGrab(const ScreenGrab& grab, float dpi, float stdDev, float saturation);
ComPtr<ID2D1Bitmap1> CaptureBlurred(const RECT& screenRect, float dpi, float stdDev, float saturation, int downscale = 1);
ComPtr<ID2D1Bitmap1> BlurImage(ID2D1Bitmap1* source, float dpi, float stdDev, float saturation);
// Copies a target bitmap back from the GPU and writes it as PNG (visual tests).
bool SavePng(ID2D1Bitmap1* target, const std::wstring& path);
// Loads an image file into a premultiplied D2D bitmap.
ComPtr<ID2D1Bitmap1> LoadImageFile(const std::wstring& path, UINT* width = nullptr, UINT* height = nullptr);

// Geometry helpers shared by the wheel and the bubbles.
ComPtr<ID2D1PathGeometry> RingGeometry(float inner, float outer);
ComPtr<ID2D1PathGeometry> SectorGeometry(float r0, float r1, float a0, float a1, float gap);
// Sector with its corners rounded by `round` (union of the shape and its widened outline).
ComPtr<ID2D1Geometry> RoundedSector(float r0, float r1, float a0, float a1, float gap, float round);
ComPtr<ID2D1PathGeometry> ArcGeometry(float r, float a0, float a1);

#include "gfx.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "svgpath.h"
#include "util.h"

Gfx g_gfx;

namespace {

D2D1_COLOR_F Hex(UINT32 rgb, float a = 1.f) {
  return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

D2D1_COLOR_F Rgba(int r, int g, int b, float a) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }

constexpr float kTolerance = 0.05f;

D2D1_POINT_2F Polar(float r, float a) { return D2D1::Point2F(r * std::cos(a), r * std::sin(a)); }

}  // namespace

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
  t = std::clamp(t, 0.f, 1.f);
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
  c.a *= std::clamp(a, 0.f, 1.f);
  return c;
}

Theme LoadTheme(int force) {
  Theme t;
  DWORD light = 1, size = sizeof(light);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  t.dark = force >= 0 ? force == 1 : light == 0;

  // AccentPalette holds 8 RGBA entries; Windows uses entry 1 on dark surfaces and entry 4 on light ones.
  D2D1_COLOR_F accLight = Hex(0x0067c0), accDark = Hex(0x4cc2ff);
  BYTE pal[32];
  DWORD psize = sizeof(pal);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                   L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &psize) == ERROR_SUCCESS &&
      psize >= 32) {
    accDark = Rgba(pal[4], pal[5], pal[6], 1);
    accLight = Rgba(pal[16], pal[17], pal[18], 1);
  }

  if (!t.dark) {
    t.accent = accLight;
    t.onAccent = Hex(0xffffff);
    t.fg = Hex(0x1a1d24);
    t.fg2 = Hex(0x596070);
    t.fg3 = Hex(0x80869a);
    t.glass = Rgba(248, 250, 253, .58f);
    t.glassHub = Rgba(252, 253, 255, .76f);
    t.glassHi = Rgba(255, 255, 255, .75f);
    t.glassLine = Rgba(255, 255, 255, .8f);
    t.glassEdge = Rgba(20, 30, 50, .12f);
    t.solid = Hex(0xf6f7f9);
    t.solidHub = Hex(0xffffff);
    t.seg = Hex(0x1f2e4d);
    t.segOpacity = .06f;
    t.sub = Hex(0xfbfcfe);
    t.key = Hex(0xffffff);
    t.keyLine = Rgba(20, 30, 50, .2f);
    t.shadow = Rgba(25, 35, 65, 1);
    t.shadowOpacity = .22f;
    t.danger = Hex(0xc42b1c);
  } else {
    t.accent = accDark;
    t.onAccent = Hex(0x0a1018);
    t.fg = Hex(0xf1f2f4);
    t.fg2 = Hex(0xa7acb6);
    t.fg3 = Hex(0x7d8390);
    t.glass = Rgba(34, 36, 44, .52f);
    t.glassHub = Rgba(40, 42, 50, .72f);
    t.glassHi = Rgba(255, 255, 255, .08f);
    t.glassLine = Rgba(255, 255, 255, .13f);
    t.glassEdge = Rgba(255, 255, 255, .12f);
    t.solid = Hex(0x2b2c31);
    t.solidHub = Hex(0x323339);
    t.seg = Hex(0xffffff);
    t.segOpacity = .07f;
    t.sub = Hex(0x2c2e34);
    t.key = Hex(0x2d2e33);
    t.keyLine = Rgba(255, 255, 255, .16f);
    t.shadow = Rgba(0, 0, 0, 1);
    t.shadowOpacity = .5f;
    t.danger = Hex(0xff99a4);
  }
  return t;
}

bool Gfx::Ensure() {
  if (Ready()) return true;
  if (!d3d) return Init();
  double t0 = (double)GetTickCount64();
  bool ok = InitDrawing();
  util::Log(L"drawing devices rebuilt in %.0f ms", (double)GetTickCount64() - t0);
  return ok;
}

bool Gfx::Init() {
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  // A few hundred pixels of 2D never need the discrete GPU; the integrated one wakes faster,
  // uses less memory and keeps the laptop's dGPU asleep.
  ComPtr<IDXGIFactory6> dxgiFactory;
  ComPtr<IDXGIAdapter1> adapter;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))
    dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&adapter));
  HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d,
                                 nullptr, nullptr);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  }
  if (FAILED(hr)) { util::Log(L"D3D11CreateDevice failed 0x%08X", (unsigned)hr); return false; }
  if (FAILED(d3d.As(&dxgi))) return false;

  D2D1_FACTORY_OPTIONS opts{};
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, (void**)factory.GetAddressOf());
  if (FAILED(hr)) { util::Log(L"D2D1CreateFactory failed 0x%08X", (unsigned)hr); return false; }
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dwrite.GetAddressOf());
  if (FAILED(hr)) return false;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

  D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  factory->CreateStrokeStyle(sp, nullptr, 0, &round);

  ComPtr<IDWriteFontCollection> fonts;
  if (SUCCEEDED(dwrite->GetSystemFontCollection(&fonts))) {
    UINT32 index;
    BOOL exists = FALSE;
    if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
      textFont = L"Segoe UI Variable Text";
    exists = FALSE;
    if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Display", &index, &exists)) && exists)
      displayFont = L"Segoe UI Variable Display";
  }
  if (!InitDrawing()) return false;
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  DXGI_ADAPTER_DESC desc{};
  if (adapter) adapter->GetDesc(&desc);
  util::Log(L"graphics ready in %.0f ms on %ls", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
            adapter ? desc.Description : L"default adapter");
  return true;
}

// The Direct2D and composition devices sit on top of Direct3D; Ensure() rebuilds just these
// when they were dropped (a lost device) but Direct3D is still there.
bool Gfx::InitDrawing() {
  if (FAILED(factory->CreateDevice(dxgi.Get(), &device))) return false;
  if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  HRESULT hr = DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice), (void**)dcomp.GetAddressOf());
  if (FAILED(hr)) { util::Log(L"DCompositionCreateDevice failed 0x%08X", (unsigned)hr); return false; }
  dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
  ++generation;
  return true;
}

void Gfx::ReleaseDrawing() {
  brush_.Reset();
  dcomp.Reset();
  dc.Reset();
  device.Reset();
  if (d3d) {
    ComPtr<ID3D11DeviceContext> ctx;
    d3d->GetImmediateContext(&ctx);
    ctx->ClearState();
    ctx->Flush();
    ComPtr<IDXGIDevice3> d3;
    if (SUCCEEDED(dxgi.As(&d3))) d3->Trim();
  }
}

void Gfx::Shutdown() {
  lost = false;
  ReleaseDrawing();
  formats_.clear();
  icons_.clear();
  round.Reset();
  wic.Reset();
  dwrite.Reset();
  factory.Reset();
  dxgi.Reset();
  d3d.Reset();
}

void Gfx::CommitIfPending() {
  if (commitPending && dcomp) dcomp->Commit();
  commitPending = false;
}

void Gfx::Idle() {
  if (!device) return;
  device->ClearResources(0);
  ComPtr<ID3D11DeviceContext> ctx;
  d3d->GetImmediateContext(&ctx);
  ctx->ClearState();
  ctx->Flush();
  ComPtr<IDXGIDevice3> d3;
  if (SUCCEEDED(dxgi.As(&d3))) d3->Trim();
}

IDWriteTextFormat* Gfx::Format(float size, DWRITE_FONT_WEIGHT weight, bool display, bool wrap, bool ellipsis,
                               DWRITE_TEXT_ALIGNMENT align) {
  FormatKey key{size, (int)weight, display, wrap, ellipsis, (int)align};
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  dwrite->CreateTextFormat((display ? displayFont : textFont).c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"vi-VN", &f);
  if (!f) return nullptr;
  f->SetTextAlignment(align);
  f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (ellipsis) {
    ComPtr<IDWriteInlineObject> sign;
    dwrite->CreateEllipsisTrimmingSign(f.Get(), &sign);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    f->SetTrimming(&trim, sign.Get());
  }
  formats_[key] = f;
  return f.Get();
}

ComPtr<IDWriteTextLayout> Gfx::Layout(const std::wstring& text, IDWriteTextFormat* format, float maxWidth,
                                      float maxHeight) {
  ComPtr<IDWriteTextLayout> layout;
  if (format) dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, maxWidth, maxHeight, &layout);
  return layout;
}

ID2D1PathGeometry* Gfx::Icon(const char* svg) {
  if (!svg) return nullptr;
  auto it = icons_.find(svg);
  if (it != icons_.end()) return it->second.Get();
  auto geo = BuildSvgPath(factory.Get(), svg);
  icons_[svg] = geo;
  return geo.Get();
}

ID2D1SolidColorBrush* Gfx::Brush(const D2D1_COLOR_F& color, float opacity) {
  brush_->SetColor(color);
  brush_->SetOpacity(std::clamp(opacity, 0.f, 1.f));
  return brush_.Get();
}

// ---------------------------------------------------------------------------

void CompSurface::Attach(HWND hwnd) { hwnd_ = hwnd; }

bool CompSurface::Ensure(UINT width, UINT height) {
  if (!g_gfx.Ensure()) return false;
  if (!target_ || gen_ != g_gfx.generation) {
    // First use, or the devices were released and rebuilt since: start over.
    Reset();
    if (FAILED(g_gfx.dcomp->CreateTargetForHwnd(hwnd_, TRUE, &target_)) ||
        FAILED(g_gfx.dcomp->CreateVisual(&visual_)))
      return false;
    target_->SetRoot(visual_.Get());
    gen_ = g_gfx.generation;
  }
  if (surface_ && width == w_ && height == h_) return true;
  surface_.Reset();
  if (FAILED(g_gfx.dcomp->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                        &surface_)))
    return false;
  visual_->SetContent(surface_.Get());
  w_ = width;
  h_ = height;
  return true;
}

ID2D1DeviceContext* CompSurface::BeginDraw(float dpi) {
  if (!surface_ || gen_ != g_gfx.generation || !g_gfx.Ready()) return nullptr;
  ComPtr<IDXGISurface> dxgiSurface;
  POINT offset{};
  HRESULT hr = surface_->BeginDraw(nullptr, __uuidof(IDXGISurface), (void**)dxgiSurface.GetAddressOf(), &offset);
  if (FAILED(hr)) { util::Log(L"surface BeginDraw failed 0x%08X", (unsigned)hr); return nullptr; }
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
  hr = g_gfx.dc->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &bitmap_);
  if (FAILED(hr)) { surface_->EndDraw(); return nullptr; }
  auto* dc = g_gfx.dc.Get();
  dc->SetTarget(bitmap_.Get());
  dc->SetDpi(dpi, dpi);
  dc->BeginDraw();
  float s = 96.f / dpi;
  base_ = D2D1::Matrix3x2F::Translation(offset.x * s, offset.y * s);
  dc->SetTransform(base_);
  // The surface may live inside a larger atlas: only touch our own rectangle.
  dc->PushAxisAlignedClip(D2D1::RectF(0, 0, w_ * s, h_ * s), D2D1_ANTIALIAS_MODE_ALIASED);
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  return dc;
}

void CompSurface::EndDraw() {
  auto* dc = g_gfx.dc.Get();
  dc->SetTransform(base_);
  dc->PopAxisAlignedClip();
  HRESULT hr = dc->EndDraw();
  if (FAILED(hr)) util::Log(L"EndDraw failed 0x%08X", (unsigned)hr);
  if (hr == (HRESULT)D2DERR_RECREATE_TARGET || hr == (HRESULT)DXGI_ERROR_DEVICE_REMOVED || hr == (HRESULT)DXGI_ERROR_DEVICE_RESET)
    g_gfx.lost = true;
  dc->SetTarget(nullptr);
  bitmap_.Reset();
  surface_->EndDraw();
  if (g_gfx.deferCommit) g_gfx.commitPending = true;
  else g_gfx.dcomp->Commit();
}

void CompSurface::Clear() {
  if (BeginDraw(96.f)) EndDraw();
}

void CompSurface::Reset() {
  bool had = target_ != nullptr;
  surface_.Reset();
  visual_.Reset();
  target_.Reset();
  w_ = h_ = 0;
  gen_ = -1;
  if (had && g_gfx.dcomp) g_gfx.dcomp->Commit();
}

// ---------------------------------------------------------------------------

namespace {
// {1feb6d69-2fe6-4ac9-8c58-1d7f93e7a6a5} and {921f03d6-641c-47df-852d-b4bb6153ae11}
const GUID kGaussianBlur = {0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
const GUID kColorMatrix = {0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};

template <class T>
void SetProp(ID2D1Effect* e, UINT32 index, D2D1_PROPERTY_TYPE type, const T& value) {
  e->SetValue(index, type, (const BYTE*)&value, sizeof(T));
}
}  // namespace

ComPtr<ID2D1Bitmap1> BlurImage(ID2D1Bitmap1* source, float dpi, float stdDev, float saturation) {
  auto* dc = g_gfx.dc.Get();
  ComPtr<ID2D1Effect> blur, sat;
  if (!dc || FAILED(dc->CreateEffect(kGaussianBlur, &blur)) || FAILED(dc->CreateEffect(kColorMatrix, &sat)))
    return nullptr;
  blur->SetInput(0, source);
  SetProp(blur.Get(), 0, D2D1_PROPERTY_TYPE_FLOAT, stdDev);    // standard deviation (DIPs)
  SetProp(blur.Get(), 2, D2D1_PROPERTY_TYPE_ENUM, (UINT32)1);  // hard border
  const float s = saturation, lr = .2126f * (1 - s), lg = .7152f * (1 - s), lb = .0722f * (1 - s);
  const float values[20] = {lr + s, lr, lr, 0, lg, lg + s, lg, 0, lb, lb, lb + s, 0, 0, 0, 0, 1, 0, 0, 0, 0};
  D2D1_MATRIX_5X4_F m;
  memcpy(&m, values, sizeof(values));
  sat->SetInputEffect(0, blur.Get());
  SetProp(sat.Get(), 0, D2D1_PROPERTY_TYPE_MATRIX_5X4, m);
  SetProp(sat.Get(), 2, D2D1_PROPERTY_TYPE_BOOL, (BOOL)TRUE);  // clamp output

  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi,
                                       dpi);
  ComPtr<ID2D1Bitmap1> out;
  if (FAILED(dc->CreateBitmap(source->GetPixelSize(), nullptr, 0, &props, &out))) return nullptr;
  dc->SetTarget(out.Get());
  dc->SetDpi(dpi, dpi);
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->Clear(D2D1::ColorF(0, 0, 0, 1));
  dc->DrawImage(sat.Get());
  HRESULT hr = dc->EndDraw();
  dc->SetTarget(nullptr);
  return SUCCEEDED(hr) ? out : nullptr;
}

namespace {

// Copies a screen rectangle with GDI. The result is blurred far past any detail, so it is taken at a
// fraction of the resolution: much less memory and work. GDI waits for the compositor here (~10 ms).
void GrabPixels(ScreenGrab& g) {
  const RECT& r = g.rect;
  int w = r.right - r.left, h = r.bottom - r.top;
  int ds = std::max(1, g.downscale);
  g.w = std::max(1, w / ds);
  g.h = std::max(1, h / ds);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = g.w;
  bi.bmiHeader.biHeight = -g.h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib) {
    HGDIOBJ old = SelectObject(mem, dib);
    if (ds == 1) {
      g.ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    } else {
      SetStretchBltMode(mem, COLORONCOLOR);  // plain sampling is enough before a heavy blur
      g.ok = StretchBlt(mem, 0, 0, g.w, g.h, screen, r.left, r.top, w, h, SRCCOPY);
    }
    SelectObject(mem, old);
    GdiFlush();
    g.pixels.assign((const BYTE*)bits, (const BYTE*)bits + (size_t)g.w * g.h * 4);
    DeleteObject(dib);
  }
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

}  // namespace

std::shared_ptr<ScreenGrab> GrabScreenAsync(const RECT& r, int downscale, HWND notify, UINT message) {
  auto g = std::make_shared<ScreenGrab>();
  g->rect = r;
  g->downscale = downscale;
  std::thread([g, notify, message] {
    GrabPixels(*g);
    g->done = true;
    if (notify) PostMessageW(notify, message, 0, 0);
  }).detach();
  return g;
}

ComPtr<ID2D1Bitmap1> BlurGrab(const ScreenGrab& g, float dpi, float stdDev, float saturation) {
  if (!g.ok || g.pixels.empty() || !g_gfx.Ensure()) return nullptr;
  // The bitmap's DPI drops with the scale, so it still covers the same area in DIPs and
  // stretches back up smoothly when drawn.
  float sdpi = dpi / std::max(1, g.downscale);
  ComPtr<ID2D1Bitmap1> src;
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), sdpi, sdpi);
  if (FAILED(g_gfx.dc->CreateBitmap(D2D1::SizeU(g.w, g.h), g.pixels.data(), g.w * 4, &props, &src))) return nullptr;
  return BlurImage(src.Get(), sdpi, stdDev, saturation);
}

ComPtr<ID2D1Bitmap1> CaptureBlurred(const RECT& r, float dpi, float stdDev, float saturation, int downscale) {
  ScreenGrab g;
  g.rect = r;
  g.downscale = downscale;
  GrabPixels(g);
  return BlurGrab(g, dpi, stdDev, saturation);
}

ComPtr<ID2D1Bitmap1> LoadImageFile(const std::wstring& path, UINT* width, UINT* height) {
  if (!g_gfx.Ensure() || !g_gfx.wic) return nullptr;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(g_gfx.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(g_gfx.wic->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeMedianCut)))
    return nullptr;
  ComPtr<ID2D1Bitmap1> bmp;
  if (FAILED(g_gfx.dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp))) return nullptr;
  if (width && height) conv->GetSize(width, height);
  return bmp;
}

bool SavePng(ID2D1Bitmap1* target, const std::wstring& path) {
  auto* dc = g_gfx.dc.Get();
  D2D1_SIZE_U size = target->GetPixelSize();
  ComPtr<ID2D1Bitmap1> cpu;
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, &cpu))) return false;
  cpu->CopyFromBitmap(nullptr, target, nullptr);
  D2D1_MAPPED_RECT mapped;
  if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
  ComPtr<IWICBitmap> wb;
  g_gfx.wic->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA, mapped.pitch,
                                     mapped.pitch * size.height, mapped.bits, &wb);
  cpu->Unmap();
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (!wb || FAILED(g_gfx.wic->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(g_gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
      FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
      FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(wb.Get(), nullptr)) || FAILED(frame->Commit()) ||
      FAILED(enc->Commit()))
    return false;
  return true;
}

ComPtr<ID2D1PathGeometry> RingGeometry(float inner, float outer) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  g_gfx.factory->CreatePathGeometry(&path);
  path->Open(&sink);
  sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
  for (float r : {outer, inner}) {
    if (r <= 0) continue;
    sink->BeginFigure(D2D1::Point2F(r, 0), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(-r, 0), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, 0), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  }
  sink->Close();
  return path;
}

ComPtr<ID2D1PathGeometry> SectorGeometry(float r0, float r1, float a0, float a1, float gap) {
  float i0 = a0 + gap / r0, i1 = a1 - gap / r0, o0 = a0 + gap / r1, o1 = a1 - gap / r1;
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  g_gfx.factory->CreatePathGeometry(&path);
  path->Open(&sink);
  sink->BeginFigure(Polar(r1, o0), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddArc(D2D1::ArcSegment(Polar(r1, o1), D2D1::SizeF(r1, r1), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                o1 - o0 > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->AddLine(Polar(r0, i1));
  sink->AddArc(D2D1::ArcSegment(Polar(r0, i0), D2D1::SizeF(r0, r0), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                i1 - i0 > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  return path;
}

ComPtr<ID2D1Geometry> RoundedSector(float r0, float r1, float a0, float a1, float gap, float round) {
  auto base = SectorGeometry(r0 + round, r1 - round, a0, a1, gap + round);
  ComPtr<ID2D1PathGeometry> wide, merged;
  ComPtr<ID2D1GeometrySink> sink;
  g_gfx.factory->CreatePathGeometry(&wide);
  wide->Open(&sink);
  base->Widen(round * 2, g_gfx.round.Get(), nullptr, kTolerance, sink.Get());
  sink->Close();
  sink.Reset();
  g_gfx.factory->CreatePathGeometry(&merged);
  merged->Open(&sink);
  base->CombineWithGeometry(wide.Get(), D2D1_COMBINE_MODE_UNION, nullptr, kTolerance, sink.Get());
  sink->Close();
  return merged;
}

ComPtr<ID2D1PathGeometry> ArcGeometry(float r, float a0, float a1) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  g_gfx.factory->CreatePathGeometry(&path);
  path->Open(&sink);
  sink->BeginFigure(Polar(r, a0), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(Polar(r, a1), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                a1 - a0 > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  return path;
}

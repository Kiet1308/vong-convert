#include "crop.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

#include "actions.h"
#include "catalog.h"
#include "gfx.h"
#include "jobs.h"
#include "perf.h"
#include "spring.h"
#include "util.h"

namespace {

constexpr UINT WM_APP_PREVIEW = WM_APP + 30;
constexpr UINT WM_APP_BACKDROP = WM_APP + 31;
constexpr DWORD kExcludeFromCapture = 0x11;  // WDA_EXCLUDEFROMCAPTURE
using M = D2D1::Matrix3x2F;

struct Preset {
  const wchar_t* label;
  double ratio;  // width / height; 0 = free, -1 = the picture's own ratio
};
const Preset kPresets[] = {{L"Tự do", 0},      {L"Gốc", -1},       {L"1:1", 1},          {L"4:3", 4.0 / 3},
                           {L"3:2", 3.0 / 2}, {L"16:9", 16.0 / 9}, {L"9:16", 9.0 / 16}};
constexpr int kPresetCount = (int)std::size(kPresets);
enum ButtonId { kReset = 100, kCancel = 101, kApply = 102 };

enum class Drag { None, Move, New, N, S, E, W, NE, NW, SE, SW };

struct PreviewResult {
  bool ok;
  std::wstring png;
  int fullW, fullH;
};

struct Button {
  int id;
  std::wstring label;
  ComPtr<IDWriteTextLayout> layout;
  float textW = 0, textH = 0;
  D2D1_RECT_F rc{};
};

M Base(const D2D1_MATRIX_3X2_F& b) { return M(b._11, b._12, b._21, b._22, b._31, b._32); }

class CropView {
 public:
  CropView(const std::wstring& file, POINT anchor, bool test);
  ~CropView();
  bool Tick(float dt);
  void Render();
  bool Dead() const { return closing_ && alpha_.t == 0 && alpha_.v < .02f; }
  void Draw(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base);
  void OnPreview(PreviewResult* r);
  void TestSetup(float w, float h, float dpi, int preset);
  void SetBackground(ComPtr<ID2D1Bitmap1> bg) { bg_ = std::move(bg); }
  // The picture area in pixels: the window minus the 64-DIP sides, 92-DIP header and 112-DIP toolbar.
  int PreviewBoxW() const { return std::max(64, (int)((W_ - 128) * dpi_ / 96)); }
  int PreviewBoxH() const { return std::max(64, (int)((H_ - 204) * dpi_ / 96)); }
  void Preset(int i) { ApplyPreset(i); }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
  void BuildText();
  void Layout();
  void SetCrop(double x0, double y0, double x1, double y1, bool animate);
  void ApplyPreset(int i);
  Drag HitTest(float x, float y) const;
  int HitButton(float x, float y) const;
  void BeginDrag(float x, float y);
  void DragTo(float x, float y);
  void Press(int id);
  void Close(bool apply);
  D2D1_POINT_2F MousePos(LPARAM lp) const;
  LPCWSTR CursorFor(float x, float y) const;

  HWND hwnd_ = nullptr, prevForeground_ = nullptr;
  CompSurface surf_;
  bool test_ = false;
  float dpi_ = 96, W_ = 0, H_ = 0;
  RECT mon_{};
  std::wstring file_, name_;
  bool video_ = false;
  Theme t_;

  ComPtr<ID2D1Bitmap1> bg_, image_;
  int fullW_ = 0, fullH_ = 0;
  bool loading_ = true, failed_ = false, closing_ = false;
  D2D1_RECT_F img_{};
  double scale_ = 1;

  double c0x_ = 0, c0y_ = 0, c1x_ = 0, c1y_ = 0;  // crop in picture pixels
  double ratio_ = 0;
  int preset_ = 0;
  Spring ex0_, ey0_, ex1_, ey1_;  // the crop frame on screen, in DIPs
  Spring alpha_, imageA_, gridA_, bgA_;
  std::shared_ptr<ScreenGrab> grab_;
  float spin_ = 0;

  Drag drag_ = Drag::None;
  double ax_ = 0, ay_ = 0, grabX_ = 0, grabY_ = 0;
  double s0x_ = 0, s0y_ = 0, s1x_ = 0, s1y_ = 0;  // crop when the drag started
  std::vector<Button> buttons_;
  D2D1_RECT_F bar_{};
  int hover_ = -1, pressed_ = -1;

  ComPtr<IDWriteTextLayout> title_, subtitle_, hint_, status_, size_;
  std::wstring sizeText_;
};

std::unique_ptr<CropView> g_view;
bool g_registered = false;

// ---------------------------------------------------------------- setup

CropView::CropView(const std::wstring& file, POINT anchor, bool test) : test_(test), file_(file) {
  name_ = util::FileNameOf(file);
  video_ = KindOf(file) == Kind::Video;
  t_ = LoadTheme(1);  // a photo viewer reads best dark, whatever the system theme
  alpha_ = Spring(0, 380, 38);
  alpha_.To(1);
  imageA_ = Spring(0, 260, 32);
  bgA_ = Spring(test ? 1.f : 0.f, 300, 34);
  gridA_ = Spring(0, 500, 44);
  ex0_ = Spring(0, 620, 46, .01f);
  ey0_ = Spring(0, 620, 46, .01f);
  ex1_ = Spring(0, 620, 46, .01f);
  ey1_ = Spring(0, 620, 46, .01f);
  if (test) return;

  if (!g_registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"VongConvert.Crop";
    RegisterClassExW(&wc);
    g_registered = true;
  }
  HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  mon_ = mi.rcMonitor;
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  dpi_ = (float)dx;
  int w = mon_.right - mon_.left, h = mon_.bottom - mon_.top;
  W_ = w * 96.f / dpi_;
  H_ = h * 96.f / dpi_;
  prevForeground_ = GetForegroundWindow();

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"VongConvert.Crop",
                          L"Cắt", WS_POPUP, mon_.left, mon_.top, w, h, nullptr, nullptr, g_hinst, nullptr);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
  surf_.Attach(hwnd_);
  surf_.Ensure(w, h);
  // The blurred desktop arrives from a worker thread a moment later and fades in; until then the
  // dimmed live desktop shows through. This window is left out of that copy.
  SetWindowDisplayAffinity(hwnd_, kExcludeFromCapture);
  grab_ = GrabScreenAsync(mon_, 4, hwnd_, WM_APP_BACKDROP);
  BuildText();
  Layout();
  Render();
  ShowWindow(hwnd_, SW_SHOW);
  ForceForeground(hwnd_);
  SetFocus(hwnd_);

  std::wstring png = util::UniquePath(util::DataDir(), L"crop-preview", L"png");
  CloseHandle(CreateFileW(png.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));  // reserve the name
  HWND target = hwnd_;
  std::wstring src = file;
  int boxW = PreviewBoxW(), boxH = PreviewBoxH();
  std::thread([target, src, png, boxW, boxH] {
    auto* r = new PreviewResult{false, png, 0, 0};
    r->ok = MakePreview(src, png, boxW, boxH, &r->fullW, &r->fullH);
    if (!PostMessageW(target, WM_APP_PREVIEW, 0, (LPARAM)r)) {
      DeleteFileW(png.c_str());
      delete r;
    }
  }).detach();
  RequestAnimation();
}

CropView::~CropView() {
  if (hwnd_) {
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
  }
}

void CropView::TestSetup(float w, float h, float dpi, int preset) {
  W_ = w;
  H_ = h;
  dpi_ = dpi;
  BuildText();
  Layout();
  if (preset > 0) ApplyPreset(preset);
}

void CropView::BuildText() {
  auto* titleFmt = g_gfx.Format(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, false, true, DWRITE_TEXT_ALIGNMENT_LEADING);
  auto* smallFmt = g_gfx.Format(12.f, DWRITE_FONT_WEIGHT_NORMAL, false, false, true, DWRITE_TEXT_ALIGNMENT_LEADING);
  auto* hintFmt = g_gfx.Format(12.f, DWRITE_FONT_WEIGHT_NORMAL, false, false, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
  title_ = g_gfx.Layout((video_ ? L"Cắt khung video · " : L"Cắt ảnh · ") + name_, titleFmt, W_ * .55f);
  std::wstring sub = fullW_ ? std::to_wstring(fullW_) + L" × " + std::to_wstring(fullH_) + L" px" : L"Đang mở…";
  if (video_ && fullW_) sub += L" · xem trước khung hình đầu";
  subtitle_ = g_gfx.Layout(sub, smallFmt, W_ * .55f);
  hint_ = g_gfx.Layout(L"Kéo để chọn vùng  ·  Enter để cắt  ·  Esc để huỷ", hintFmt, W_ * .4f);

  auto* btnFmt = g_gfx.Format(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  buttons_.clear();
  auto add = [&](int id, const std::wstring& label) {
    Button b;
    b.id = id;
    b.label = label;
    b.layout = g_gfx.Layout(label, btnFmt, 200);
    DWRITE_TEXT_METRICS m{};
    b.layout->GetMetrics(&m);
    b.textW = m.width;
    b.textH = m.height;
    b.layout->SetMaxWidth(m.width + 28);
    buttons_.push_back(b);
  };
  for (int i = 0; i < kPresetCount; ++i) add(i, kPresets[i].label);
  add(kReset, L"Đặt lại");
  add(kCancel, L"Huỷ");
  add(kApply, video_ ? L"Cắt video" : L"Cắt ảnh");
}

void CropView::Layout() {
  // Toolbar: presets, a gap, then the actions.
  float h = 36, pad = 6, x = 0;
  for (auto& b : buttons_) {
    if (b.id == kReset) x += 18;
    float w = b.textW + 28;
    b.rc = D2D1::RectF(x, 0, x + w, h);
    x += w + 4;
  }
  float total = x - 4 + pad * 2;
  float bx = (W_ - total) / 2, by = H_ - 78;
  bar_ = D2D1::RectF(bx, by, bx + total, by + h + pad * 2);
  for (auto& b : buttons_) {
    b.rc.left += bx + pad;
    b.rc.right += bx + pad;
    b.rc.top = by + pad;
    b.rc.bottom = by + pad + h;
  }
  if (!fullW_) return;
  float top = 92, bottom = 112, side = 64;
  double availW = W_ - side * 2, availH = H_ - top - bottom;
  scale_ = std::min(availW / fullW_, availH / fullH_);
  float iw = (float)(fullW_ * scale_), ih = (float)(fullH_ * scale_);
  img_ = D2D1::RectF((W_ - iw) / 2, top + (float)(availH - ih) / 2, (W_ + iw) / 2, top + (float)(availH + ih) / 2);
}

void CropView::OnPreview(PreviewResult* r) {
  UINT pw = 0, ph = 0;
  if (r->ok) image_ = LoadImageFile(r->png, &pw, &ph);
  DeleteFileW(r->png.c_str());
  loading_ = false;
  if (!image_) {
    failed_ = true;
    util::Log(L"crop: no preview for %ls", file_.c_str());
  } else {
    fullW_ = r->fullW > 0 ? r->fullW : (int)pw;
    fullH_ = r->fullH > 0 ? r->fullH : (int)ph;
    BuildText();
    Layout();
    // Open with the frame on the whole picture; it settles in from slightly inside.
    SetCrop(fullW_ * .04, fullH_ * .04, fullW_ * .96, fullH_ * .96, false);
    SetCrop(0, 0, fullW_, fullH_, true);
    imageA_.To(1);
  }
  RequestAnimation();
}

// ---------------------------------------------------------------- crop logic

void CropView::SetCrop(double x0, double y0, double x1, double y1, bool animate) {
  c0x_ = std::clamp(std::min(x0, x1), 0.0, (double)fullW_);
  c1x_ = std::clamp(std::max(x0, x1), 0.0, (double)fullW_);
  c0y_ = std::clamp(std::min(y0, y1), 0.0, (double)fullH_);
  c1y_ = std::clamp(std::max(y0, y1), 0.0, (double)fullH_);
  float l = img_.left + (float)(c0x_ * scale_), r = img_.left + (float)(c1x_ * scale_);
  float t = img_.top + (float)(c0y_ * scale_), b = img_.top + (float)(c1y_ * scale_);
  if (animate && !ReducedMotion()) {
    ex0_.To(l); ey0_.To(t); ex1_.To(r); ey1_.To(b);
  } else {
    ex0_.Snap(l); ey0_.Snap(t); ex1_.Snap(r); ey1_.Snap(b);
  }
  RequestAnimation();
}

void CropView::ApplyPreset(int i) {
  if (!fullW_ || i < 0 || i >= kPresetCount) return;
  preset_ = i;
  double r = kPresets[i].ratio;
  if (r < 0) r = (double)fullW_ / fullH_;
  ratio_ = r;
  if (r == 0) return;
  // The largest frame of that shape, kept around the current centre.
  double w = fullW_, h = w / r;
  if (h > fullH_) { h = fullH_; w = h * r; }
  double cx = (c0x_ + c1x_) / 2, cy = (c0y_ + c1y_) / 2;
  double x0 = std::clamp(cx - w / 2, 0.0, fullW_ - w), y0 = std::clamp(cy - h / 2, 0.0, fullH_ - h);
  SetCrop(x0, y0, x0 + w, y0 + h, true);
}

Drag CropView::HitTest(float x, float y) const {
  if (!image_) return Drag::None;
  float l = ex0_.t, t = ey0_.t, r = ex1_.t, b = ey1_.t;
  auto within = [](float a, float b, float d) { return std::fabs(a - b) <= d; };
  const float c = 16, e = 10;
  if (within(x, l, c) && within(y, t, c)) return Drag::NW;
  if (within(x, r, c) && within(y, t, c)) return Drag::NE;
  if (within(x, l, c) && within(y, b, c)) return Drag::SW;
  if (within(x, r, c) && within(y, b, c)) return Drag::SE;
  bool inY = y > t && y < b, inX = x > l && x < r;
  if (within(x, l, e) && inY) return Drag::W;
  if (within(x, r, e) && inY) return Drag::E;
  if (within(y, t, e) && inX) return Drag::N;
  if (within(y, b, e) && inX) return Drag::S;
  if (inX && inY) return Drag::Move;
  if (x >= img_.left && x <= img_.right && y >= img_.top && y <= img_.bottom) return Drag::New;
  return Drag::None;
}

int CropView::HitButton(float x, float y) const {
  for (auto& b : buttons_)
    if (x >= b.rc.left && x < b.rc.right && y >= b.rc.top && y < b.rc.bottom) return b.id;
  return -1;
}

void CropView::BeginDrag(float x, float y) {
  drag_ = HitTest(x, y);
  if (drag_ == Drag::None) return;
  double px = std::clamp((x - img_.left) / scale_, 0.0, (double)fullW_);
  double py = std::clamp((y - img_.top) / scale_, 0.0, (double)fullH_);
  s0x_ = c0x_; s0y_ = c0y_; s1x_ = c1x_; s1y_ = c1y_;
  switch (drag_) {
    case Drag::Move: grabX_ = px - c0x_; grabY_ = py - c0y_; break;
    case Drag::New: ax_ = px; ay_ = py; break;
    case Drag::NW: ax_ = c1x_; ay_ = c1y_; break;
    case Drag::NE: ax_ = c0x_; ay_ = c1y_; break;
    case Drag::SW: ax_ = c1x_; ay_ = c0y_; break;
    case Drag::SE: ax_ = c0x_; ay_ = c0y_; break;
    default: break;
  }
  gridA_.To(1);
  if (!test_) SetCapture(hwnd_);
}

void CropView::DragTo(float x, float y) {
  double mx = std::clamp((x - img_.left) / scale_, 0.0, (double)fullW_);
  double my = std::clamp((y - img_.top) / scale_, 0.0, (double)fullH_);
  const double minSize = std::max(4.0, 12 / scale_);
  double r = ratio_;
  switch (drag_) {
    case Drag::Move: {
      double w = c1x_ - c0x_, h = c1y_ - c0y_;
      double nx = std::clamp(mx - grabX_, 0.0, fullW_ - w), ny = std::clamp(my - grabY_, 0.0, fullH_ - h);
      SetCrop(nx, ny, nx + w, ny + h, false);
      break;
    }
    case Drag::New: case Drag::NW: case Drag::NE: case Drag::SW: case Drag::SE: {
      double dx = mx - ax_, dy = my - ay_;
      double sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
      double w = std::fabs(dx), h = std::fabs(dy);
      double maxW = sx > 0 ? fullW_ - ax_ : ax_, maxH = sy > 0 ? fullH_ - ay_ : ay_;
      if (r > 0) {
        if (w / std::max(h, 1e-6) > r) h = w / r; else w = h * r;  // grow until the pointer is inside
        if (w > maxW) { w = maxW; h = w / r; }
        if (h > maxH) { h = maxH; w = h * r; }
        if (w < minSize) { w = minSize; h = w / r; }
      } else {
        w = std::clamp(w, minSize, std::max(minSize, maxW));
        h = std::clamp(h, minSize, std::max(minSize, maxH));
      }
      double x0 = sx > 0 ? ax_ : ax_ - w, y0 = sy > 0 ? ay_ : ay_ - h;
      SetCrop(x0, y0, x0 + w, y0 + h, false);
      break;
    }
    case Drag::E: case Drag::W: {
      double x0 = s0x_, x1 = s1x_;
      if (drag_ == Drag::E) x1 = std::max(mx, x0 + minSize); else x0 = std::min(mx, x1 - minSize);
      double y0 = s0y_, y1 = s1y_;
      if (r > 0) {
        double w = x1 - x0, h = w / r;
        if (h > fullH_) { h = fullH_; w = h * r; if (drag_ == Drag::E) x1 = x0 + w; else x0 = x1 - w; }
        double cy = (s0y_ + s1y_) / 2;
        y0 = std::clamp(cy - h / 2, 0.0, fullH_ - h);
        y1 = y0 + h;
      }
      SetCrop(x0, y0, x1, y1, false);
      break;
    }
    case Drag::N: case Drag::S: {
      double y0 = s0y_, y1 = s1y_;
      if (drag_ == Drag::S) y1 = std::max(my, y0 + minSize); else y0 = std::min(my, y1 - minSize);
      double x0 = s0x_, x1 = s1x_;
      if (r > 0) {
        double h = y1 - y0, w = h * r;
        if (w > fullW_) { w = fullW_; h = w / r; if (drag_ == Drag::S) y1 = y0 + h; else y0 = y1 - h; }
        double cx = (s0x_ + s1x_) / 2;
        x0 = std::clamp(cx - w / 2, 0.0, fullW_ - w);
        x1 = x0 + w;
      }
      SetCrop(x0, y0, x1, y1, false);
      break;
    }
    case Drag::None: break;
  }
}

void CropView::Press(int id) {
  if (id >= 0 && id < kPresetCount) ApplyPreset(id);
  else if (id == kReset) {
    preset_ = 0;
    ratio_ = 0;
    SetCrop(0, 0, fullW_, fullH_, true);
  } else if (id == kCancel) Close(false);
  else if (id == kApply) Close(true);
}

void CropView::Close(bool apply) {
  if (closing_) return;
  if (apply && (!image_ || c1x_ - c0x_ < 1 || c1y_ - c0y_ < 1)) return;
  closing_ = true;
  alpha_.k = 700;
  alpha_.c = 56;
  alpha_.To(0);
  if (!test_) {
    ReleaseCapture();
    if (prevForeground_ && IsWindow(prevForeground_)) SetForegroundWindow(prevForeground_);
  }
  if (apply) {
    int x = (int)std::lround(c0x_), y = (int)std::lround(c0y_);
    int w = (int)std::lround(c1x_) - x, h = (int)std::lround(c1y_) - y;
    float s = dpi_ / 96.f;
    POINT center{mon_.left + (LONG)std::lround((ex0_.t + ex1_.t) / 2 * s),
                 mon_.top + (LONG)std::lround((ey0_.t + ey1_.t) / 2 * s)};
    util::Log(L"crop %ls to %d,%d %dx%d", file_.c_str(), x, y, w, h);
    OnCropConfirmed(file_, x, y, w, h, center, dpi_);
  }
  RequestAnimation();
}

// ---------------------------------------------------------------- input

D2D1_POINT_2F CropView::MousePos(LPARAM lp) const {
  float s = 96.f / dpi_;
  return D2D1::Point2F(GET_X_LPARAM(lp) * s, GET_Y_LPARAM(lp) * s);
}

LPCWSTR CropView::CursorFor(float x, float y) const {
  if (HitButton(x, y) >= 0) return IDC_HAND;
  switch (drag_ != Drag::None ? drag_ : HitTest(x, y)) {
    case Drag::NW: case Drag::SE: return IDC_SIZENWSE;
    case Drag::NE: case Drag::SW: return IDC_SIZENESW;
    case Drag::E: case Drag::W: return IDC_SIZEWE;
    case Drag::N: case Drag::S: return IDC_SIZENS;
    case Drag::Move: return IDC_SIZEALL;
    case Drag::New: return IDC_CROSS;
    default: return IDC_ARROW;
  }
}

LRESULT CALLBACK CropView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = (CropView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (self) return self->Proc(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CropView::Proc(UINT msg, WPARAM wp, LPARAM lp) {
  if (closing_ && msg != WM_APP_PREVIEW && msg != WM_APP_BACKDROP) {
    if (msg == WM_SETCURSOR) { SetCursor(LoadCursor(nullptr, IDC_ARROW)); return TRUE; }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
  }
  switch (msg) {
    case WM_APP_BACKDROP:
      if (grab_ && grab_->done) {
        bg_ = BlurGrab(*grab_, dpi_, 26.f, 1.15f);
        grab_.reset();
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        bgA_.To(1);
        RequestAnimation();
      }
      return 0;
    case WM_APP_PREVIEW: {
      auto* r = (PreviewResult*)lp;
      if (!closing_) OnPreview(r);
      else DeleteFileW(r->png.c_str());
      delete r;
      return 0;
    }
    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd_, &pt);
      float s = 96.f / dpi_;
      SetCursor(LoadCursor(nullptr, CursorFor(pt.x * s, pt.y * s)));
      return TRUE;
    }
    case WM_MOUSEMOVE: {
      auto p = MousePos(lp);
      if (drag_ != Drag::None) DragTo(p.x, p.y);
      int h = HitButton(p.x, p.y);
      if (h != hover_) { hover_ = h; RequestAnimation(); }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      auto p = MousePos(lp);
      int b = HitButton(p.x, p.y);
      if (b >= 0) { pressed_ = b; SetCapture(hwnd_); RequestAnimation(); return 0; }
      BeginDrag(p.x, p.y);
      if (drag_ == Drag::New) DragTo(p.x, p.y);
      return 0;
    }
    case WM_LBUTTONUP: {
      auto p = MousePos(lp);
      ReleaseCapture();
      if (pressed_ >= 0) {
        int b = pressed_;
        pressed_ = -1;
        if (HitButton(p.x, p.y) == b) Press(b);
        RequestAnimation();
      }
      if (drag_ != Drag::None) {
        drag_ = Drag::None;
        gridA_.To(0);
        // A click without dragging on the picture would leave a tiny frame: undo it.
        if (c1x_ - c0x_ < 2 / scale_ || c1y_ - c0y_ < 2 / scale_) SetCrop(s0x_, s0y_, s1x_, s1y_, true);
        RequestAnimation();
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      auto p = MousePos(lp);
      if (HitTest(p.x, p.y) == Drag::Move) Close(true);
      return 0;
    }
    case WM_KEYDOWN: {
      bool shift = GetKeyState(VK_SHIFT) < 0;
      double step = (shift ? 10 : 1) / std::max(scale_, 1e-6);  // one (or ten) screen pixels
      double dx = 0, dy = 0;
      switch (wp) {
        case VK_ESCAPE: Close(false); return 0;
        case VK_RETURN: Close(true); return 0;
        case 'R': Press(kReset); return 0;
        case VK_LEFT: dx = -step; break;
        case VK_RIGHT: dx = step; break;
        case VK_UP: dy = -step; break;
        case VK_DOWN: dy = step; break;
        default:
          if (wp >= '1' && wp < '1' + kPresetCount) ApplyPreset((int)(wp - '1'));
          return 0;
      }
      double w = c1x_ - c0x_, h = c1y_ - c0y_;
      double nx = std::clamp(c0x_ + dx, 0.0, fullW_ - w), ny = std::clamp(c0y_ + dy, 0.0, fullH_ - h);
      SetCrop(nx, ny, nx + w, ny + h, true);
      return 0;
    }
    case WM_DPICHANGED: return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------- frame

bool CropView::Tick(float dt) {
  bool moving = loading_;
  if (loading_) spin_ += dt * 360;
  for (Spring* s : {&alpha_, &imageA_, &gridA_, &bgA_, &ex0_, &ey0_, &ex1_, &ey1_}) {
    s->Step(dt);
    moving |= s->Moving();
  }
  return moving;
}

void CropView::Render() {
  if (!surf_.Ensure((UINT)(mon_.right - mon_.left), (UINT)(mon_.bottom - mon_.top))) return;
  auto* dc = surf_.BeginDraw(dpi_);
  if (!dc) return;
  Draw(dc, surf_.Base());
  surf_.EndDraw();
}

void CropView::Draw(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
  const Theme& t = t_;
  M B = Base(base);
  float A = std::clamp(alpha_.v, 0.f, 1.f);
  auto white = D2D1::ColorF(1, 1, 1, 1), black = D2D1::ColorF(0, 0, 0, 1);
  dc->SetTransform(B);

  // Backdrop: the desktop, blurred and dimmed.
  if (bg_) {
    D2D1_RECT_F all = D2D1::RectF(0, 0, W_, H_);
    dc->DrawBitmap(bg_.Get(), &all, A * std::clamp(bgA_.v, 0.f, 1.f), D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
  }
  dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), g_gfx.Brush(D2D1::ColorF(0.03f, 0.035f, 0.05f, 1), .66f * A));

  // Header.
  float side = 64;
  if (title_) dc->DrawTextLayout(D2D1::Point2F(side, 28), title_.Get(), g_gfx.Brush(white, A));
  if (subtitle_) dc->DrawTextLayout(D2D1::Point2F(side, 52), subtitle_.Get(), g_gfx.Brush(t.fg2, A));
  if (hint_) dc->DrawTextLayout(D2D1::Point2F(W_ - side - W_ * .4f, 38), hint_.Get(), g_gfx.Brush(t.fg2, A));

  if (loading_ || failed_) {
    float cx = W_ / 2, cy = (H_ - 40) / 2;
    if (loading_) {
      dc->SetTransform(M::Rotation(spin_) * M::Translation(cx, cy) * B);
      dc->DrawGeometry(ArcGeometry(18, 0, 4.2f).Get(), g_gfx.Brush(t.accent, A), 3, g_gfx.round.Get());
      dc->SetTransform(B);
    }
    std::wstring msg = loading_ ? (video_ ? L"Đang lấy khung hình…" : L"Đang mở ảnh…") : L"Không mở được file này. Bấm Esc để đóng.";
    status_ = g_gfx.Layout(msg, g_gfx.Format(13.f, DWRITE_FONT_WEIGHT_NORMAL), 600);
    dc->DrawTextLayout(D2D1::Point2F(cx - 300, cy + (loading_ ? 34.f : -8.f)), status_.Get(), g_gfx.Brush(t.fg2, A));
  }

  if (image_) {
    float ia = A * std::clamp(imageA_.v, 0.f, 1.f);
    float zoom = .975f + .025f * std::clamp(imageA_.v, 0.f, 1.f);
    D2D1_POINT_2F ic = D2D1::Point2F((img_.left + img_.right) / 2, (img_.top + img_.bottom) / 2);
    dc->SetTransform(M::Scale(zoom, zoom, ic) * B);
    // Soft shadow under the picture.
    for (int i = 4; i >= 1; --i) {
      float g = i * 5.f;
      dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(img_.left - g, img_.top - g + 10, img_.right + g, img_.bottom + g + 10), g, g),
                               g_gfx.Brush(black, .07f * ia));
    }
    dc->DrawBitmap(image_.Get(), &img_, ia, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);

    D2D1_RECT_F c = D2D1::RectF(ex0_.v, ey0_.v, ex1_.v, ey1_.v);
    auto shade = g_gfx.Brush(D2D1::ColorF(0, 0, 0, 1), .6f * ia);
    dc->FillRectangle(D2D1::RectF(img_.left, img_.top, img_.right, c.top), shade);
    dc->FillRectangle(D2D1::RectF(img_.left, c.bottom, img_.right, img_.bottom), shade);
    dc->FillRectangle(D2D1::RectF(img_.left, c.top, c.left, c.bottom), shade);
    dc->FillRectangle(D2D1::RectF(c.right, c.top, img_.right, c.bottom), shade);

    float g = std::clamp(gridA_.v, 0.f, 1.f);
    if (g > .01f) {
      auto grid = g_gfx.Brush(white, .42f * g * ia);
      for (int i = 1; i <= 2; ++i) {
        float x = c.left + (c.right - c.left) * i / 3, y = c.top + (c.bottom - c.top) * i / 3;
        dc->DrawLine(D2D1::Point2F(x, c.top), D2D1::Point2F(x, c.bottom), grid, 1);
        dc->DrawLine(D2D1::Point2F(c.left, y), D2D1::Point2F(c.right, y), grid, 1);
      }
    }
    dc->DrawRectangle(c, g_gfx.Brush(white, .9f * ia), 1.2f);

    // Handles: L-shaped corners and short bars on the edges.
    auto hb = g_gfx.Brush(white, ia);
    float L = std::min(22.f, std::min(c.right - c.left, c.bottom - c.top) / 2), th = 3.5f, o = 1.6f;
    auto corner = [&](float x, float y, float sx, float sy) {
      dc->FillRectangle(D2D1::RectF(std::min(x - sx * o, x + sx * L), std::min(y - sy * o, y - sy * o + sy * th),
                                    std::max(x - sx * o, x + sx * L), std::max(y - sy * o, y - sy * o + sy * th)), hb);
      dc->FillRectangle(D2D1::RectF(std::min(x - sx * o, x - sx * o + sx * th), std::min(y - sy * o, y + sy * L),
                                    std::max(x - sx * o, x - sx * o + sx * th), std::max(y - sy * o, y + sy * L)), hb);
    };
    corner(c.left, c.top, 1, 1);
    corner(c.right, c.top, -1, 1);
    corner(c.left, c.bottom, 1, -1);
    corner(c.right, c.bottom, -1, -1);
    float mx = (c.left + c.right) / 2, my = (c.top + c.bottom) / 2, bar = 12;
    if (c.right - c.left > 80) {
      dc->FillRectangle(D2D1::RectF(mx - bar, c.top - o, mx + bar, c.top - o + th), hb);
      dc->FillRectangle(D2D1::RectF(mx - bar, c.bottom + o - th, mx + bar, c.bottom + o), hb);
    }
    if (c.bottom - c.top > 80) {
      dc->FillRectangle(D2D1::RectF(c.left - o, my - bar, c.left - o + th, my + bar), hb);
      dc->FillRectangle(D2D1::RectF(c.right + o - th, my - bar, c.right + o, my + bar), hb);
    }

    // Size of the result, in real pixels.
    int pw = (int)std::lround(c1x_) - (int)std::lround(c0x_), ph = (int)std::lround(c1y_) - (int)std::lround(c0y_);
    std::wstring st = std::to_wstring(pw) + L" × " + std::to_wstring(ph);
    if (st != sizeText_) {
      sizeText_ = st;
      size_ = g_gfx.Layout(st, g_gfx.Format(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), 200);
    }
    if (size_) {
      DWRITE_TEXT_METRICS m{};
      size_->GetMetrics(&m);
      float w = m.width + 20, y = std::min(c.bottom + 12, bar_.top - 34);
      auto pill = D2D1::RoundedRect(D2D1::RectF(mx - w / 2, y, mx + w / 2, y + 24), 12, 12);
      dc->FillRoundedRectangle(pill, g_gfx.Brush(black, .62f * ia));
      dc->DrawTextLayout(D2D1::Point2F(mx - 100, y + 12 - m.height / 2), size_.Get(), g_gfx.Brush(white, ia));
    }
  }

  // Toolbar.
  dc->SetTransform(B);
  auto barRect = D2D1::RoundedRect(bar_, (bar_.bottom - bar_.top) / 2, (bar_.bottom - bar_.top) / 2);
  dc->FillRoundedRectangle(barRect, g_gfx.Brush(D2D1::ColorF(0.13f, 0.135f, 0.155f, 1), .94f * A));
  dc->DrawRoundedRectangle(barRect, g_gfx.Brush(white, .08f * A), 1);
  for (auto& b : buttons_) {
    bool active = (b.id < kPresetCount && b.id == preset_) || b.id == kApply;
    bool hot = b.id == hover_ || b.id == pressed_;
    bool disabled = !image_ && b.id != kCancel;
    float r = (b.rc.bottom - b.rc.top) / 2;
    auto rr = D2D1::RoundedRect(b.rc, r, r);
    if (active) dc->FillRoundedRectangle(rr, g_gfx.Brush(t.accent, (hot ? 1.f : .92f) * A * (disabled ? .4f : 1)));
    else if (hot) dc->FillRoundedRectangle(rr, g_gfx.Brush(white, .09f * A));
    if (b.id == kReset) {
      float sx = b.rc.left - 11;
      dc->DrawLine(D2D1::Point2F(sx, b.rc.top + 8), D2D1::Point2F(sx, b.rc.bottom - 8), g_gfx.Brush(white, .14f * A), 1);
    }
    D2D1_COLOR_F col = active ? t.onAccent : (b.id < kPresetCount ? t.fg2 : white);
    if (hot && !active) col = white;
    dc->DrawTextLayout(D2D1::Point2F(b.rc.left, (b.rc.top + b.rc.bottom) / 2 - b.textH / 2), b.layout.Get(),
                       g_gfx.Brush(col, A * (disabled ? .4f : 1)));
  }
  dc->SetTransform(base);
}

}  // namespace

namespace crop {

void Open(const std::wstring& file, POINT anchor) {
  if (g_view && !g_view->Dead()) return;  // one at a time
  g_view = std::make_unique<CropView>(file, anchor, false);
}

bool Active() { return g_view != nullptr; }

bool Tick(float dt) {
  if (!g_view) return false;
  bool more = g_view->Tick(dt);
  if (g_view->Dead()) {
    g_view.reset();
    return false;
  }
  return more;
}

void Render() {
  if (g_view) g_view->Render();
}

void Close() { g_view.reset(); }

std::wstring Bench(const std::wstring& file) {
  if (!g_gfx.Ensure()) return L"no graphics";
  HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  float dpi = (float)dx;
  RECT r = mi.rcMonitor;
  UINT pw = r.right - r.left, ph = r.bottom - r.top;
  std::wstring out;

  perf::Stats copy, glass;
  ComPtr<ID2D1Bitmap1> bg;
  for (int i = 0; i < 6; ++i) {
    double t0 = perf::NowMs();
    auto grab = GrabScreenAsync(r, 4);
    while (!grab->done) Sleep(0);
    double t1 = perf::NowMs();
    bg = BlurGrab(*grab, dpi, 26.f, 1.15f);
    perf::WaitGpu();
    if (i) { copy.Add(t1 - t0); glass.Add(perf::NowMs() - t1); }
  }
  out += copy.Line(L"crop screen copy (worker thread)") + L"\n" + glass.Line(L"crop backdrop blur (UI thread)") + L"\n";
  g_gfx.Idle();
  out += L"memory after crop backdrops: " + perf::Memory() + L"\n";

  std::wstring png = util::UniquePath(util::DataDir(), L"crop-bench", L"png");
  double t0 = perf::NowMs();
  auto* res = new PreviewResult{false, png, 0, 0};
  res->ok = MakePreview(file, png, (int)(pw - 128 * dpi / 96), (int)(ph - 204 * dpi / 96), &res->fullW, &res->fullH);
  double t1 = perf::NowMs();
  CropView view(file, POINT{0, 0}, true);
  view.TestSetup(pw * 96.f / dpi, ph * 96.f / dpi, dpi, 0);
  view.SetBackground(bg);
  view.OnPreview(res);
  delete res;
  double t2 = perf::NowMs();
  wchar_t buf[160];
  swprintf(buf, 160, L"crop preview: ffmpeg %.0f ms, load %.0f ms\n", t1 - t0, t2 - t1);
  out += buf;
  out += L"memory with preview loaded: " + perf::Memory() + L"\n";

  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
  ComPtr<ID2D1Bitmap1> target;
  auto* dc = g_gfx.dc.Get();
  dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &props, &target);
  perf::Stats cpu, total;
  for (int f = 0; f < 240; ++f) {
    if (f % 40 == 0) view.Preset((f / 40) % kPresetCount);
    view.Tick(1 / 60.f);
    double a = perf::NowMs();
    dc->SetTarget(target.Get());
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    view.Draw(dc, D2D1::Matrix3x2F::Translation(0, 0));
    dc->EndDraw();
    dc->SetTarget(nullptr);
    double b = perf::NowMs();
    perf::WaitGpu();
    cpu.Add(b - a);
    total.Add(perf::NowMs() - a);
  }
  out += cpu.Line(L"crop frame CPU") + L"\n" + total.Line(L"crop frame CPU+GPU") + L"\n";
  target.Reset();
  g_gfx.Idle();
  out += L"memory after crop frames: " + perf::Memory() + L"\n";
  return out;
}

bool RenderTest(const std::wstring& file, const std::wstring& out, int preset) {
  if (!g_gfx.Ensure()) return false;
  const float dpi = 120, w = 1280, h = 800;
  CropView view(file, POINT{0, 0}, true);
  std::wstring png = util::UniquePath(util::DataDir(), L"crop-test", L"png");
  auto* r = new PreviewResult{false, png, 0, 0};
  r->ok = MakePreview(file, png, (int)((w - 128) * dpi / 96), (int)((h - 204) * dpi / 96), &r->fullW, &r->fullH);
  view.TestSetup(w, h, dpi, 0);
  view.OnPreview(r);
  delete r;
  view.TestSetup(w, h, dpi, preset);
  for (int i = 0; i < 120; ++i) view.Tick(1 / 60.f);

  auto* dc = g_gfx.dc.Get();
  UINT pw = (UINT)(w * dpi / 96), ph = (UINT)(h * dpi / 96);
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &props, &target))) return false;
  dc->SetTarget(target.Get());
  dc->SetDpi(dpi, dpi);
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0x2a3550));
  view.Draw(dc, D2D1::Matrix3x2F::Identity());
  HRESULT hr = dc->EndDraw();
  dc->SetTarget(nullptr);
  return SUCCEEDED(hr) && SavePng(target.Get(), out);
}

}  // namespace crop

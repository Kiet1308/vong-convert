#include "wheel.h"

#include <dwrite_1.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>

#include "perf.h"
#include "settings.h"
#include "util.h"

Wheel g_wheel;

namespace {

// Wheel layout in DIPs, same numbers as the HTML prototype.
struct Geo {
  float ro = 150, ri = 64, rh = 55, so = 48;
  float sz, r2i, r2o, rm, subStep;
  Geo() {
    sz = 2 * (ro + so + 24);
    r2i = ro + 8;
    r2o = ro + 8 + so;
    rm = (ri + ro) / 2;
    subStep = 78.f / ((r2i + r2o) / 2);
  }
};
const Geo G;

constexpr UINT_PTR kPollTimer = 1;
constexpr DWORD kExcludeFromCapture = 0x11;  // WDA_EXCLUDEFROMCAPTURE, Windows 10 2004+
constexpr float kTau = 2 * kPi;

float SegAngle(int i, int n) { return -kPi / 2 + i * kTau / n; }
float AngDiff(float a, float b) {
  float d = a - b;
  while (d > kPi) d -= kTau;
  while (d < -kPi) d += kTau;
  return d;
}
float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
int Mod(int a, int n) { return ((a % n) + n) % n; }
bool LmbDown() {
  int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Segment shapes depend only on how many segments there are, so they are built once and reused.
std::map<std::tuple<int, int, int, int>, ComPtr<ID2D1Geometry>> g_sectors;
int g_sectorsGen = -1;

template <class Make>
ComPtr<ID2D1Geometry> CachedSector(int n, int i, int m, int j, Make make) {
  if (g_sectorsGen != g_gfx.generation) {
    g_sectors.clear();
    g_sectorsGen = g_gfx.generation;
  }
  auto& slot = g_sectors[{n, i, m, j}];
  if (!slot) slot = make();
  return slot;
}

// Row-vector convention like D2D: apply a, then b.
D2D1_MATRIX_3X2_F Mul(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) {
  D2D1_MATRIX_3X2_F r;
  r._11 = a._11 * b._11 + a._12 * b._21;
  r._12 = a._11 * b._12 + a._12 * b._22;
  r._21 = a._21 * b._11 + a._22 * b._21;
  r._22 = a._21 * b._12 + a._22 * b._22;
  r._31 = a._31 * b._11 + a._32 * b._21 + b._31;
  r._32 = a._31 * b._12 + a._32 * b._22 + b._32;
  return r;
}

ComPtr<ID2D1RadialGradientBrush> ShadowBrush(const D2D1_COLOR_F& color, float alpha, float radius, float blur,
                                             float dy) {
  auto* dc = g_gfx.dc.Get();
  float R = radius + blur;
  D2D1_GRADIENT_STOP stops[] = {
      {std::max(0.f, (radius - blur * .5f) / R), WithAlpha(color, alpha)},
      {radius / R, WithAlpha(color, alpha * .55f)},
      {(radius + blur * .45f) / R, WithAlpha(color, alpha * .16f)},
      {1.f, WithAlpha(color, 0)},
  };
  ComPtr<ID2D1GradientStopCollection> coll;
  dc->CreateGradientStopCollection(stops, 4, &coll);
  ComPtr<ID2D1RadialGradientBrush> b;
  dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, dy), D2D1::Point2F(0, 0), R, R),
                                coll.Get(), &b);
  return b;
}

ComPtr<ID2D1LinearGradientBrush> HighlightBrush(const D2D1_COLOR_F& hi, float r) {
  auto* dc = g_gfx.dc.Get();
  D2D1_GRADIENT_STOP stops[] = {{0, hi}, {.55f, WithAlpha(hi, 0)}};
  ComPtr<ID2D1GradientStopCollection> coll;
  dc->CreateGradientStopCollection(stops, 2, &coll);
  ComPtr<ID2D1LinearGradientBrush> b;
  dc->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(D2D1::Point2F(-.34f * r, -r), D2D1::Point2F(.34f * r, r)), coll.Get(), &b);
  return b;
}

class DropTarget final : public IDropTarget {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *ppv = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
    *effect = g_wheel.OnDragEnter(data, pt, *effect);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
    *effect = g_wheel.OnDragOver(pt, *effect);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    g_wheel.OnDragLeave();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL pt, DWORD* effect) override {
    *effect = g_wheel.OnDrop(pt, *effect);
    return S_OK;
  }

 private:
  LONG ref_ = 1;
};

std::vector<std::wstring> FilesFrom(IDataObject* data) {
  std::vector<std::wstring> files;
  FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM stg{};
  if (FAILED(data->GetData(&fe, &stg))) return files;
  if (auto drop = (HDROP)GlobalLock(stg.hGlobal)) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
      UINT len = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring p(len, L'\0');
      DragQueryFileW(drop, i, p.data(), len + 1);
      files.push_back(p);
    }
    GlobalUnlock(stg.hGlobal);
  }
  ReleaseStgMedium(&stg);
  return files;
}

DWORD Accept(DWORD allowed) {
  if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
  if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
  return DROPEFFECT_NONE;
}

}  // namespace

// ---------------------------------------------------------------- window

bool Wheel::Create() {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = g_hinst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"VongConvert.Wheel";
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
                          L"Vòng Convert", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_hinst, nullptr);
  if (!hwnd_) return false;
  surf_.Attach(hwnd_);
  drop_ = new DropTarget();
  HRESULT hr = RegisterDragDrop(hwnd_, drop_);
  if (FAILED(hr)) util::Log(L"RegisterDragDrop failed 0x%08X", (unsigned)hr);
  return SUCCEEDED(hr);
}

void Wheel::Destroy() {
  if (hwnd_) {
    RevokeDragDrop(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (drop_) {
    drop_->Release();
    drop_ = nullptr;
  }
  ReleaseGraphics();
}

void Wheel::ReleaseGraphics() {
  segs_.clear();
  subs_.clear();
  blur_.Reset();
  blurBrush_.Reset();
  ring_.Reset();
  shadowRing_.Reset();
  hubShadowRing_.Reset();
  notch_.Reset();
  ringShadow_.Reset();
  hubShadow_.Reset();
  ringHi_.Reset();
  hubHi_.Reset();
  h1_.Reset();
  h2_.Reset();
  h3_.Reset();
  h4_.Reset();
  for (auto& p : pills_) p = Pill{};
  surf_.Reset();
}

LRESULT CALLBACK Wheel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
      if (wp == kPollTimer) g_wheel.OnPollTimer();
      return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: g_wheel.OnMouse(msg, lp); return 0;
    case WM_DPICHANGED: return 0;  // we size the window ourselves
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- state machine

void Wheel::OnModifier(int mod, bool down) {
  if (!down || mod < 0 || mod > 2 || kb_) return;  // the keyboard wheel switches with Tab instead
  Mode m = (Mode)g_settings.keyMode[mod];
  switch (state_) {
    case State::Hidden:
      if (!paused_ && LmbDown()) Arm(m);
      break;
    case State::Armed: pendingMode_ = m; break;
    case State::Open:
      if (!commit_) SwitchMode(m);
      break;
    case State::Closing: break;
  }
}

// A drag might be under way: put an invisible drop target under the cursor and wait
// for OLE to tell us whether it carries files.
void Wheel::Arm(Mode mode) {
  POINT pt;
  GetCursorPos(&pt);
  if (!Place(pt)) return;
  pendingMode_ = mode;
  state_ = State::Armed;
  kb_ = false;
  lmbUpSince_ = 0;
  SetTimer(hwnd_, kPollTimer, 40, nullptr);

  // Nudge the cursor so the drag loop re-checks which window is under it right away.
  INPUT in[2]{};
  in[0].type = in[1].type = INPUT_MOUSE;
  in[0].mi.dwFlags = in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
  in[0].mi.dx = 1;
  in[1].mi.dx = -1;
  SendInput(2, in, sizeof(INPUT));
}

void Wheel::OpenKeyboard(const std::vector<std::wstring>& files, POINT anchor, HWND owner) {
  if (state_ != State::Hidden || files.empty() || !Place(anchor)) return;
  files_ = files;
  kb_ = true;
  kbOwner_ = owner;
  Open(Mode::Convert);
  if (!items_.empty()) SetSel(0, -1, true);
  BuildLegend();
  SetKeyCapture(true);
  SetTimer(hwnd_, kPollTimer, 60, nullptr);
}

void Wheel::OnKey(UINT vk, bool shift) {
  if (!kb_ || state_ != State::Open || commit_) return;
  int n = (int)items_.size();
  const Item* it = sel_ >= 0 && sel_ < n ? &items_[sel_] : nullptr;
  switch (vk) {
    case VK_RIGHT: case VK_LEFT: {
      if (!n) break;
      int dir = vk == VK_RIGHT ? 1 : -1;
      if (it && !it->subs.empty() && sub_ >= 0) SetSel(sel_, std::clamp(sub_ + dir, 0, (int)it->subs.size() - 1));
      else SetSel(Mod((sel_ < 0 ? (dir > 0 ? -1 : 0) : sel_) + dir, n), -1);
      break;
    }
    case VK_UP:
      if (it && !it->subs.empty()) SetSel(sel_, sub_ < 0 ? it->def : sub_);
      break;
    case VK_DOWN:
      if (sub_ >= 0) SetSel(sel_, -1);
      break;
    case VK_TAB:
      SwitchMode((Mode)Mod((int)mode_ + (shift ? -1 : 1), 3));
      if (!items_.empty()) SetSel(0, -1, true);
      break;
    case VK_RETURN: case VK_SPACE:
      if (sel_ >= 0) Commit();
      break;
    case VK_ESCAPE: Close(false); break;
  }
  RequestAnimation();
}

void Wheel::OnMouse(UINT msg, LPARAM lp) {
  if (!kb_ || state_ != State::Open || commit_) return;
  POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
  ClientToScreen(hwnd_, &pt);
  Hit h = HitTest(pt);
  bool onItem = h.zone == Hit::Segment || h.zone == Hit::Sub;
  if (msg == WM_MOUSEMOVE && onItem) SetSel(h.i, h.zone == Hit::Sub ? h.j : -1);
  if (msg == WM_LBUTTONUP) {
    if (onItem) {
      SetSel(h.i, h.zone == Hit::Sub ? h.j : -1);
      Commit();
    } else if (h.zone == Hit::Center) {
      Close(false);
    }
  }
  RequestAnimation();
}

// Sizes and shows the (still empty) wheel window around `pt`, kept clear of the screen edges.
bool Wheel::Place(POINT pt) {
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  dpi_ = (float)dx;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  float s = dpi_ / 96.f;
  int half = (int)std::ceil(G.sz / 2 * s);
  int margin = (int)((G.ro + G.so + 12) * s);
  RECT wa = mi.rcWork;
  int cx = wa.left + margin < wa.right - margin ? std::clamp((int)pt.x, (int)wa.left + margin, (int)wa.right - margin) : (int)pt.x;
  int bottom = wa.bottom - margin - (int)(24 * s);
  int cy = wa.top + margin < bottom ? std::clamp((int)pt.y, (int)wa.top + margin, bottom) : (int)pt.y;
  center_ = {cx, cy};
  win_ = {cx - half, cy - half, cx + half, cy + half};

  if (!surf_.Ensure(2 * half, 2 * half)) {
    util::Log(L"wheel: graphics unavailable");
    return false;
  }
  surf_.Clear();
  SetWindowPos(hwnd_, HWND_TOPMOST, win_.left, win_.top, 2 * half, 2 * half, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Copy what is behind for the frosted glass on a worker thread, leaving this window out of it.
  SetWindowDisplayAffinity(hwnd_, kExcludeFromCapture);
  grab_ = GrabScreenAsync(win_, 2);
  return true;
}

// Turns a finished screen copy into the blurred glass and lets it fade in.
void Wheel::TakeGrab(bool fade) {
  if (!grab_ || !grab_->done) return;
  blur_ = BlurGrab(*grab_, dpi_, 22.f, 1.7f);
  grab_.reset();
  SetWindowDisplayAffinity(hwnd_, WDA_NONE);
  MakeBlurBrush();
  if (fade) glassA_.To(1);
  else glassA_.Snap(1);
  glassMs_ = fade ? perf::NowMs() - openedAt_ : 0;
}

void Wheel::MakeBlurBrush() {
  blurBrush_.Reset();
  if (!blur_) return;
  g_gfx.dc->CreateBitmapBrush(blur_.Get(),
                              D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                           D2D1_INTERPOLATION_MODE_LINEAR),
                              D2D1::BrushProperties(), &blurBrush_);
}

void Wheel::Open(Mode mode) {
  theme_ = LoadTheme();
  solid_ = g_settings.solid;
  glassA_ = Spring(blur_ ? 1.f : 0.f, 300, 34);
  openedAt_ = perf::NowMs();
  firstFrameMs_ = glassMs_ = -1;
  frames_.v.clear();
  if (!test_) TakeGrab(false);
  BuildStatic();
  alpha_ = Spring(0, 420, 40);
  alpha_.To(1);
  scale_ = Spring(.55f, 520, 24);
  scale_.To(1);
  subA_ = Spring(0, 700, 48);
  notchA_ = Spring(0, 520, 36);
  notchO_ = Spring(0, 600, 44);
  swap_ = Spring(1, 500, 44);
  commit_ = hubOff_ = touched_ = false;
  hubKey_.clear();
  state_ = State::Open;
  BuildSegs(mode);
  UpdateHub();
  BuildLegend();
  util::Log(L"wheel open: mode %d, %zu file(s), first %ls", (int)mode, files_.size(),
            files_.empty() ? L"-" : files_[0].c_str());
  RequestAnimation();
}

void Wheel::Close(bool commit) {
  state_ = State::Closing;
  commit_ = commit;
  if (kb_) SetKeyCapture(false);
  alpha_.k = commit ? 380.f : 900.f;
  alpha_.c = commit ? 40.f : 60.f;
  alpha_.To(0);
  scale_.To(commit ? 1.03f : .86f);
  for (size_t k = 0; k < segs_.size(); ++k) {
    auto& s = segs_[k];
    s.delay = 0;
    if (commit && (int)k == sel_) {
      s.h.To(sub_ < 0 ? 1.7f : .8f);
      s.a.To(1);
    } else {
      s.a.k = 900;
      s.a.c = 60;
      s.a.To(0);
    }
  }
  for (size_t j = 0; j < subs_.size(); ++j)
    if (commit && (int)j == sub_) subs_[j].h.To(1.7f);
  if (!commit) subA_.To(0);
  notchO_.To(0);
  hubOff_ = commit;
  RequestAnimation();
}

void Wheel::Hide() {
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  KillTimer(hwnd_, kPollTimer);
  if (kb_) SetKeyCapture(false);
  kb_ = false;
  grab_.reset();
  SetWindowDisplayAffinity(hwnd_, WDA_NONE);
  if (!frames_.v.empty())
    util::Log(L"wheel session: first frame %.1f ms after open, glass %ls, %ls", firstFrameMs_,
              glassMs_ < 0 ? L"none" : glassMs_ == 0 ? L"ready at open" : (std::to_wstring((int)glassMs_) + L" ms").c_str(),
              frames_.Line(L"render").c_str());
  frames_.v.clear();
  state_ = State::Hidden;
  blur_.Reset();
  blurBrush_.Reset();
  segs_.clear();
  subs_.clear();
  files_.clear();
}

void Wheel::Commit() {
  WheelCommit c;
  c.mode = mode_;
  c.item = items_[sel_];
  if (!c.item.subs.empty()) {
    c.sub = c.item.subs[sub_ >= 0 ? sub_ : c.item.def];
    c.hasSub = true;
  }
  c.files = files_;
  c.center = center_;
  c.dpi = dpi_;
  c.hubDiameter = G.rh * 2;
  Close(true);
  OnWheelCommit(c);
}

void Wheel::OnPollTimer() {
  if (kb_) {
    // Opened from the keyboard: close when the user moves on to something else.
    if (state_ != State::Open || commit_) return;
    POINT pt;
    GetCursorPos(&pt);
    if (GetForegroundWindow() != kbOwner_ || (LmbDown() && WindowFromPoint(pt) != hwnd_)) Close(false);
    return;
  }
  if (LmbDown()) {
    lmbUpSince_ = 0;
    return;
  }
  // Give OLE time to deliver Drop before deciding the drag ended somewhere else.
  ULONGLONG now = GetTickCount64();
  if (!lmbUpSince_) lmbUpSince_ = now;
  if (now - lmbUpSince_ < 400) return;
  if (state_ == State::Armed) Hide();
  else if (state_ == State::Open && !commit_) Close(false);
}

DWORD Wheel::OnDragEnter(IDataObject* data, POINTL pt, DWORD allowed) {
  if (state_ == State::Closing && !commit_ && !files_.empty()) {
    // Came back while fading out: bring the wheel back.
    state_ = State::Open;
    alpha_.k = 420;
    alpha_.c = 40;
    alpha_.To(1);
    scale_.To(1);
    for (auto& s : segs_) {
      s.a.k = 520;
      s.a.c = 28;
      s.a.To(1);
    }
    RequestAnimation();
    return OnDragOver(pt, allowed);
  }
  if (state_ != State::Armed) return DROPEFFECT_NONE;
  files_ = FilesFrom(data);
  if (files_.empty()) {
    Hide();
    return DROPEFFECT_NONE;
  }
  Open(pendingMode_);
  return DROPEFFECT_NONE;
}

DWORD Wheel::OnDragOver(POINTL pt, DWORD allowed) {
  if (state_ != State::Open || commit_) return DROPEFFECT_NONE;
  Hit h = HitTest({pt.x, pt.y});
  SetSel(h.zone == Hit::Segment || h.zone == Hit::Sub ? h.i : -1, h.zone == Hit::Sub ? h.j : -1);
  RequestAnimation();
  return h.zone == Hit::Segment || h.zone == Hit::Sub ? Accept(allowed) : DROPEFFECT_NONE;
}

void Wheel::OnDragLeave() {
  if (state_ == State::Open && !commit_) Close(false);
  else if (state_ == State::Armed) Hide();
}

DWORD Wheel::OnDrop(POINTL pt, DWORD allowed) {
  if (state_ != State::Open) {
    if (state_ == State::Armed) Hide();
    return DROPEFFECT_NONE;
  }
  Hit h = HitTest({pt.x, pt.y});
  if (h.zone == Hit::Segment || h.zone == Hit::Sub) {
    SetSel(h.i, h.zone == Hit::Sub ? h.j : -1);
    Commit();
    return Accept(allowed);
  }
  Close(false);
  return DROPEFFECT_NONE;
}

// ---------------------------------------------------------------- model

D2D1_POINT_2F Wheel::CenterDip() const {
  float s = 96.f / dpi_;
  return D2D1::Point2F((center_.x - win_.left) * s, (center_.y - win_.top) * s);
}

Wheel::Hit Wheel::HitTest(POINT screen) const {
  float s = 96.f / dpi_;
  return HitLocal((screen.x - center_.x) * s, (screen.y - center_.y) * s);
}

Wheel::Hit Wheel::HitLocal(float x, float y) const {
  Hit h;
  float d = std::hypot(x, y);
  int n = (int)items_.size();
  if (d < G.ri - 4) { h.zone = Hit::Center; return h; }
  if (!n) { h.zone = d < G.ro + 40 ? Hit::Center : Hit::None; return h; }
  float ang = std::atan2(y, x), step = kTau / n;
  int i = Mod((int)std::lround((ang + kPi / 2) / step), n);
  if (sel_ >= 0 && !items_[sel_].subs.empty() && d > G.ro + 2) {
    float span = items_[sel_].subs.size() * G.subStep;
    if (std::fabs(AngDiff(ang, SegAngle(sel_, n))) <= span / 2 + .14f) i = sel_;
  }
  const Item& it = items_[i];
  if (!it.subs.empty() && d > G.ro + 2) {
    if (d > G.r2o + 56) return h;
    int m = (int)it.subs.size();
    float off = AngDiff(ang, SegAngle(i, n));
    h.zone = Hit::Sub;
    h.i = i;
    h.j = std::clamp((int)std::lround(off / G.subStep + (m - 1) / 2.f), 0, m - 1);
    return h;
  }
  if (d > G.ro + 64) return h;
  h.zone = Hit::Segment;
  h.i = i;
  return h;
}

void Wheel::BuildStatic() {
  // The shapes never change, so they are made once per graphics device; only colours follow the theme.
  if (!ring_ || shapesGen_ != g_gfx.generation) {
    ring_ = RingGeometry(G.ri, G.ro);
    shadowRing_ = RingGeometry(G.ro - 1, G.ro + 70);
    hubShadowRing_ = RingGeometry(G.rh - 1, G.rh + 30);
    notch_ = ArcGeometry((G.rh + G.ri) / 2, -.2f, .2f);
    shapesGen_ = g_gfx.generation;
  }
  ringShadow_ = ShadowBrush(theme_.shadow, theme_.shadowOpacity, G.ro, 46, 18);
  hubShadow_ = ShadowBrush(theme_.shadow, theme_.shadowOpacity * .6f, G.rh, 20, 7);
  ringHi_ = HighlightBrush(theme_.glassHi, G.ro);
  hubHi_ = HighlightBrush(theme_.glassHi, G.rh);
  MakeBlurBrush();
}

void Wheel::BuildSegs(Mode mode) {
  mode_ = mode;
  items_ = ItemsFor(mode, files_);
  sel_ = sub_ = subParent_ = -1;
  pendingSub_ = INT_MIN;
  subs_.clear();
  subA_.Snap(0);
  segs_.clear();
  int n = (int)items_.size();
  float step = kTau / std::max(n, 1);
  auto* labelFmt = g_gfx.Format(15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  auto* smallFmt = g_gfx.Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (int i = 0; i < n; ++i) {
    const Item& it = items_[i];
    float th = SegAngle(i, n);
    Seg s;
    s.geo = CachedSector(n, i, -1, -1, [&] { return RoundedSector(G.ri, G.ro, th - step / 2, th + step / 2, 2, 3); });
    s.mx = G.rm * std::cos(th);
    s.my = G.rm * std::sin(th);
    s.ux = std::cos(th);
    s.uy = std::sin(th);
    s.icon = g_gfx.Icon(it.icon);
    s.label = g_gfx.Layout(it.label, s.icon ? smallFmt : labelFmt, 120);
    DWRITE_TEXT_METRICS m{};
    if (s.label) s.label->GetMetrics(&m);
    s.labelW = 120;
    s.labelH = m.height;
    s.a = Spring(0, 520, 28);
    s.h = Spring(0, 700, 40);
    s.delay = ReducedMotion() ? 0.f : .025f + i * .022f;
    segs_.push_back(std::move(s));
  }
}

void Wheel::SwitchMode(Mode mode) {
  if (mode == mode_) return;
  BuildSegs(mode);
  notchO_.To(0);
  if (!ReducedMotion()) scale_.vel += 1.1f;
  touched_ = false;
  UpdateHub();
  RequestAnimation();
}

void Wheel::RequestSub(int i) {
  if (subParent_ == i) {
    pendingSub_ = INT_MIN;
    if (i >= 0) subA_.To(1);
    return;
  }
  if (!subs_.empty() && subA_.v > .06f) {
    pendingSub_ = i;
    subA_.To(0);
  } else {
    BuildSub(i);
  }
}

void Wheel::BuildSub(int i) {
  subs_.clear();
  subParent_ = i;
  pendingSub_ = INT_MIN;
  if (i < 0 || i >= (int)items_.size() || items_[i].subs.empty()) {
    subA_.Snap(0);
    return;
  }
  const Item& it = items_[i];
  float th = SegAngle(i, (int)items_.size()), st = G.subStep, rm = (G.r2i + G.r2o) / 2;
  int m = (int)it.subs.size();
  auto* fmt = g_gfx.Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (int j = 0; j < m; ++j) {
    float c = th + (j - (m - 1) / 2.f) * st;
    Seg s;
    s.geo = CachedSector((int)items_.size(), i, m, j, [&] { return RoundedSector(G.r2i, G.r2o, c - st / 2, c + st / 2, 2, 3); });
    s.mx = rm * std::cos(c);
    s.my = rm * std::sin(c);
    s.ux = std::cos(c);
    s.uy = std::sin(c);
    s.label = g_gfx.Layout(it.subs[j].label, fmt, 90);
    DWRITE_TEXT_METRICS mt{};
    if (s.label) s.label->GetMetrics(&mt);
    s.labelW = 90;
    s.labelH = mt.height;
    s.h = Spring(0, 700, 40);
    subs_.push_back(std::move(s));
  }
  subTheta_ = th;
  subA_.Snap(0);
  subA_.To(1);
}

void Wheel::SetSel(int i, int j, bool force) {
  if (!force && i == sel_ && j == sub_) return;
  bool parentChanged = i != sel_ || force;
  sel_ = i;
  sub_ = j;
  if (i >= 0) touched_ = true;
  for (size_t k = 0; k < segs_.size(); ++k) segs_[k].h.To((int)k == i ? (j >= 0 ? .8f : 1.f) : 0.f);
  if (parentChanged) RequestSub(i >= 0 && !items_[i].subs.empty() ? i : -1);
  if (i >= 0) {
    const Item& it = items_[i];
    float a = SegAngle(i, (int)items_.size());
    if (j >= 0) a += (j - (it.subs.size() - 1) / 2.f) * G.subStep;
    float deg = a * 180 / kPi, cur = notchA_.v;
    while (deg - cur > 180) deg -= 360;
    while (deg - cur < -180) deg += 360;
    if (notchO_.v < .05f) notchA_.Snap(deg);
    else notchA_.To(deg);
    notchO_.To(1);
  } else {
    notchO_.To(0);
  }
  UpdateHub();
}

void Wheel::UpdateHub() {
  std::wstring h1, h2, h3, h4;
  bool acc = false;
  if (items_.empty()) {
    HubText t = EmptyText(mode_, files_);
    h1 = ModeTitle(mode_);
    h2 = t.h2;
    h3 = t.h3;
    h4 = t.h4;
  } else if (sel_ < 0) {
    h1 = files_.size() > 1 ? std::to_wstring(files_.size()) + L" mục" : util::FileNameOf(files_[0]);
    h2 = ModeTitle(mode_);
    h3 = kb_ ? L"Dùng ← → để chọn" : L"Kéo vào một ô";
  } else {
    const Item& it = items_[sel_];
    const SubItem* sub = it.subs.empty() ? nullptr : &it.subs[sub_ >= 0 ? sub_ : it.def];
    bool defaultSub = !it.subs.empty() && sub_ < 0;
    HubText t = Describe(mode_, files_, it, sub, defaultSub);
    h1 = ModeTitle(mode_);
    acc = true;
    h2 = t.h2;
    h3 = defaultSub && kb_ ? L"Bấm ↑ để chọn mức" : t.h3;
    h4 = t.h4;
  }
  std::wstring key = std::to_wstring((int)mode_) + L"|" + h1 + L"|" + h2 + L"|" + h3 + L"|" + h4;
  if (key == hubKey_) return;
  hubKey_ = key;
  float w = G.rh * 2 - 20;
  h1Accent_ = acc;
  h1_ = g_gfx.Layout(util::Upper(h1), g_gfx.Format(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, true), w);
  ComPtr<IDWriteTextLayout1> l1;
  if (h1_ && SUCCEEDED(h1_.As(&l1))) l1->SetCharacterSpacing(0, .76f, 0, DWRITE_TEXT_RANGE{0, (UINT32)h1.size()});
  h2_ = g_gfx.Layout(h2, g_gfx.Format(h2.size() > 8 ? 13.5f : 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), w);
  h3_ = h3.empty() ? nullptr : g_gfx.Layout(h3, g_gfx.Format(10.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true), w, 30);
  h4_ = h4.empty() ? nullptr : g_gfx.Layout(h4, g_gfx.Format(11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true), w + 8);
  hubTotal_ = 0;
  for (auto* l : {h1_.Get(), h2_.Get(), h3_.Get(), h4_.Get()}) {
    if (!l) continue;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    hubTotal_ += std::min(m.height, l == h3_.Get() ? 30.f : 100.f) + 1;
  }
  swap_.Snap(0);
  swap_.To(1);
}

void Wheel::BuildLegend() {
  auto* keyFmt = g_gfx.Format(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  auto* labelFmt = g_gfx.Format(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, false, DWRITE_TEXT_ALIGNMENT_LEADING);
  kbHint_ = kb_ ? g_gfx.Layout(L"← → chọn  ·  ↑ chọn mức  ·  Tab đổi vòng  ·  Enter chạy  ·  Esc đóng",
                               g_gfx.Format(11.f, DWRITE_FONT_WEIGHT_NORMAL), 320)
                : nullptr;
  pillsW_ = 0;
  for (int i = 0; i < 3; ++i) {
    Pill& p = pills_[i];
    DWRITE_TEXT_METRICS m{};
    p.key = g_gfx.Layout(KeyForMode((Mode)i), keyFmt, 80);
    p.key->GetMetrics(&m);
    p.keyW = m.width + 12;
    p.key->SetMaxWidth(p.keyW);
    p.label = g_gfx.Layout(ModeTitle((Mode)i), labelFmt, 200);
    p.label->GetMetrics(&m);
    p.labelW = m.width;
    p.w = 4 + p.keyW + 6 + p.labelW + 10;
    pillsW_ += p.w + (i ? 6 : 0);
  }
}

// ---------------------------------------------------------------- background glass

bool Wheel::CaptureBackground() {
  blur_ = CaptureBlurred(win_, dpi_, 22.f, 1.7f, 2);
  return blur_ != nullptr;
}

bool Wheel::MakeBlur(ID2D1Bitmap1* source) {
  blur_ = BlurImage(source, dpi_, 22.f, 1.7f);
  return blur_ != nullptr;
}

// ---------------------------------------------------------------- frame

bool Wheel::Tick(float dt) {
  if (state_ != State::Open && state_ != State::Closing) return false;
  if (state_ == State::Open && !commit_ && !test_ && !kb_) {
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) == hwnd_) {
      Hit h = HitTest(pt);
      SetSel(h.zone == Hit::Segment || h.zone == Hit::Sub ? h.i : -1, h.zone == Hit::Sub ? h.j : -1);
    }
  }
  bool moving = false;
  if (grab_) {
    TakeGrab(true);
    moving = true;
  }
  auto step = [&](Spring& s) {
    s.Step(dt);
    moving |= s.Moving();
  };
  step(glassA_);
  step(alpha_);
  step(scale_);
  step(subA_);
  step(notchA_);
  step(notchO_);
  step(swap_);
  for (auto& s : segs_) {
    if (s.delay > 0) {
      s.delay -= dt;
      moving = true;
      if (s.delay <= 0 && state_ == State::Open) s.a.To(1);
    }
    step(s.a);
    step(s.h);
  }
  if (pendingSub_ != INT_MIN && subA_.v < .06f) {
    BuildSub(pendingSub_);
    moving = true;
  }
  const Item* parent = subParent_ >= 0 && subParent_ < (int)items_.size() ? &items_[subParent_] : nullptr;
  for (size_t j = 0; j < subs_.size(); ++j) {
    auto& s = subs_[j];
    if (!(commit_ && (int)j == sub_))
      s.h.To(sub_ == (int)j ? 1.f : (sub_ < 0 && parent && (int)j == parent->def ? .3f : 0.f));
    step(s.h);
  }
  if (state_ == State::Closing && alpha_.t == 0 && alpha_.v < .02f) {
    if (!test_) Hide();
    return false;
  }
  return moving;
}

void Wheel::Render() {
  if (state_ != State::Open && state_ != State::Closing) return;
  double t0 = perf::NowMs();
  auto* dc = surf_.BeginDraw(dpi_);
  if (!dc) return;
  Draw(dc, surf_.Base());
  surf_.EndDraw();
  double t1 = perf::NowMs();
  frames_.Add(t1 - t0);
  if (firstFrameMs_ < 0) firstFrameMs_ = t1 - openedAt_;
}

void Wheel::Draw(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
  const Theme& t = theme_;
  float A = Clamp01(alpha_.v), s = scale_.v, sa = Clamp01(subA_.v);
  D2D1_POINT_2F c = CenterDip();
  D2D1_MATRIX_3X2_F L = D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(c.x, c.y);
  D2D1_MATRIX_3X2_F W = Mul(L, base);
  D2D1_MATRIX_3X2_F inv = D2D1::Matrix3x2F::Scale(1 / s, 1 / s) * D2D1::Matrix3x2F::Translation(-c.x / s, -c.y / s);
  auto* round = g_gfx.round.Get();
  dc->SetTransform(W);

  // Ring: shadow, frosted glass, tint, sheen, edges.
  ringShadow_->SetOpacity(A);
  dc->FillGeometry(shadowRing_.Get(), ringShadow_.Get());
  float glass = A * std::clamp(glassA_.v, 0.f, 1.f);
  if (!solid_ && blurBrush_) {
    blurBrush_->SetTransform(inv);
    blurBrush_->SetOpacity(glass);
    dc->FillGeometry(ring_.Get(), blurBrush_.Get());
  }
  dc->FillGeometry(ring_.Get(), g_gfx.Brush(solid_ ? t.solid : t.glass, A));
  ringHi_->SetOpacity(A);
  dc->FillGeometry(ring_.Get(), ringHi_.Get());
  dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), G.ro - .5f, G.ro - .5f), g_gfx.Brush(t.glassLine, A), 1);
  dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), G.ri + .5f, G.ri + .5f), g_gfx.Brush(t.glassEdge, A), 1);

  auto drawSeg = [&](Seg& sg, float a, float h, float push, float sc, const D2D1_COLOR_F& bg, float bgOpacity) {
    float hc = Clamp01(h);
    D2D1_MATRIX_3X2_F T = D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(sg.mx, sg.my)) *
                          D2D1::Matrix3x2F::Translation(sg.ux * push, sg.uy * push);
    T = Mul(T, W);
    dc->SetTransform(T);
    dc->FillGeometry(sg.geo.Get(), g_gfx.Brush(bg, bgOpacity * a * A));
    if (hc > .003f) {
      dc->DrawGeometry(sg.geo.Get(), g_gfx.Brush(t.accent, .09f * hc * a * A), 16, round);
      dc->DrawGeometry(sg.geo.Get(), g_gfx.Brush(t.accent, .15f * hc * a * A), 8, round);
      dc->FillGeometry(sg.geo.Get(), g_gfx.Brush(t.accent, hc * a * A));
    }
    D2D1_COLOR_F col = Mix(t.fg, t.onAccent, hc);
    float ty = sg.my - sg.labelH / 2;
    if (sg.icon) {
      dc->SetTransform(Mul(D2D1::Matrix3x2F::Scale(22.f / 24, 22.f / 24) * D2D1::Matrix3x2F::Translation(sg.mx - 11, sg.my - 21), T));
      dc->DrawGeometry(sg.icon, g_gfx.Brush(col, a * A), 1.8f, round);
      dc->SetTransform(T);
      ty = sg.my + 11 - sg.labelH / 2;
    }
    if (sg.label) dc->DrawTextLayout(D2D1::Point2F(sg.mx - sg.labelW / 2, ty), sg.label.Get(), g_gfx.Brush(col, a * A));
  };

  // Outer ring of levels for the hovered tool.
  if (!subs_.empty() && sa > .003f) {
    for (auto& sg : subs_) {
      float h = sg.h.v, push = h * 4 - (1 - sa) * 14, sc = .9f + .1f * sa + h * .04f;
      D2D1_MATRIX_3X2_F T = Mul(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(sg.mx, sg.my)) *
                                    D2D1::Matrix3x2F::Translation(sg.ux * push, sg.uy * push + 4),
                                W);
      dc->SetTransform(T);
      dc->FillGeometry(sg.geo.Get(), g_gfx.Brush(t.shadow, t.shadowOpacity * .45f * sa * A));
      drawSeg(sg, sa, h, push, sc, t.sub, .96f);
    }
  }
  for (auto& sg : segs_) {
    float a = Clamp01(sg.a.v);
    if (a <= .003f) continue;
    float h = sg.h.v;
    drawSeg(sg, a, h, h * 6 - (1 - a) * 18, .86f + .14f * a + h * .035f, t.seg, t.segOpacity);
  }

  // Notch pointing at the selection.
  if (notchO_.v > .003f) {
    dc->SetTransform(Mul(D2D1::Matrix3x2F::Rotation(notchA_.v), W));
    dc->DrawGeometry(notch_.Get(), g_gfx.Brush(t.accent, Clamp01(notchO_.v) * A), 3, round);
  }

  // Hub.
  dc->SetTransform(W);
  if (!hubOff_) {
    auto hub = D2D1::Ellipse(D2D1::Point2F(0, 0), G.rh, G.rh);
    hubShadow_->SetOpacity(A);
    dc->FillGeometry(hubShadowRing_.Get(), hubShadow_.Get());
    if (!solid_ && blurBrush_) {
      blurBrush_->SetOpacity(glass);
      dc->FillEllipse(hub, blurBrush_.Get());
    }
    dc->FillEllipse(hub, g_gfx.Brush(solid_ ? t.solidHub : t.glassHub, A));
    hubHi_->SetOpacity(A);
    dc->FillEllipse(hub, hubHi_.Get());
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), G.rh - .5f, G.rh - .5f), g_gfx.Brush(t.glassLine, A), 1);

    float sw = Clamp01(swap_.v), ta = A * (.25f + .75f * sw);
    float y = -hubTotal_ / 2 + (1 - sw) * 3, x = -(G.rh - 10);
    auto line = [&](IDWriteTextLayout* l, const D2D1_COLOR_F& col, float clip) {
      if (!l) return;
      DWRITE_TEXT_METRICS m{};
      l->GetMetrics(&m);
      dc->DrawTextLayout(D2D1::Point2F(x, y), l, g_gfx.Brush(col, ta), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      y += std::min(m.height, clip) + 1;
    };
    line(h1_.Get(), h1Accent_ ? t.accent : t.fg2, 100);
    line(h2_.Get(), t.fg, 100);
    line(h3_.Get(), t.fg2, 30);
    if (h4_) {
      x -= 4;
      line(h4_.Get(), t.fg, 100);
    }
  }

  // Legend under the wheel: which key opens which wheel.
  bool lowSub = !subs_.empty() && std::sin(subTheta_) > .35f;
  float la = A * (1 - sa * (lowSub ? 1 : 0));
  if (la > .003f) {
    float x = -pillsW_ / 2, y = G.ro + 14;
    for (int i = 0; i < 3; ++i) {
      Pill& p = pills_[i];
      bool on = (int)mode_ == i;
      auto pill = D2D1::RoundedRect(D2D1::RectF(x, y, x + p.w, y + 28), 14, 14);
      dc->FillRoundedRectangle(pill, g_gfx.Brush(solid_ ? t.solidHub : t.glassHub, la));
      dc->DrawRoundedRectangle(pill, g_gfx.Brush(t.glassLine, la), 1);
      auto key = D2D1::RoundedRect(D2D1::RectF(x + 4, y + 4, x + 4 + p.keyW, y + 24), 10, 10);
      if (on) {
        dc->FillRoundedRectangle(key, g_gfx.Brush(t.accent, la));
      } else {
        dc->FillRoundedRectangle(key, g_gfx.Brush(t.key, la));
        dc->DrawRoundedRectangle(key, g_gfx.Brush(t.keyLine, la), 1);
      }
      DWRITE_TEXT_METRICS km{}, lm{};
      p.key->GetMetrics(&km);
      p.label->GetMetrics(&lm);
      dc->DrawTextLayout(D2D1::Point2F(x + 4, y + 14 - km.height / 2), p.key.Get(),
                         g_gfx.Brush(on ? t.onAccent : t.fg, la));
      dc->DrawTextLayout(D2D1::Point2F(x + 4 + p.keyW + 6, y + 14 - lm.height / 2), p.label.Get(),
                         g_gfx.Brush(on ? t.fg : t.fg2, la));
      x += p.w + 6;
    }
    if (kb_ && kbHint_) {
      DWRITE_TEXT_METRICS m{};
      kbHint_->GetMetrics(&m);
      float w = m.width + 20, hy = y + 34;
      auto r = D2D1::RoundedRect(D2D1::RectF(-w / 2, hy, w / 2, hy + 22), 11, 11);
      dc->FillRoundedRectangle(r, g_gfx.Brush(solid_ ? t.solidHub : t.glassHub, la));
      dc->DrawRoundedRectangle(r, g_gfx.Brush(t.glassLine, la), 1);
      dc->DrawTextLayout(D2D1::Point2F(-160, hy + 11 - m.height / 2), kbHint_.Get(), g_gfx.Brush(t.fg2, la));
    }
  }
  dc->SetTransform(base);
}

// ---------------------------------------------------------------- test rendering

std::wstring Wheel::Bench(const std::vector<std::wstring>& files, float dpi) {
  if (!g_gfx.Ensure()) return L"no graphics";
  auto* dc = g_gfx.dc.Get();
  test_ = true;
  dpi_ = dpi;
  files_ = files;
  LONG px = (LONG)std::ceil(G.sz * dpi / 96);
  POINT c{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
  center_ = c;
  win_ = {c.x - px / 2, c.y - px / 2, c.x - px / 2 + px, c.y - px / 2 + px};
  std::wstring out;

  // Opening: screen capture + blur, then building the segments.
  perf::Stats copy, glass, open;
  for (int i = 0; i < 12; ++i) {
    double t0 = perf::NowMs();
    grab_ = GrabScreenAsync(win_, 2);
    while (!grab_->done) Sleep(0);
    double t1 = perf::NowMs();
    TakeGrab(false);
    perf::WaitGpu();
    double t2 = perf::NowMs();
    Open(Mode::Tools);
    double t3 = perf::NowMs();
    if (i) { copy.Add(t1 - t0); glass.Add(t2 - t1); open.Add(t3 - t2); }  // the first run warms caches
  }
  out += copy.Line(L"wheel screen copy (worker thread)") + L"\n" + glass.Line(L"wheel glass blur (UI thread)") + L"\n" +
         open.Line(L"wheel build (UI thread)") + L"\n";
  g_gfx.Idle();
  out += L"memory after wheel opens: " + perf::Memory() + L"\n";

  // Frames: opening animation, then hovering from item to item, into the outer ring and back.
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
  ComPtr<ID2D1Bitmap1> target;
  dc->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, &props, &target);
  perf::Stats cpu, total;
  Open(Mode::Tools);
  int n = (int)items_.size();
  for (int f = 0; f < 360; ++f) {
    if (f >= 40 && f % 25 == 0 && n) {
      int i = (f / 25) % n;
      bool outer = !items_[i].subs.empty() && (f / 25) % 2;
      SetSel(i, outer ? 1 : -1);
    }
    Tick(1 / 60.f);
    double t0 = perf::NowMs();
    dc->SetTarget(target.Get());
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    Draw(dc, D2D1::Matrix3x2F::Translation(0, 0));
    dc->EndDraw();
    dc->SetTarget(nullptr);
    double t1 = perf::NowMs();
    perf::WaitGpu();
    double t2 = perf::NowMs();
    cpu.Add(t1 - t0);
    total.Add(t2 - t0);
  }
  out += cpu.Line(L"wheel frame CPU") + L"\n" + total.Line(L"wheel frame CPU+GPU") + L"\n";
  target.Reset();
  g_gfx.Idle();
  out += L"memory after wheel frames: " + perf::Memory() + L"\n";
  state_ = State::Hidden;
  test_ = false;
  ReleaseGraphics();
  return out;
}

bool Wheel::RenderTest(const std::wstring& out, Mode mode, const std::vector<std::wstring>& files,
                       const std::wstring& selectId, int sub, float dpi, int theme) {
  if (!g_gfx.Ensure()) return false;
  auto* dc = g_gfx.dc.Get();
  test_ = true;
  dpi_ = dpi;
  files_ = files;
  UINT px = (UINT)std::ceil(G.sz * dpi / 96);
  center_ = {(LONG)px / 2, (LONG)px / 2};
  win_ = {0, 0, (LONG)px, (LONG)px};
  Theme th = LoadTheme(theme);
  float dipW = px * 96.f / dpi;

  // A stand-in desktop: wallpaper blobs and part of a light window, so the glass has something to blur.
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
  ComPtr<ID2D1Bitmap1> bg, target;
  dc->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, &props, &bg);
  dc->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, &props, &target);
  dc->SetTarget(bg.Get());
  dc->SetDpi(dpi, dpi);
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->Clear(th.dark ? D2D1::ColorF(0x080c18) : D2D1::ColorF(0xe6ecf6));
  struct Blob { float x, y, r; UINT32 light, dark; } blobs[] = {
      {.15f, .2f, .55f, 0x9cc2ff, 0x1c3d97}, {.9f, .15f, .5f, 0xcbb9ff, 0x0d566a},
      {.75f, .95f, .55f, 0xffc8ab, 0x6a2a7c}, {.2f, .95f, .45f, 0xa6e3d6, 0x7a2d4d}};
  for (auto& b : blobs) {
    D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(th.dark ? b.dark : b.light, 1)},
                                  {1, D2D1::ColorF(th.dark ? b.dark : b.light, 0)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    dc->CreateGradientStopCollection(stops, 2, &coll);
    ComPtr<ID2D1RadialGradientBrush> rb;
    float r = b.r * dipW;
    dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(b.x * dipW, b.y * dipW),
                                                                      D2D1::Point2F(0, 0), r, r),
                                  coll.Get(), &rb);
    dc->FillRectangle(D2D1::RectF(0, 0, dipW, dipW), rb.Get());
  }
  auto winRect = D2D1::RoundedRect(D2D1::RectF(dipW * .42f, dipW * .08f, dipW * 1.2f, dipW * .78f), 10, 10);
  dc->FillRoundedRectangle(winRect, g_gfx.Brush(th.dark ? D2D1::ColorF(0x202124) : D2D1::ColorF(0xf9fafb)));
  UINT32 tiles[] = {0x5aa9ff, 0xd13438, 0x2f6fd8, 0x1f8a4c, 0xd4602b, 0x7c4ddb};
  for (int i = 0; i < 6; ++i) {
    float x = dipW * .48f + (i % 3) * 78, y = dipW * .2f + (i / 3) * 96;
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + 48, y + 58), 4, 4), g_gfx.Brush(D2D1::ColorF(tiles[i])));
  }
  dc->EndDraw();
  dc->SetTarget(nullptr);
  if (!MakeBlur(bg.Get())) return false;

  Open(mode);
  theme_ = th;
  BuildStatic();
  hubKey_.clear();
  UpdateHub();
  for (int f = 0; f < 30; ++f) Tick(1 / 60.f);
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i].id == selectId) SetSel((int)i, sub >= 0 && sub < (int)items_[i].subs.size() ? sub : -1);
  for (int f = 0; f < 150; ++f) Tick(1 / 60.f);

  dc->SetTarget(target.Get());
  dc->SetDpi(dpi, dpi);
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  dc->DrawBitmap(bg.Get());
  Draw(dc, D2D1::Matrix3x2F::Identity());
  HRESULT hr = dc->EndDraw();
  dc->SetTarget(nullptr);
  if (FAILED(hr)) { util::Log(L"test EndDraw failed 0x%08X", (unsigned)hr); return false; }
  return SavePng(target.Get(), out);
}

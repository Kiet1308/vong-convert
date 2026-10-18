#include "bubble.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "actions.h"

namespace {

constexpr float kW = 160, kH = 172;          // window size in DIPs
constexpr float kCx = 80, kCy = 70;          // disc center inside the window
constexpr float kDisc = 84;                  // resting diameter
const char* kCheck = "M5 12.5l4.5 4.5L19 7.5";
const char* kBang = "M12 6v8M12 18.5v.01";

std::vector<std::unique_ptr<Bubble>> g_bubbles;
bool g_registered = false;

ComPtr<ID2D1PathGeometry> Arc(float r, float fraction) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  g_gfx.factory->CreatePathGeometry(&path);
  path->Open(&sink);
  float a0 = -kPi / 2, a1 = a0 + std::clamp(fraction, 0.f, .9999f) * 2 * kPi;
  sink->BeginFigure(D2D1::Point2F(r * std::cos(a0), r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r * std::cos(a1), r * std::sin(a1)), D2D1::SizeF(r, r), 0,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                a1 - a0 > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  return path;
}

}  // namespace

Bubble::Bubble(POINT center, float dpi, float startDiameter, std::wstring label)
    : dpi_(dpi), label_(std::move(label)) {
  if (!g_registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"VongConvert.Bubble";
    RegisterClassExW(&wc);
    g_registered = true;
  }
  theme_ = LoadTheme();
  float s = dpi / 96.f;
  int w = (int)std::ceil(kW * s), h = (int)std::ceil(kH * s);
  int x = center.x - (int)std::lround(kCx * s), y = center.y - (int)std::lround(kCy * s);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                          L"VongConvert.Bubble", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, g_hinst, nullptr);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
  surf_.Attach(hwnd_);
  w_ = w;
  h_ = h;
  size_ = Spring(startDiameter, 520, 30);
  size_.To(kDisc);
  alpha_ = Spring(1, 600, 44);
  shown_ = Spring(0, 300, 30);
  badge_ = Spring(0, 520, 22);
  Render();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  RequestAnimation();
}

Bubble::~Bubble() {
  if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK Bubble::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = (Bubble*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP:
      if (self) self->OnClick();
      return 0;
    case WM_DPICHANGED: return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void Bubble::SetProgress(float p) {
  if (p < 0) indeterminate_ = true;
  else target_ = std::max(target_, p);
  RequestAnimation();
}

void Bubble::Finish(bool ok, std::vector<std::wstring> outputs, std::wstring pill) {
  ok_ = ok;
  outputs_ = std::move(outputs);
  pill_ = std::move(pill);
  state_ = State::Done;
  target_ = 1;
  hold_ = outputs_.empty() ? 1.1f : 2.6f;
  badge_.To(1);
  RequestAnimation();
}

void Bubble::OnClick() {
  if (state_ == State::Done && ok_ && !outputs_.empty()) {
    RevealInExplorer(outputs_);
    state_ = State::Fading;
    alpha_.To(0);
    size_.To(kDisc * 1.12f);
    RequestAnimation();
  }
}

void Bubble::FlyTo(POINT target) {
  if (state_ == State::Fading || state_ == State::Flying) return;
  RECT rc;
  GetWindowRect(hwnd_, &rc);
  origin_ = {rc.left, rc.top};
  float s = dpi_ / 96.f;
  flyX_ = Spring(0, 150, 21, .3f);
  flyY_ = Spring(0, 150, 21, .3f);
  flyX_.To(target.x - (rc.left + kCx * s));
  flyY_.To(target.y - (rc.top + kCy * s));
  size_.To(28);
  state_ = State::Flying;
  flown_ = true;
  RequestAnimation();
}

bool Bubble::Tick(float dt) {
  if (state_ == State::Flying) {
    // Glide into the new file in Explorer, then melt away on top of it.
    flyX_.Step(dt);
    flyY_.Step(dt);
    for (Spring* s : {&size_, &alpha_, &shown_, &badge_}) s->Step(dt);
    SetWindowPos(hwnd_, nullptr, origin_.x + (int)std::lround(flyX_.v), origin_.y + (int)std::lround(flyY_.v), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (std::hypot(flyX_.t - flyX_.v, flyY_.t - flyY_.v) < 8) {
      state_ = State::Fading;
      alpha_.k = 500;
      alpha_.c = 44;
      alpha_.To(0);
    }
    return true;
  }
  bool moving = false;
  // Jobs that cannot report progress still get a ring that creeps forward.
  if (state_ == State::Running && indeterminate_ && target_ < .897f) {
    target_ += (0.9f - target_) * std::min(1.f, dt * 1.6f);
    moving = true;
  }
  shown_.To(target_);
  for (Spring* s : {&size_, &alpha_, &shown_, &badge_}) {
    s->Step(dt);
    moving |= s->Moving();
  }
  if (state_ == State::Done) {
    hold_ -= dt;
    if (hold_ <= 0) {
      state_ = State::Fading;
      alpha_.To(0);
      size_.To(kDisc * 1.12f);
    }
    return true;
  }
  return moving;
}

bool Bubble::Dead() const { return state_ == State::Fading && alpha_.t == 0 && alpha_.v < .02f; }

void Bubble::Render() {
  if (!surf_.Ensure(w_, h_)) return;
  auto* dc = surf_.BeginDraw(dpi_);
  if (!dc) return;
  Draw(dc);
  surf_.EndDraw();
}

void Bubble::Draw(ID2D1DeviceContext* dc) {
  const Theme& t = theme_;
  float A = std::clamp(alpha_.v, 0.f, 1.f), d = size_.v, r = d / 2;
  D2D1_MATRIX_3X2_F W = D2D1::Matrix3x2F::Translation(kCx, kCy) * surf_.Base();
  dc->SetTransform(W);
  auto* round = g_gfx.round.Get();

  // Soft shadow, glass disc, sheen, edge.
  for (int i = 3; i >= 1; --i)
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 6), r + i * 4.f, r + i * 4.f),
                    g_gfx.Brush(t.shadow, t.shadowOpacity * .18f * A));
  auto disc = D2D1::Ellipse(D2D1::Point2F(0, 0), r, r);
  D2D1_COLOR_F fill = t.glassHub;
  fill.a = std::max(fill.a, .9f);
  dc->FillEllipse(disc, g_gfx.Brush(fill, A));
  dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), r - .5f, r - .5f), g_gfx.Brush(t.glassLine, A), 1);

  // Progress ring.
  float rr = r - 5;
  dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), rr, rr), g_gfx.Brush(t.glassEdge, A), 3);
  float p = std::clamp(shown_.v, 0.f, 1.f);
  D2D1_COLOR_F ringColor = state_ != State::Running && !ok_ ? t.danger : t.accent;
  if (p >= .999f) dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), rr, rr), g_gfx.Brush(ringColor, A), 3.6f);
  else if (p > .002f) dc->DrawGeometry(Arc(rr, p).Get(), g_gfx.Brush(ringColor, A), 3.6f, round);

  // Label in the middle.
  if (!labelLayout_) labelLayout_ = g_gfx.Layout(label_, g_gfx.Format(label_.size() > 7 ? 12.f : 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, true), kDisc - 22);
  if (labelLayout_) {
    DWRITE_TEXT_METRICS m{};
    labelLayout_->GetMetrics(&m);
    dc->DrawTextLayout(D2D1::Point2F(-(kDisc - 22) / 2, -m.height / 2), labelLayout_.Get(), g_gfx.Brush(t.fg, A));
  }

  // Check or error badge.
  float b = std::max(0.f, badge_.v);
  if (b > .01f) {
    D2D1_MATRIX_3X2_F B = D2D1::Matrix3x2F::Scale(b, b) * D2D1::Matrix3x2F::Translation(r * .68f, r * .68f) * W;
    dc->SetTransform(B);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), 14, 14), g_gfx.Brush(ok_ ? t.accent : t.danger, A));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(16.f / 24, 16.f / 24) * D2D1::Matrix3x2F::Translation(-8, -8) * B);
    dc->DrawGeometry(g_gfx.Icon(ok_ ? kCheck : kBang), g_gfx.Brush(t.onAccent, A), 2.6f, round);
    dc->SetTransform(W);
  }

  // Pill under the disc: percentage while running, then what to do next.
  std::wstring pill;
  if (state_ == State::Running) pill = indeterminate_ ? L"Đang xử lý" : std::to_wstring((int)std::lround(p * 100)) + L"%";
  else if (state_ != State::Flying && !flown_) pill = pill_;
  if (pill != pillShown_) {
    pillShown_ = pill;
    pillLayout_ = g_gfx.Layout(pill, g_gfx.Format(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), 150);
  }
  if (pillLayout_ && !pill.empty()) {
    DWRITE_TEXT_METRICS m{};
    pillLayout_->GetMetrics(&m);
    float w = m.width + 20, y = kDisc / 2 + 10;
    auto rect = D2D1::RoundedRect(D2D1::RectF(-w / 2, y, w / 2, y + 24), 12, 12);
    dc->FillRoundedRectangle(rect, g_gfx.Brush(fill, A));
    dc->DrawRoundedRectangle(rect, g_gfx.Brush(t.glassLine, A), 1);
    dc->DrawTextLayout(D2D1::Point2F(-75, y + 12 - m.height / 2), pillLayout_.Get(), g_gfx.Brush(t.fg, A));
  }
  dc->SetTransform(surf_.Base());
}

namespace bubbles {

Bubble* Spawn(POINT center, float dpi, float startDiameter, const std::wstring& label) {
  g_bubbles.push_back(std::make_unique<Bubble>(center, dpi, startDiameter, label));
  return g_bubbles.back().get();
}

Bubble* ForJob(int jobId) {
  for (auto& b : g_bubbles)
    if (b->jobId == jobId) return b.get();
  return nullptr;
}

bool TickAll(float dt) {
  bool any = false;
  for (size_t i = 0; i < g_bubbles.size();) {
    any |= g_bubbles[i]->Tick(dt);
    if (g_bubbles[i]->Dead()) g_bubbles.erase(g_bubbles.begin() + i);
    else ++i;
  }
  return any;
}

void RenderAll() {
  for (auto& b : g_bubbles) b->Render();
}

void Clear() { g_bubbles.clear(); }

size_t Count() { return g_bubbles.size(); }

}  // namespace bubbles

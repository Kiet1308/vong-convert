#include "pin.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "actions.h"
#include "gfx.h"
#include "spring.h"
#include "util.h"

namespace {

using M = D2D1::Matrix3x2F;
constexpr float kCardW = 280, kCardH = 68, kMargin = 16, kGap = 8;
const char* kClose = "M6 6l12 12M18 6L6 18";

class Card;
std::vector<std::unique_ptr<Card>> g_cards;
bool g_registered = false;

M Base(const D2D1_MATRIX_3X2_F& b) { return M(b._11, b._12, b._21, b._22, b._31, b._32); }

ComPtr<ID2D1Bitmap1> Thumbnail(const std::wstring& path, int px) {
  ComPtr<IShellItemImageFactory> factory;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
  HBITMAP hbm = nullptr;
  if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &hbm)) || !hbm) return nullptr;
  ComPtr<IWICBitmap> wb;
  ComPtr<IWICFormatConverter> conv;
  ComPtr<ID2D1Bitmap1> bmp;
  if (SUCCEEDED(g_gfx.wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUsePremultipliedAlpha, &wb)) &&
      SUCCEEDED(g_gfx.wic->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeMedianCut)))
    g_gfx.dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
  DeleteObject(hbm);
  return bmp;
}

class Card {
 public:
  Card(const std::vector<std::wstring>& files, POINT from) : files_(files) {
    if (!g_registered) {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.style = CS_DBLCLKS;
      wc.lpfnWndProc = WndProc;
      wc.hInstance = g_hinst;
      wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      wc.lpszClassName = L"VongConvert.Pin";
      RegisterClassExW(&wc);
      g_registered = true;
    }
    HMONITOR mon = MonitorFromPoint(from, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    work_ = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    dpi_ = (float)dx;
    theme_ = LoadTheme();
    float s = dpi_ / 96.f;
    w_ = (UINT)std::ceil((kCardW + kMargin * 2) * s);
    h_ = (UINT)std::ceil((kCardH + kMargin * 2) * s);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                            L"VongConvert.Pin", L"Ghim nổi", WS_POPUP, 0, 0, w_, h_, nullptr, nullptr, g_hinst, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    surf_.Attach(hwnd_);
    surf_.Ensure(w_, h_);

    thumb_ = Thumbnail(files_[0], (int)std::lround(88 * s));
    std::wstring name = files_.size() > 1 ? std::to_wstring(files_.size()) + L" mục" : util::FileNameOf(files_[0]);
    double bytes = 0;
    for (auto& f : files_) bytes += util::IsDirectory(f) ? 0 : (double)util::FileSize(f);
    std::wstring sub = L"Kéo ra để dùng";
    if (files_.size() > 1) sub = util::FileNameOf(files_[0]) + L" và " + std::to_wstring(files_.size() - 1) + L" mục khác";
    else if (bytes > 0) sub = util::FormatSize(bytes) + L" · Kéo ra để dùng";
    float textW = kCardW - 66 - 44;
    name_ = g_gfx.Layout(name, g_gfx.Format(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false, true, DWRITE_TEXT_ALIGNMENT_LEADING), textW);
    sub_ = g_gfx.Layout(sub, g_gfx.Format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, false, true, DWRITE_TEXT_ALIGNMENT_LEADING), textW);

    alpha_ = Spring(0, 420, 40);
    alpha_.To(1);
    slide_ = Spring(48, 360, 30);
    slide_.To(0);
    y_ = Spring(0, 420, 40, .05f);
    hover_ = Spring(0, 500, 44);
    closeHot_ = Spring(0, 600, 48);
  }

  ~Card() {
    if (hwnd_) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      DestroyWindow(hwnd_);
    }
  }

  void SetSlot(int slot, bool snap) {
    float target = slot * (kCardH + kGap);
    if (snap) y_.Snap(target);
    else y_.To(target);
    RequestAnimation();
  }

  bool Tick(float dt) {
    bool moving = false;
    for (Spring* s : {&alpha_, &slide_, &y_, &hover_, &closeHot_}) {
      s->Step(dt);
      moving |= s->Moving();
    }
    Place();
    // Redraw while anything moves, plus one frame once it settles.
    dirty_ |= moving || wasMoving_;
    wasMoving_ = moving;
    return moving;
  }

  bool Dead() const { return closing_ && alpha_.t == 0 && alpha_.v < .02f; }
  bool Closing() const { return closing_; }

  void Close() {
    if (closing_) return;
    closing_ = true;
    alpha_.To(0);
    slide_.To(40);
    RequestAnimation();
  }

  void Render() {
    if (!dirty_) return;
    dirty_ = false;
    if (!surf_.Ensure(w_, h_)) return;
    auto* dc = surf_.BeginDraw(dpi_);
    if (!dc) return;
    Draw(dc);
    surf_.EndDraw();
  }

  void Show() {
    Place();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (Card*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->Proc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  D2D1_POINT_2F Mouse(LPARAM lp) const {
    float s = 96.f / dpi_;
    return D2D1::Point2F(GET_X_LPARAM(lp) * s, GET_Y_LPARAM(lp) * s);
  }
  bool InCard(D2D1_POINT_2F p) const {
    return p.x >= kMargin && p.x <= kMargin + kCardW && p.y >= kMargin && p.y <= kMargin + kCardH;
  }
  bool InClose(D2D1_POINT_2F p) const {
    float cx = kMargin + kCardW - 22, cy = kMargin + kCardH / 2;
    return std::hypot(p.x - cx, p.y - cy) <= 14;
  }

  LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
      case WM_NCHITTEST: {
        // Only the card itself catches the mouse; the shadow margin lets clicks through to our own
        // windows at least, and keeps the hot area honest.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        float s = 96.f / dpi_;
        return InCard(D2D1::Point2F(pt.x * s, pt.y * s)) ? HTCLIENT : HTTRANSPARENT;
      }
      case WM_MOUSEMOVE: {
        if (!tracking_) {
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
          TrackMouseEvent(&tme);
          tracking_ = true;
        }
        auto p = Mouse(lp);
        hover_.To(1);
        closeHot_.To(InClose(p) ? 1.f : 0.f);
        RequestAnimation();
        return 0;
      }
      case WM_MOUSELEAVE:
        tracking_ = false;
        hover_.To(0);
        closeHot_.To(0);
        RequestAnimation();
        return 0;
      case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        float s = 96.f / dpi_;
        SetCursor(LoadCursor(nullptr, InClose(D2D1::Point2F(pt.x * s, pt.y * s)) ? IDC_HAND : IDC_SIZEALL));
        return TRUE;
      }
      case WM_LBUTTONDOWN: {
        auto p = Mouse(lp);
        if (InClose(p)) { Close(); return 0; }
        POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd_, &screen);
        if (DragDetect(hwnd_, screen)) StartDrag();
        return 0;
      }
      case WM_LBUTTONDBLCLK: {
        if (InClose(Mouse(lp))) return 0;
        if (files_.size() == 1 && !util::IsDirectory(files_[0]))
          ShellExecuteW(nullptr, L"open", files_[0].c_str(), nullptr, util::DirOf(files_[0]).c_str(), SW_SHOWNORMAL);
        else
          RevealInExplorer(files_);
        return 0;
      }
      case WM_RBUTTONUP: RevealInExplorer(files_); return 0;
      case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
  }

  void StartDrag() {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& f : files_) {
      PIDLIST_ABSOLUTE p = nullptr;
      if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &p, 0, nullptr))) pidls.push_back(p);
    }
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &items)) &&
        SUCCEEDED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
      DWORD effect = DROPEFFECT_NONE;
      // The shell's own drag loop adds the familiar drag image and the default drop source.
      SHDoDragDrop(hwnd_, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    }
    for (auto p : pidls) CoTaskMemFree(p);
    // If the drop moved the files elsewhere, the card has nothing left to hold.
    bool any = false;
    for (auto& f : files_) any |= util::Exists(f);
    if (!any) Close();
  }

  void Place() {
    float s = dpi_ / 96.f;
    int x = work_.right - (int)std::lround((kCardW + kMargin * 2 - 4 - slide_.v) * s);
    int y = work_.top + (int)std::lround((y_.v + 4) * s);
    if (x != lastX_ || y != lastY_) {
      SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      lastX_ = x;
      lastY_ = y;
    }
  }

  void Draw(ID2D1DeviceContext* dc) {
    const Theme& t = theme_;
    float A = std::clamp(alpha_.v, 0.f, 1.f), hv = std::clamp(hover_.v, 0.f, 1.f);
    dc->SetTransform(Base(surf_.Base()));
    auto card = D2D1::RectF(kMargin, kMargin, kMargin + kCardW, kMargin + kCardH);
    for (int i = 4; i >= 1; --i) {
      float g = i * 3.f;
      dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left - g, card.top - g + 5, card.right + g, card.bottom + g + 5), 12 + g, 12 + g),
                               g_gfx.Brush(t.shadow, t.shadowOpacity * (.10f + .04f * hv) * A));
    }
    auto rr = D2D1::RoundedRect(card, 12, 12);
    D2D1_COLOR_F fill = t.glassHub;
    fill.a = std::max(fill.a, .95f);
    dc->FillRoundedRectangle(rr, g_gfx.Brush(fill, A));
    dc->FillRoundedRectangle(rr, g_gfx.Brush(t.accent, .05f * hv * A));
    dc->DrawRoundedRectangle(rr, g_gfx.Brush(Mix(t.glassLine, t.accent, .5f * hv), A), 1);

    // Thumbnail, or a generic page if the shell had none.
    auto thumbRect = D2D1::RectF(card.left + 12, card.top + 12, card.left + 56, card.top + 56);
    if (thumb_) {
      D2D1_SIZE_F sz = thumb_->GetSize();
      float sc = std::min(44 / sz.width, 44 / sz.height);
      float w = sz.width * sc, h = sz.height * sc;
      auto r = D2D1::RectF(thumbRect.left + (44 - w) / 2, thumbRect.top + (44 - h) / 2, thumbRect.left + (44 + w) / 2,
                           thumbRect.top + (44 + h) / 2);
      dc->DrawBitmap(thumb_.Get(), &r, A, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } else {
      dc->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 6, 6), g_gfx.Brush(t.seg, .08f * A));
    }
    if (files_.size() > 1) {
      auto badge = D2D1::Ellipse(D2D1::Point2F(thumbRect.right - 2, thumbRect.top + 2), 9, 9);
      dc->FillEllipse(badge, g_gfx.Brush(t.accent, A));
      auto n = g_gfx.Layout(std::to_wstring(std::min<size_t>(files_.size(), 99)), g_gfx.Format(10.f, DWRITE_FONT_WEIGHT_BOLD), 18);
      DWRITE_TEXT_METRICS m{};
      n->GetMetrics(&m);
      dc->DrawTextLayout(D2D1::Point2F(thumbRect.right - 11, thumbRect.top + 2 - m.height / 2), n.Get(), g_gfx.Brush(t.onAccent, A));
    }
    if (name_) dc->DrawTextLayout(D2D1::Point2F(card.left + 66, card.top + 15), name_.Get(), g_gfx.Brush(t.fg, A));
    if (sub_) dc->DrawTextLayout(D2D1::Point2F(card.left + 66, card.top + 36), sub_.Get(), g_gfx.Brush(t.fg2, A));

    // Close button: faint until the card is hovered.
    float ch = std::clamp(closeHot_.v, 0.f, 1.f);
    D2D1_POINT_2F cc = D2D1::Point2F(card.right - 22, card.top + kCardH / 2);
    if (ch > .01f) dc->FillEllipse(D2D1::Ellipse(cc, 13, 13), g_gfx.Brush(t.seg, .12f * ch * A));
    dc->SetTransform(M::Scale(12.f / 24, 12.f / 24) * M::Translation(cc.x - 6, cc.y - 6) * Base(surf_.Base()));
    dc->DrawGeometry(g_gfx.Icon(kClose), g_gfx.Brush(t.fg2, (.45f + .55f * std::max(hv, ch)) * A), 2.2f, g_gfx.round.Get());
    dc->SetTransform(surf_.Base());
  }

  HWND hwnd_ = nullptr;
  CompSurface surf_;
  std::vector<std::wstring> files_;
  RECT work_{};
  float dpi_ = 96;
  UINT w_ = 0, h_ = 0;
  Theme theme_;
  ComPtr<ID2D1Bitmap1> thumb_;
  ComPtr<IDWriteTextLayout> name_, sub_;
  Spring alpha_, slide_, y_, hover_, closeHot_;
  bool closing_ = false, tracking_ = false, dirty_ = true, wasMoving_ = false;
  int lastX_ = INT_MIN, lastY_ = INT_MIN;
};

void Restack(bool snapNewest) {
  int slot = 0;
  for (size_t i = 0; i < g_cards.size(); ++i) {
    if (g_cards[i]->Closing()) continue;
    g_cards[i]->SetSlot(slot++, snapNewest && i + 1 == g_cards.size());
  }
}

}  // namespace

namespace pins {

void Pin(const std::vector<std::wstring>& files, POINT from) {
  if (files.empty() || !g_gfx.Ensure()) return;
  g_cards.push_back(std::make_unique<Card>(files, from));
  Restack(true);
  g_cards.back()->Show();
  RequestAnimation();
}

bool TickAll(float dt) {
  bool any = false;
  bool removed = false;
  for (size_t i = 0; i < g_cards.size();) {
    any |= g_cards[i]->Tick(dt);
    if (g_cards[i]->Dead()) {
      g_cards.erase(g_cards.begin() + i);
      removed = true;
    } else {
      ++i;
    }
  }
  static size_t lastOpen = 0;
  size_t open = 0;
  for (auto& c : g_cards) open += !c->Closing();
  if (removed || open != lastOpen) {
    Restack(false);
    any = true;
  }
  lastOpen = open;
  return any;
}

void RenderAll() {
  for (auto& c : g_cards) c->Render();
}

size_t Count() { return g_cards.size(); }

void Clear() { g_cards.clear(); }

}  // namespace pins

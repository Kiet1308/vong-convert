#pragma once
#include <oleidl.h>

#include <climits>

#include "catalog.h"
#include "gfx.h"
#include "perf.h"
#include "spring.h"

// What the user picked, handed to main.cpp to run.
struct WheelCommit {
  Mode mode = Mode::Convert;
  Item item;
  SubItem sub;
  bool hasSub = false;
  std::vector<std::wstring> files;
  POINT center{};          // screen pixels
  float dpi = 96;
  float hubDiameter = 110;  // DIPs, so the progress bubble can start at the hub's size
};

class Wheel {
 public:
  bool Create();
  void Destroy();
  void ReleaseGraphics();  // drops every GPU resource; rebuilt on the next drag

  void OnModifier(int mod, bool down);
  // Opens the wheel for files already selected in Explorer; driven by keys from the hook.
  void OpenKeyboard(const std::vector<std::wstring>& files, POINT anchor, HWND owner);
  void OnKey(UINT vk, bool shift);
  void OnMouse(UINT msg, LPARAM lp);
  void OnPollTimer();
  bool Tick(float dt);  // returns true while something still animates
  void Render();
  bool Active() const { return state_ != State::Hidden; }
  void SetPaused(bool paused) { paused_ = paused; }
  bool Paused() const { return paused_; }

  DWORD OnDragEnter(IDataObject* data, POINTL pt, DWORD allowed);
  DWORD OnDragOver(POINTL pt, DWORD allowed);
  void OnDragLeave();
  DWORD OnDrop(POINTL pt, DWORD allowed);

  // Times opening and drawing offscreen; returns a report (used with --bench).
  std::wstring Bench(const std::vector<std::wstring>& files, float dpi);

  // Renders an open wheel over a synthetic desktop into a PNG (used with --render-test).
  bool RenderTest(const std::wstring& out, Mode mode, const std::vector<std::wstring>& files,
                  const std::wstring& selectId, int sub, float dpi, int theme);

 private:
  enum class State { Hidden, Armed, Open, Closing };
  struct Seg {
    ComPtr<ID2D1Geometry> geo;
    ComPtr<IDWriteTextLayout> label;
    float labelW = 0, labelH = 0;
    float mx = 0, my = 0, ux = 0, uy = 0;
    Spring a, h;
    float delay = 0;
    ID2D1PathGeometry* icon = nullptr;
  };
  struct Hit {
    enum Zone { None, Center, Segment, Sub } zone = None;
    int i = -1, j = -1;
  };
  struct Pill {
    ComPtr<IDWriteTextLayout> key, label;
    float w = 0, keyW = 0, labelW = 0;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  bool Place(POINT pt);
  void Arm(Mode mode);
  void Open(Mode mode);
  void Close(bool commit);
  void Hide();
  void Commit();
  void BuildStatic();
  void BuildSegs(Mode mode);
  void SwitchMode(Mode mode);
  void RequestSub(int i);
  void BuildSub(int i);
  void SetSel(int i, int j, bool force = false);
  void UpdateHub();
  void BuildLegend();
  Hit HitTest(POINT screen) const;
  Hit HitLocal(float x, float y) const;
  bool CaptureBackground();
  void TakeGrab(bool fade);
  void MakeBlurBrush();
  bool MakeBlur(ID2D1Bitmap1* source);
  void Draw(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base);
  D2D1_POINT_2F CenterDip() const;

  State state_ = State::Hidden;
  HWND hwnd_ = nullptr;
  CompSurface surf_;
  IDropTarget* drop_ = nullptr;
  bool paused_ = false, test_ = false, kb_ = false;
  HWND kbOwner_ = nullptr;

  Mode mode_ = Mode::Convert, pendingMode_ = Mode::Convert;
  std::vector<std::wstring> files_;
  float dpi_ = 96;
  POINT center_{};
  RECT win_{};
  ULONGLONG lmbUpSince_ = 0;
  Theme theme_;
  bool solid_ = false;

  std::vector<Item> items_;
  std::vector<Seg> segs_, subs_;
  int sel_ = -1, sub_ = -1, subParent_ = -1, pendingSub_ = INT_MIN;
  float subTheta_ = 0;
  bool touched_ = false, commit_ = false, hubOff_ = false;
  Spring alpha_, scale_, subA_, notchA_, notchO_, swap_;

  std::shared_ptr<ScreenGrab> grab_;
  double openedAt_ = 0, firstFrameMs_ = -1, glassMs_ = -1;
  perf::Stats frames_;
  Spring glassA_;
  ComPtr<ID2D1Bitmap1> blur_;
  ComPtr<ID2D1BitmapBrush1> blurBrush_;
  ComPtr<ID2D1PathGeometry> ring_, shadowRing_, hubShadowRing_, notch_;
  int shapesGen_ = -1;
  ComPtr<ID2D1RadialGradientBrush> ringShadow_, hubShadow_;
  ComPtr<ID2D1LinearGradientBrush> ringHi_, hubHi_;

  ComPtr<IDWriteTextLayout> h1_, h2_, h3_, h4_;
  bool h1Accent_ = false;
  float hubTotal_ = 0;
  std::wstring hubKey_;
  Pill pills_[3];
  ComPtr<IDWriteTextLayout> kbHint_;
  float pillsW_ = 0;
};

extern Wheel g_wheel;

// Implemented in main.cpp: runs the chosen job or action.
void OnWheelCommit(const WheelCommit& commit);

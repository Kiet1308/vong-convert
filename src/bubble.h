#pragma once
#include "common.h"
#include "gfx.h"
#include "spring.h"

// The small progress disc left behind where the wheel was. It grows out of the
// wheel's hub, fills a ring while the job runs, then shows a check and fades.
// Clicking it after success opens the output in Explorer.
class Bubble {
 public:
  Bubble(POINT center, float dpi, float startDiameter, std::wstring label);
  ~Bubble();
  void SetProgress(float p);  // 0..1, or negative when the job cannot report progress
  void Finish(bool ok, std::vector<std::wstring> outputs, std::wstring pill);
  bool Tick(float dt);  // true while it needs more frames
  bool Dead() const;    // faded out; safe to delete
  void Render();
  void OnClick();
  // Flies to a point on screen (the new file in Explorer) and fades there.
  void FlyTo(POINT target);
  bool Done() const { return state_ == State::Done; }

  int jobId = 0;

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  void Draw(ID2D1DeviceContext* dc);

  HWND hwnd_ = nullptr;
  CompSurface surf_;
  UINT w_ = 0, h_ = 0;
  float dpi_;
  Theme theme_;
  std::wstring label_, pill_;
  std::vector<std::wstring> outputs_;
  enum class State { Running, Done, Flying, Fading } state_ = State::Running;
  POINT origin_{};
  Spring flyX_, flyY_;
  bool flown_ = false;
  bool ok_ = false, indeterminate_ = false;
  float target_ = 0, hold_ = 0;
  Spring size_, alpha_, shown_, badge_;
  ComPtr<IDWriteTextLayout> labelLayout_, pillLayout_;
  std::wstring pillShown_;
};

namespace bubbles {
Bubble* Spawn(POINT center, float dpi, float startDiameter, const std::wstring& label);
Bubble* ForJob(int jobId);
bool TickAll(float dt);
size_t Count();
void RenderAll();
void Clear();
}  // namespace bubbles

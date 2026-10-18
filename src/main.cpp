#include "common.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <atomic>
#include <deque>

#include "actions.h"
#include "bubble.h"
#include "crop.h"
#include "pin.h"
#include "gfx.h"
#include "jobs.h"
#include "perf.h"
#include "settings.h"
#include "util.h"
#include "wheel.h"

HINSTANCE g_hinst = nullptr;
HWND g_hwndMain = nullptr;

namespace {

constexpr UINT WM_APP_COMMIT = WM_APP + 10;
constexpr UINT WM_APP_KBOPEN = WM_APP + 11;  // lParam: the Explorer window
constexpr UINT WM_APP_KBKEY = WM_APP + 12;   // wParam: virtual key, lParam: Shift held
constexpr UINT ID_PAUSE = 100, ID_STARTUP = 101, ID_LOG = 102, ID_HELP = 103, ID_EXIT = 104;
constexpr UINT ID_LAYOUT = 200, ID_GLASS = 210, ID_SOLID = 211, ID_KEYBOARD = 212;
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring HelpText() {
  std::wstring t = std::wstring(L"Kéo một file rồi giữ ") + KeyForMode(Mode::Convert) + L" để chuyển đổi, " +
                   KeyForMode(Mode::Tools) + L" để dùng công cụ, " + KeyForMode(Mode::Actions) + L" để chia sẻ, ghim, copy.";
  if (g_settings.keyboard) t += L" Hoặc chọn file trong Explorer rồi bấm Ctrl+Shift+Space.";
  return t;
}

HANDLE g_animEvent = nullptr;
std::atomic<bool> g_framePending{false}, g_quit{false};
bool g_reducedMotion = false, g_clockIdle = true;
LARGE_INTEGER g_lastFrame{}, g_freq{};
DWORD g_hookThreadId = 0;
HANDLE g_clockThread = nullptr, g_hookThread = nullptr;
bool g_modDown[3] = {};
UINT g_taskbarCreated = 0;
NOTIFYICONDATAW g_nid{};
std::vector<std::wstring> g_lastOutputs;
std::deque<WheelCommit> g_commits;
bool g_cliJob = false;
int g_cliCode = 6;  // stays 6 if the job never finished

// ---------------------------------------------------------------- frame clock

// The display output showing the cursor, so frames follow that monitor's refresh rate.
ComPtr<IDXGIOutput> OutputUnderCursor(ComPtr<IDXGIFactory1>& factory, HMONITOR& cached, ComPtr<IDXGIOutput> current) {
  POINT pt;
  GetCursorPos(&pt);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  if (mon == cached && current && factory && factory->IsCurrent()) return current;
  cached = mon;
  if (!factory || !factory->IsCurrent()) {
    factory.Reset();
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  }
  if (!factory) return nullptr;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i, adapter.Reset()) {
    ComPtr<IDXGIOutput> out;
    for (UINT j = 0; SUCCEEDED(adapter->EnumOutputs(j, &out)); ++j, out.Reset()) {
      DXGI_OUTPUT_DESC d;
      if (SUCCEEDED(out->GetDesc(&d)) && d.Monitor == mon) return out;
    }
  }
  return nullptr;
}

DWORD WINAPI ClockThread(LPVOID) {
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIOutput> output;
  HMONITOR monitor = nullptr;
  while (!g_quit) {
    WaitForSingleObject(g_animEvent, INFINITE);
    if (g_quit) break;
    output = OutputUnderCursor(factory, monitor, output);
    if (!output || FAILED(output->WaitForVBlank())) Sleep(15);
    if (!g_framePending.exchange(true)) PostMessageW(g_hwndMain, WM_APP_FRAME, 0, 0);
  }
  return 0;
}

constexpr UINT_PTR kIdleTimer = 3, kWarmTimer = 4, kLocateTimer = 5, kSleepTimer = 6;

struct Locate {
  int jobId;
  std::vector<std::wstring> outputs;
  int tries;
};
std::vector<Locate> g_locate;

// Explorer lists new files a moment after they appear, so this retries for about two seconds.
void LocateOutputs() {
  for (size_t i = 0; i < g_locate.size();) {
    Locate& l = g_locate[i];
    Bubble* b = bubbles::ForJob(l.jobId);
    RECT rc{};
    bool done = !b || !b->Done();
    if (!done && SelectInOpenExplorer(l.outputs, &rc)) {
      // Aim at the icon end: a details row is wide, an icon tile is about square.
      LONG w = std::min(rc.right - rc.left, (rc.bottom - rc.top) * 3);
      b->FlyTo({rc.left + w / 2, (rc.top + rc.bottom) / 2});
      done = true;
    }
    if (done || ++l.tries > 16) g_locate.erase(g_locate.begin() + i);
    else ++i;
  }
  if (g_locate.empty()) KillTimer(g_hwndMain, kLocateTimer);
}

bool Busy() { return g_wheel.Active() || bubbles::Count() || crop::Active(); }

void OnFrame() {
  g_framePending = false;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  float dt = g_clockIdle ? 1.f / 60 : (float)(now.QuadPart - g_lastFrame.QuadPart) / g_freq.QuadPart;
  dt = std::clamp(dt, 1.f / 240, 1.f / 20);
  g_lastFrame = now;
  g_clockIdle = false;
  g_gfx.deferCommit = true;
  bool more = g_wheel.Tick(dt);
  g_wheel.Render();
  more |= bubbles::TickAll(dt);
  bubbles::RenderAll();
  more |= crop::Tick(dt);
  crop::Render();
  more |= pins::TickAll(dt);
  pins::RenderAll();
  g_gfx.deferCommit = false;
  g_gfx.CommitIfPending();
  if (!more) {
    ResetEvent(g_animEvent);
    g_clockIdle = true;
    // Nothing on screen: hand idle pages back to Windows after a quiet spell.
    if (!Busy()) SetTimer(g_hwndMain, kIdleTimer, 3000, nullptr);
  }
  if (g_gfx.lost) {
    // The GPU was reset (driver update, sleep): drop everything; the next drag rebuilds it.
    util::Log(L"graphics device lost, rebuilding on next use");
    g_wheel.ReleaseGraphics();
    crop::Close();
    g_gfx.Shutdown();
  }
}

void TrimIfIdle() {
  KillTimer(g_hwndMain, kIdleTimer);
  if (Busy()) return;
  // Nothing on screen: hand GPU caches and idle pages back. The devices stay, so the next drag is instant.
  g_gfx.Idle();
  SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
  // After a long quiet spell the devices go too, freeing all GPU memory; the next drag rebuilds them.
  SetTimer(g_hwndMain, kSleepTimer, 5 * 60 * 1000, nullptr);
}

void SleepIfIdle() {
  KillTimer(g_hwndMain, kSleepTimer);
  if (Busy() || pins::Count() || !g_gfx.Ready()) return;
  g_wheel.ReleaseGraphics();
  g_gfx.Shutdown();
  SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
  util::Log(L"graphics released after a long idle");
}

// ---------------------------------------------------------------- keyboard hook

std::atomic<bool> g_keyCapture{false};

bool ExplorerInForeground(HWND fg) {
  wchar_t cls[64] = {};
  GetClassNameW(fg, cls, 64);
  return !wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"ExploreWClass") || !wcscmp(cls, L"Progman") ||
         !wcscmp(cls, L"WorkerW");
}

bool IsWheelKey(DWORD vk) {
  return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_RETURN || vk == VK_ESCAPE ||
         vk == VK_TAB || vk == VK_SPACE;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION) {
    auto* k = (KBDLLHOOKSTRUCT*)lp;
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    int mod = -1;
    switch (k->vkCode) {
      case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: mod = 0; break;
      case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: mod = 1; break;
      case VK_MENU: case VK_LMENU: case VK_RMENU: mod = 2; break;
    }
    if (mod >= 0) {
      if (down != g_modDown[mod]) {
        g_modDown[mod] = down;
        PostMessageW(g_hwndMain, WM_APP_MODKEY, mod, down);
      }
    } else if (g_keyCapture && IsWheelKey(k->vkCode)) {
      // The keyboard wheel is open: its keys belong to it, not to Explorer underneath.
      if (down) PostMessageW(g_hwndMain, WM_APP_KBKEY, k->vkCode, g_modDown[0]);
      return 1;
    } else if (down && k->vkCode == VK_SPACE && g_modDown[0] && g_modDown[1] && !g_modDown[2] && g_settings.keyboard) {
      HWND fg = GetForegroundWindow();
      if (ExplorerInForeground(fg)) {
        PostMessageW(g_hwndMain, WM_APP_KBOPEN, 0, (LPARAM)fg);
        return 1;
      }
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

// The hook lives on its own thread so a busy UI thread never delays the user's typing.
DWORD WINAPI HookThread(LPVOID) {
  HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_hinst, 0);
  if (!hook) util::Log(L"SetWindowsHookEx failed (%lu)", GetLastError());
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
  }
  if (hook) UnhookWindowsHookEx(hook);
  return 0;
}

// ---------------------------------------------------------------- tray

bool StartupEnabled() {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"VongConvert", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetStartup(bool on) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  if (on) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring v = L"\"" + std::wstring(exe) + L"\"";
    RegSetValueExW(key, L"VongConvert", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, L"VongConvert");
  }
  RegCloseKey(key);
}

void AddTrayIcon() {
  g_nid = {};
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = g_hwndMain;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_nid.uCallbackMessage = WM_APP_TRAY;
  LoadIconMetric(g_hinst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_nid.hIcon);
  wcsncpy(g_nid.szTip, L"Vòng Convert: kéo file rồi giữ Shift, Ctrl hoặc Alt", std::size(g_nid.szTip) - 1);
  Shell_NotifyIconW(NIM_ADD, &g_nid);
  g_nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void Notify(const std::wstring& title, const std::wstring& text) {
  NOTIFYICONDATAW n = g_nid;
  n.uFlags = NIF_INFO;
  wcsncpy(n.szInfoTitle, title.c_str(), std::size(n.szInfoTitle) - 1);
  wcsncpy(n.szInfo, text.empty() ? L" " : text.c_str(), std::size(n.szInfo) - 1);
  n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  LoadIconMetric(g_hinst, MAKEINTRESOURCEW(1), LIM_LARGE, &n.hBalloonIcon);
  Shell_NotifyIconW(NIM_MODIFY, &n);
  if (n.hBalloonIcon) DestroyIcon(n.hBalloonIcon);
}

void ShowTrayMenu(POINT pt) {
  static const wchar_t* keyNames[3] = {L"Shift", L"Ctrl", L"Alt"};
  HMENU keys = CreatePopupMenu();
  for (int l = 0; l < kKeyLayoutCount; ++l) {
    const int* layout = KeyLayouts()[l];
    // Show which key opens each wheel, in wheel order: convert, tools, actions.
    std::wstring label;
    for (int mode = 0; mode < 3; ++mode)
      for (int key = 0; key < 3; ++key)
        if (layout[key] == mode) label += (mode ? L"  ·  " : L"") + std::wstring(keyNames[key]) + L" " + util::Lower(ModeTitle((Mode)mode));
    bool on = layout[0] == g_settings.keyMode[0] && layout[1] == g_settings.keyMode[1];
    AppendMenuW(keys, MF_STRING | (on ? MF_CHECKED : 0), ID_LAYOUT + l, label.c_str());
  }
  HMENU style = CreatePopupMenu();
  AppendMenuW(style, MF_STRING | (!g_settings.solid ? MF_CHECKED : 0), ID_GLASS, L"Kính mờ");
  AppendMenuW(style, MF_STRING | (g_settings.solid ? MF_CHECKED : 0), ID_SOLID, L"Đặc");

  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Vòng Convert");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_HELP, L"Cách dùng");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)keys, L"Phím giữ khi kéo");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)style, L"Kiểu vòng");
  AppendMenuW(m, MF_STRING | (g_settings.keyboard ? MF_CHECKED : 0), ID_KEYBOARD,
              L"Ctrl+Shift+Space mở vòng cho file đang chọn");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (g_wheel.Paused() ? MF_CHECKED : 0), ID_PAUSE, L"Tạm dừng");
  AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Khởi động cùng Windows");
  AppendMenuW(m, MF_STRING, ID_LOG, L"Mở thư mục nhật ký");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_EXIT, L"Thoát");
  SetForegroundWindow(g_hwndMain);
  UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwndMain, nullptr);
  PostMessageW(g_hwndMain, WM_NULL, 0, 0);
  DestroyMenu(m);  // also destroys the submenus
  if (cmd >= ID_LAYOUT && cmd < ID_LAYOUT + kKeyLayoutCount) {
    for (int i = 0; i < 3; ++i) g_settings.keyMode[i] = KeyLayouts()[cmd - ID_LAYOUT][i];
    SaveSettings();
    return;
  }
  switch (cmd) {
    case ID_HELP: Notify(L"Cách dùng Vòng Convert", HelpText()); break;
    case ID_GLASS: case ID_SOLID: g_settings.solid = cmd == ID_SOLID; SaveSettings(); break;
    case ID_KEYBOARD: g_settings.keyboard = !g_settings.keyboard; SaveSettings(); break;
    case ID_PAUSE: g_wheel.SetPaused(!g_wheel.Paused()); break;
    case ID_STARTUP: SetStartup(!StartupEnabled()); break;
    case ID_LOG: ShellExecuteW(nullptr, L"open", util::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_EXIT: DestroyWindow(g_hwndMain); break;
  }
}

// ---------------------------------------------------------------- running a choice

void RunCommit(const WheelCommit& c) {
  std::wstring label = c.item.label + (c.hasSub ? L" " + c.sub.label : L"");
  if (c.mode == Mode::Actions && c.item.id == L"float") {
    pins::Pin(c.files, c.center);
    return;
  }
  if (c.mode == Mode::Actions) {
    Bubble* b = bubbles::Spawn(c.center, c.dpi, c.hubDiameter, c.item.label);
    const std::wstring& id = c.item.id;
    bool ok = false;
    std::wstring pill;
    std::vector<std::wstring> outputs;
    if (id == L"copy") {
      ok = ActionCopyFiles(g_hwndMain, c.files);
      pill = ok ? L"Đã copy, dán bằng Ctrl+V" : L"Không copy được";
    } else if (id == L"path") {
      ok = ActionCopyPaths(g_hwndMain, c.files);
      pill = ok ? L"Đã copy đường dẫn" : L"Không copy được";
    } else if (id == L"terminal") {
      ok = ActionTerminal(c.files);
      pill = ok ? L"Đã mở Terminal" : L"Không mở được";
    } else if (id == L"share") {
      ok = ActionShare(g_hwndMain, c.files);
      pill = ok ? L"Đã mở Chia sẻ" : L"Không mở được Chia sẻ";
    } else if (id == L"move") {
      std::wstring dest;
      ok = ActionMove(g_hwndMain, c.files, &dest);
      pill = ok ? L"Đã chuyển" : L"Đã huỷ";
      if (ok)
        for (auto& f : c.files) outputs.push_back(dest + L"\\" + util::FileNameOf(f));
    }
    b->Finish(ok, outputs, pill);
    util::Log(L"action %ls: %ls", id.c_str(), ok ? L"ok" : L"failed");
    return;
  }
  if (c.mode == Mode::Tools && c.item.id == L"crop") {
    // Cropping needs the user to pick the area first.
    auto targets = TargetsFor(c.mode, c.files);
    if (!targets.empty()) crop::Open(targets[0], c.center);
    return;
  }
  JobRequest r;
  r.mode = c.mode;
  r.itemId = c.item.id;
  r.subId = c.hasSub ? c.sub.id : L"";
  r.files = c.files;
  Bubble* b = bubbles::Spawn(c.center, c.dpi, c.hubDiameter, label);
  int id = StartJob(g_hwndMain, r);
  if (!id) b->Finish(false, {}, L"Không có file phù hợp");
  else b->jobId = id;
}

void OnJobMessage(int id, LPARAM value) {
  Bubble* b = bubbles::ForJob(id);
  if (value == kJobDone || value == kJobFailed) {
    JobResult r = TakeJobResult(id);
    if (b) {
      b->Finish(r.ok, r.outputs, r.ok ? (r.outputs.empty() ? L"Xong" : L"Bấm để xem") : L"Không xử lý được");
      // If the folder is open in Explorer, select the result there and fly the bubble into it.
      if (r.ok && !r.outputs.empty()) {
        g_locate.push_back({id, r.outputs, 0});
        SetTimer(g_hwndMain, kLocateTimer, 120, nullptr);
      }
    }
    if (r.ok) g_lastOutputs = r.outputs;
    if (!g_cliJob) Notify(r.title, r.detail);
    if (g_cliJob) {
      g_cliCode = r.ok ? 0 : 5;
      DestroyWindow(g_hwndMain);
    }
    return;
  }
  if (b) b->SetProgress(value == kJobIndeterminate ? -1.f : value / 1000.f);
}

// ---------------------------------------------------------------- main window

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_APP_FRAME: OnFrame(); return 0;
    case WM_TIMER:
      if (wp == kIdleTimer) TrimIfIdle();
      if (wp == kSleepTimer) SleepIfIdle();
      if (wp == kLocateTimer) LocateOutputs();
      if (wp == kWarmTimer) {
        // Build the graphics devices ahead of the first drag so the wheel opens instantly.
        KillTimer(hwnd, kWarmTimer);
        g_gfx.Ensure();
        SetTimer(hwnd, kIdleTimer, 5000, nullptr);
      }
      return 0;
    case WM_APP_MODKEY: g_wheel.OnModifier((int)wp, lp != 0); return 0;
    case WM_APP_KBKEY: g_wheel.OnKey((UINT)wp, lp != 0); return 0;
    case WM_APP_KBOPEN: {
      if (g_wheel.Paused() || g_wheel.Active()) return 0;
      HWND explorer = (HWND)lp;
      POINT anchor{};
      auto files = ExplorerSelection(explorer, &anchor);
      if (files.empty()) Notify(L"Chưa chọn file nào", L"Chọn một hoặc vài file trong Explorer rồi bấm lại Ctrl+Shift+Space.");
      else g_wheel.OpenKeyboard(files, anchor, explorer);
      return 0;
    }
    case WM_APP_JOB: OnJobMessage((int)wp, lp); return 0;
    case WM_APP_COMMIT:
      while (!g_commits.empty()) {
        WheelCommit c = g_commits.front();
        g_commits.pop_front();
        RunCommit(c);
      }
      return 0;
    case WM_APP_TRAY:
      switch (LOWORD(lp)) {
        case WM_CONTEXTMENU: ShowTrayMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}); break;
        case NIN_SELECT:
        case NIN_KEYSELECT: Notify(L"Cách dùng Vòng Convert", HelpText()); break;
        case NIN_BALLOONUSERCLICK:
          if (!g_lastOutputs.empty()) RevealInExplorer(g_lastOutputs);
          break;
      }
      return 0;
    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &g_nid);
      PostQuitMessage(0);
      return 0;
  }
  if (msg == g_taskbarCreated && msg) {
    AddTrayIcon();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int RenderTest(int argc, wchar_t** argv) {
  // --render-test out.png mode file [select] [sub] [dpi] [theme]
  std::wstring out = argv[2];
  Mode mode = (Mode)_wtoi(argv[3]);
  std::vector<std::wstring> files = {argv[4]};
  std::wstring sel = argc > 5 ? argv[5] : L"";
  int sub = argc > 6 ? _wtoi(argv[6]) : -1;
  float dpi = argc > 7 ? (float)_wtof(argv[7]) : 120.f;
  int theme = argc > 8 ? _wtoi(argv[8]) : -1;
  bool ok = g_wheel.RenderTest(out, mode, files, sel, sub, dpi, theme);
  return ok ? 0 : 2;
}

}  // namespace

void RequestAnimation() { SetEvent(g_animEvent); }
void SetKeyCapture(bool on) { g_keyCapture = on; }
bool ReducedMotion() { return g_reducedMotion; }

void OnCropConfirmed(const std::wstring& file, int x, int y, int w, int h, POINT center, float dpi) {
  JobRequest r;
  r.mode = Mode::Tools;
  r.itemId = L"crop";
  r.subId = std::to_wstring(x) + L":" + std::to_wstring(y) + L":" + std::to_wstring(w) + L":" + std::to_wstring(h);
  r.files = {file};
  Bubble* b = bubbles::Spawn(center, dpi, 40, L"Cắt");
  int id = StartJob(g_hwndMain, r);
  if (!id) b->Finish(false, {}, L"Không cắt được");
  else b->jobId = id;
}

void OnWheelCommit(const WheelCommit& commit) {
  // Runs after Drop returns, so the app that started the drag is never kept waiting.
  g_commits.push_back(commit);
  PostMessageW(g_hwndMain, WM_APP_COMMIT, 0, 0);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
  g_hinst = hinst;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  OleInitialize(nullptr);
  QueryPerformanceFrequency(&g_freq);
  BOOL anim = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
  g_reducedMotion = !anim;
  g_animEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LoadSettings();

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 5 && !wcscmp(argv[1], L"--render-test")) return RenderTest(argc, argv);
  if (argc >= 3 && !wcscmp(argv[1], L"--bench")) {  // --bench image: timings and memory into the log
    util::Log(L"bench: start, %ls", perf::Memory().c_str());
    double t0 = perf::NowMs();
    g_gfx.Ensure();
    util::Log(L"bench: graphics %.0f ms, %ls", perf::NowMs() - t0, perf::Memory().c_str());
    std::wstring report = g_wheel.Bench({argv[2]}, 144) + crop::Bench(argv[2]);
    for (size_t a = 0, b; a < report.size(); a = b + 1) {
      b = report.find(L'\n', a);
      if (b == std::wstring::npos) b = report.size();
      if (b > a) util::Log(L"bench: %ls", report.substr(a, b - a).c_str());
    }
    util::Log(L"bench: after use, %ls", perf::Memory().c_str());
    g_gfx.Idle();
    util::Log(L"bench: after idle cleanup, %ls", perf::Memory().c_str());
    g_wheel.ReleaseGraphics();
    g_gfx.Shutdown();
    util::Log(L"bench: devices released (long idle), %ls", perf::Memory().c_str());
    double t1 = perf::NowMs();
    g_gfx.Ensure();
    util::Log(L"bench: devices back in %.1f ms", perf::NowMs() - t1);
    return 0;
  }
  if (argc >= 3 && !wcscmp(argv[1], L"--share-test")) {  // --share-test file: loads the share handler only
    std::wstring text = ShareMenuText({argv[2]});
    util::Log(L"share-test: menu entry \"%ls\"", text.c_str());
    return text.empty() ? 3 : 0;
  }
  if (argc >= 3 && !wcscmp(argv[1], L"--explorer-test")) {  // --explorer-test file: select it in an open folder window
    RECT rc{};
    HWND top = nullptr;
    bool ok = SelectInOpenExplorer({argv[2]}, &rc, &top);
    util::Log(L"explorer-test: select %d at %ld,%ld-%ld,%ld", ok, rc.left, rc.top, rc.right, rc.bottom);
    POINT anchor{};
    auto sel = ok ? ExplorerSelection(top, &anchor) : std::vector<std::wstring>{};
    util::Log(L"explorer-test: selection %zu item(s), first %ls, anchor %ld,%ld", sel.size(),
              sel.empty() ? L"-" : sel[0].c_str(), anchor.x, anchor.y);
    return ok && !sel.empty() ? 0 : 3;
  }
  if (argc >= 4 && !wcscmp(argv[1], L"--crop-test"))  // --crop-test file out.png [preset]
    return crop::RenderTest(argv[2], argv[3], argc > 4 ? _wtoi(argv[4]) : 0) ? 0 : 2;
  g_cliJob = argc >= 6 && !wcscmp(argv[1], L"--job");
  HANDLE mutex = g_cliJob ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\VongConvert.Singleton");
  if (!g_cliJob && GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Vòng Convert đang chạy rồi. Biểu tượng nằm ở khay hệ thống.", L"Vòng Convert",
                MB_ICONINFORMATION);
    return 0;
  }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = MainProc;
  wc.hInstance = hinst;
  wc.lpszClassName = L"VongConvert.Main";
  RegisterClassExW(&wc);
  g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Vòng Convert", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, hinst, nullptr);
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  if (!g_wheel.Create()) {
    MessageBoxW(nullptr, L"Không tạo được cửa sổ vòng tròn.", L"Vòng Convert", MB_ICONERROR);
    return 1;
  }
  g_clockThread = CreateThread(nullptr, 0, ClockThread, nullptr, 0, nullptr);
  g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
  util::Log(L"started");

  if (g_cliJob) {
    // --job mode item sub|- file...: runs one job without any UI and exits with 0 on success.
    JobRequest r;
    r.mode = (Mode)_wtoi(argv[2]);
    r.itemId = argv[3];
    r.subId = wcscmp(argv[4], L"-") ? argv[4] : L"";
    for (int i = 5; i < argc; ++i) r.files.push_back(argv[i]);
    if (!StartJob(g_hwndMain, r)) return 7;
  } else {
    AddTrayIcon();
    SetTimer(g_hwndMain, kWarmTimer, 1500, nullptr);
    // Start with Windows by default; once set, turning it off in the tray menu sticks.
    if (!g_settings.startupSet) {
      SetStartup(true);
      g_settings.startupSet = true;
      SaveSettings();
      util::Log(L"start with Windows switched on");
    }
  }

  std::wstring marker = util::DataDir() + L"\\welcomed";
  if (!g_cliJob && !util::Exists(marker)) {
    Notify(L"Vòng Convert đang chạy", HelpText());
    CloseHandle(CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Stop the helper threads before any graphics object goes away.
  g_quit = true;
  SetEvent(g_animEvent);
  PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
  for (HANDLE t : {g_clockThread, g_hookThread}) {
    if (!t) continue;
    WaitForSingleObject(t, 2000);
    CloseHandle(t);
  }
  bubbles::Clear();
  crop::Close();
  pins::Clear();
  g_wheel.Destroy();
  ShutdownJobs();
  g_gfx.Shutdown();
  OleUninitialize();
  if (mutex) CloseHandle(mutex);
  return g_cliJob ? g_cliCode : 0;
}

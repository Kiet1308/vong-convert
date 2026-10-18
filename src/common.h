#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Messages posted to the hidden main window.
constexpr UINT WM_APP_FRAME = WM_APP + 1;   // one vsync tick from the frame clock thread
constexpr UINT WM_APP_MODKEY = WM_APP + 2;  // wParam: 0 Shift, 1 Ctrl, 2 Alt; lParam: 1 down, 0 up
constexpr UINT WM_APP_JOB = WM_APP + 3;     // wParam: job id; lParam: progress in permille, or kJobDone / kJobFailed
constexpr UINT WM_APP_TRAY = WM_APP + 4;

constexpr LPARAM kJobIndeterminate = -1;
constexpr LPARAM kJobDone = 1001;
constexpr LPARAM kJobFailed = 1002;

constexpr float kPi = 3.14159265358979f;
constexpr const wchar_t* kVersion = L"0.1.0";  // keep in step with res/app.rc

extern HINSTANCE g_hinst;
extern HWND g_hwndMain;

// Wakes the frame clock so every animated window gets ticked on the next vsync.
void RequestAnimation();
// While on, the keyboard hook swallows arrows, Enter, Esc, Tab and Space and sends them to the wheel.
void SetKeyCapture(bool on);
// True when the user turned off animations in Windows settings.
bool ReducedMotion();

#include "settings.h"

Settings g_settings;

namespace {

const wchar_t* kKey = L"Software\\VongConvert";

DWORD ReadDword(const wchar_t* name, DWORD fallback) {
  DWORD v = 0, size = sizeof(v);
  return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v
                                                                                                              : fallback;
}

const int kLayouts[kKeyLayoutCount][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

}  // namespace

const int (*KeyLayouts())[3] { return kLayouts; }

void LoadSettings() {
  DWORD layout = ReadDword(L"KeyLayout", 0);
  if (layout >= (DWORD)kKeyLayoutCount) layout = 0;
  for (int i = 0; i < 3; ++i) g_settings.keyMode[i] = kLayouts[layout][i];
  g_settings.solid = ReadDword(L"Solid", 0) != 0;
  g_settings.keyboard = ReadDword(L"Keyboard", 1) != 0;
  g_settings.startupSet = ReadDword(L"StartupSet", 0) != 0;
}

void SaveSettings() {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  DWORD layout = 0;
  for (int l = 0; l < kKeyLayoutCount; ++l)
    if (kLayouts[l][0] == g_settings.keyMode[0] && kLayouts[l][1] == g_settings.keyMode[1]) layout = l;
  DWORD solid = g_settings.solid, keyboard = g_settings.keyboard, startupSet = g_settings.startupSet;
  RegSetValueExW(key, L"KeyLayout", 0, REG_DWORD, (const BYTE*)&layout, sizeof(DWORD));
  RegSetValueExW(key, L"Solid", 0, REG_DWORD, (const BYTE*)&solid, sizeof(DWORD));
  RegSetValueExW(key, L"Keyboard", 0, REG_DWORD, (const BYTE*)&keyboard, sizeof(DWORD));
  RegSetValueExW(key, L"StartupSet", 0, REG_DWORD, (const BYTE*)&startupSet, sizeof(DWORD));
  RegCloseKey(key);
}

const wchar_t* KeyForMode(Mode m) {
  static const wchar_t* names[3] = {L"Shift", L"Ctrl", L"Alt"};
  for (int i = 0; i < 3; ++i)
    if (g_settings.keyMode[i] == (int)m) return names[i];
  return L"";
}

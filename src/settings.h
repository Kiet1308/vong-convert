#pragma once
#include "catalog.h"
#include "common.h"

// User preferences, kept in HKCU\Software\VongConvert.
struct Settings {
  int keyMode[3] = {0, 1, 2};  // wheel opened by Shift, Ctrl, Alt (values are Mode)
  bool solid = false;          // opaque wheel instead of frosted glass
  bool keyboard = true;        // Ctrl+Shift+Space opens the wheel for the Explorer selection
  bool startupSet = false;     // start-with-Windows was switched on once; later the user's choice stands
};

extern Settings g_settings;

void LoadSettings();
void SaveSettings();

// The modifier ("Shift", "Ctrl", "Alt") that opens a given wheel.
const wchar_t* KeyForMode(Mode m);
// The six ways to hand the three keys to the three wheels, for the tray menu.
const int (*KeyLayouts())[3];
constexpr int kKeyLayoutCount = 6;

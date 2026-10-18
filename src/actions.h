#pragma once
#include "common.h"

// File actions from the Alt wheel. All run on the UI thread.
bool ActionCopyFiles(HWND owner, const std::vector<std::wstring>& files);
bool ActionCopyPaths(HWND owner, const std::vector<std::wstring>& files);
bool ActionTerminal(const std::vector<std::wstring>& files);
// Asks for a folder, then moves the files there with Explorer's own copy engine (undo works).
bool ActionMove(HWND owner, const std::vector<std::wstring>& files, std::wstring* destination);
// Opens the Windows share sheet for the files.
bool ActionShare(HWND owner, const std::vector<std::wstring>& files);
// The label Explorer would show for Share on these files, or empty if sharing is unavailable (tests).
std::wstring ShareMenuText(const std::vector<std::wstring>& files);

// The files selected in an Explorer window (or on the desktop) whose top-level window is `top`.
// `anchor` receives where the wheel should open: the pointer if it is over the view, else its centre.
std::vector<std::wstring> ExplorerSelection(HWND top, POINT* anchor);

// If the files' folder is open in Explorer, selects them there and returns where the first one
// sits on screen. Fails while Explorer has not listed a new file yet; callers retry briefly.
bool SelectInOpenExplorer(const std::vector<std::wstring>& files, RECT* where, HWND* window = nullptr);

// Opens the containing folder in Explorer with the paths selected.
void RevealInExplorer(const std::vector<std::wstring>& paths);
// Brings one of our windows to the front even though another app is in the foreground.
void ForceForeground(HWND hwnd);

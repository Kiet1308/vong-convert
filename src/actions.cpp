#include "actions.h"

#include <exdisp.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "util.h"

namespace {

bool OpenClipboardRetry(HWND owner) {
  for (int i = 0; i < 10; ++i) {
    if (OpenClipboard(owner)) return true;
    Sleep(20);
  }
  return false;
}

HGLOBAL GlobalText(const std::wstring& text) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (!h) return nullptr;
  memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(h);
  return h;
}

std::wstring FolderOf(const std::vector<std::wstring>& files) {
  if (files.empty()) return {};
  return util::IsDirectory(files[0]) && files.size() == 1 ? files[0] : util::DirOf(files[0]);
}

}  // namespace

void ForceForeground(HWND hwnd) {
  HWND fg = GetForegroundWindow();
  DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  if (other && other != me) {
    AttachThreadInput(me, other, TRUE);
    SetForegroundWindow(hwnd);
    AttachThreadInput(me, other, FALSE);
  } else {
    SetForegroundWindow(hwnd);
  }
}

bool ActionCopyFiles(HWND owner, const std::vector<std::wstring>& files) {
  size_t chars = 1;
  for (auto& f : files) chars += f.size() + 1;
  HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
  if (!drop) return false;
  auto* df = (DROPFILES*)GlobalLock(drop);
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  auto* p = (wchar_t*)(df + 1);
  for (auto& f : files) {
    memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
    p += f.size() + 1;
  }
  GlobalUnlock(drop);
  HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  *(DWORD*)GlobalLock(effect) = DROPEFFECT_COPY;
  GlobalUnlock(effect);

  if (!OpenClipboardRetry(owner)) { GlobalFree(drop); GlobalFree(effect); return false; }
  EmptyClipboard();
  SetClipboardData(CF_HDROP, drop);
  SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect);
  CloseClipboard();
  return true;
}

bool ActionCopyPaths(HWND owner, const std::vector<std::wstring>& files) {
  std::wstring text;
  for (size_t i = 0; i < files.size(); ++i) text += (i ? L"\r\n" : L"") + files[i];
  HGLOBAL h = GlobalText(text);
  if (!h || !OpenClipboardRetry(owner)) { if (h) GlobalFree(h); return false; }
  EmptyClipboard();
  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
  return true;
}

bool ActionTerminal(const std::vector<std::wstring>& files) {
  std::wstring dir = FolderOf(files);
  if (dir.empty()) return false;
  std::wstring args = L"-d " + util::QuoteArg(dir + (dir.back() == L'\\' ? L"." : L""));
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  sei.lpFile = L"wt.exe";
  sei.lpParameters = args.c_str();
  sei.lpDirectory = dir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) return true;
  sei.lpFile = L"powershell.exe";
  sei.lpParameters = L"-NoExit";
  return ShellExecuteExW(&sei);
}

bool ActionMove(HWND owner, const std::vector<std::wstring>& files, std::wstring* destination) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Chuyển tới thư mục");
  dlg->SetOkButtonLabel(L"Chuyển tới đây");
  ComPtr<IShellItem> start;
  if (SUCCEEDED(SHCreateItemFromParsingName(FolderOf(files).c_str(), nullptr, IID_PPV_ARGS(&start))))
    dlg->SetFolder(start.Get());
  ForceForeground(owner);
  if (FAILED(dlg->Show(owner))) return false;
  ComPtr<IShellItem> dest;
  if (FAILED(dlg->GetResult(&dest))) return false;

  ComPtr<IFileOperation> op;
  if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return false;
  op->SetOwnerWindow(owner);
  op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
  for (auto& f : files) {
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(&item))))
      op->MoveItem(item.Get(), dest.Get(), nullptr, nullptr);
  }
  HRESULT hr = op->PerformOperations();
  BOOL aborted = FALSE;
  op->GetAnyOperationsAborted(&aborted);
  if (destination) {
    PWSTR p = nullptr;
    if (SUCCEEDED(dest->GetDisplayName(SIGDN_FILESYSPATH, &p))) { *destination = p; CoTaskMemFree(p); }
  }
  return SUCCEEDED(hr) && !aborted;
}

namespace {
// The "Share" entry of Explorer's menu is this context-menu handler (ModernSharing under
// HKCR\AllFilesystemObjects\shellex\ContextMenuHandlers), not a named verb, so it is loaded and invoked directly.
const CLSID kModernSharing = {0xe2bf9676, 0x5f8f, 0x435c, {0x97, 0xeb, 0x11, 0x60, 0x7a, 0x5b, 0xed, 0xf7}};

// Loads the share handler for the files and fills `menu` with its entry. `first` is the command id it got.
ComPtr<IContextMenu> ShareMenu(const std::vector<std::wstring>& files, HMENU menu, UINT first) {
  std::vector<PIDLIST_ABSOLUTE> pidls;
  for (auto& f : files) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &p, 0, nullptr))) pidls.push_back(p);
  }
  ComPtr<IContextMenu> cm;
  ComPtr<IShellItemArray> items;
  ComPtr<IDataObject> data;
  ComPtr<IShellExtInit> init;
  PIDLIST_ABSOLUTE folder = nullptr;
  HRESULT hr = E_FAIL;
  if (!pidls.empty() &&
      SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &items)) &&
      SUCCEEDED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) &&
      SUCCEEDED(SHParseDisplayName(util::DirOf(files[0]).c_str(), nullptr, &folder, 0, nullptr)) &&
      SUCCEEDED(CoCreateInstance(kModernSharing, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init))) &&
      SUCCEEDED(init->Initialize(folder, data.Get(), nullptr)) && SUCCEEDED(init.As(&cm)))
    hr = cm->QueryContextMenu(menu, 0, first, first + 100, CMF_NORMAL);
  if (FAILED(hr)) util::Log(L"share handler unavailable 0x%08X", (unsigned)hr);
  if (folder) CoTaskMemFree(folder);
  for (auto p : pidls) CoTaskMemFree(p);
  return SUCCEEDED(hr) && HRESULT_CODE(hr) > 0 ? cm : nullptr;
}
}  // namespace

bool ActionShare(HWND owner, const std::vector<std::wstring>& files) {
  HMENU hm = CreatePopupMenu();
  auto cm = ShareMenu(files, hm, 1);
  bool ok = false;
  if (cm) {
    ForceForeground(owner);
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
    ici.hwnd = owner;
    ici.lpVerb = MAKEINTRESOURCEA(0);  // the handler's first (only) command
    ici.lpVerbW = MAKEINTRESOURCEW(0);
    ici.nShow = SW_SHOWNORMAL;
    HRESULT hr = cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici);
    ok = SUCCEEDED(hr);
    if (!ok) util::Log(L"share failed 0x%08X", (unsigned)hr);
  }
  DestroyMenu(hm);
  return ok;
}

std::wstring ShareMenuText(const std::vector<std::wstring>& files) {
  HMENU hm = CreatePopupMenu();
  auto cm = ShareMenu(files, hm, 1);
  wchar_t text[128] = L"";
  if (cm && GetMenuItemCount(hm) > 0) GetMenuStringW(hm, 0, text, 128, MF_BYPOSITION);
  DestroyMenu(hm);
  return text;
}

namespace {
// SID_STopLevelBrowser
const GUID kTopLevelBrowser = {0x4C96BE40, 0x915C, 0x11CF, {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

std::vector<std::wstring> SelectionOf(IShellBrowser* browser, POINT* anchor) {
  std::vector<std::wstring> files;
  ComPtr<IShellView> view;
  if (FAILED(browser->QueryActiveShellView(&view))) return files;
  HWND viewHwnd = nullptr;
  view->GetWindow(&viewHwnd);
  ComPtr<IFolderView2> fv;
  ComPtr<IShellItemArray> items;
  if (FAILED(view.As(&fv)) || FAILED(fv->GetSelection(FALSE, &items)) || !items) return files;
  DWORD n = 0;
  items->GetCount(&n);
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      files.push_back(path);
      CoTaskMemFree(path);
    }
  }
  // Open the wheel under the pointer when it is over the view, else in the middle of it.
  RECT rc{};
  if (viewHwnd && GetWindowRect(viewHwnd, &rc) && anchor) {
    POINT pt;
    GetCursorPos(&pt);
    *anchor = PtInRect(&rc, pt) ? pt : POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
  }
  return files;
}
}  // namespace

std::vector<std::wstring> ExplorerSelection(HWND top, POINT* anchor) {
  std::vector<std::wstring> files;
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return files;
  wchar_t cls[64] = {};
  GetClassNameW(top, cls, 64);
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) {
    VARIANT loc{}, empty{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp &&
        SUCCEEDED(disp.As(&sp)) && SUCCEEDED(sp->QueryService(kTopLevelBrowser, IID_PPV_ARGS(&browser))))
      files = SelectionOf(browser.Get(), anchor);
    return files;
  }
  long count = 0;
  windows->get_Count(&count);
  for (long i = 0; i < count && files.empty(); ++i) {
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = i;
    ComPtr<IDispatch> disp;
    ComPtr<IWebBrowserApp> app;
    if (FAILED(windows->Item(v, &disp)) || !disp || FAILED(disp.As(&app))) continue;
    SHANDLE_PTR h = 0;
    if (FAILED(app->get_HWND(&h)) || (HWND)h != top) continue;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    if (FAILED(app.As(&sp)) || FAILED(sp->QueryService(kTopLevelBrowser, IID_PPV_ARGS(&browser)))) continue;
    // With tabs, every tab shares the window; only the visible one is the user's.
    HWND viewHwnd = nullptr;
    if (SUCCEEDED(browser->QueryActiveShellView(&view)) && SUCCEEDED(view->GetWindow(&viewHwnd)) &&
        !IsWindowVisible(viewHwnd))
      continue;
    files = SelectionOf(browser.Get(), anchor);
  }
  return files;
}

namespace {
// {ff48dba4-60ef-4201-aa87-54103eef594e}
const GUID kCUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};

// The on-screen rectangle of the selected item called `name` inside `view`, via UI Automation.
bool SelectedItemRect(HWND view, const std::wstring& name, RECT* out) {
  ComPtr<IUIAutomation> uia;
  ComPtr<IUIAutomationElement> root, item;
  ComPtr<IUIAutomationCondition> byName, bySelected, both;
  if (FAILED(CoCreateInstance(kCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia)) ||
      FAILED(uia->ElementFromHandle(view, &root)))
    return false;
  VARIANT vName{}, vSel{};
  vName.vt = VT_BSTR;
  vName.bstrVal = SysAllocString(name.c_str());
  vSel.vt = VT_BOOL;
  vSel.boolVal = VARIANT_TRUE;
  bool ok = SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, vName, &byName)) &&
            SUCCEEDED(uia->CreatePropertyCondition(UIA_SelectionItemIsSelectedPropertyId, vSel, &bySelected)) &&
            SUCCEEDED(uia->CreateAndCondition(byName.Get(), bySelected.Get(), &both)) &&
            SUCCEEDED(root->FindFirst(TreeScope_Descendants, both.Get(), &item)) && item &&
            SUCCEEDED(item->get_CurrentBoundingRectangle(out)) && out->right > out->left;
  VariantClear(&vName);
  return ok;
}
}  // namespace

bool SelectInOpenExplorer(const std::vector<std::wstring>& files, RECT* where, HWND* window) {
  if (files.empty()) return false;
  std::wstring dir = util::Lower(util::DirOf(files[0]));
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return false;
  long count = 0;
  windows->get_Count(&count);
  for (long i = 0; i < count; ++i) {
    VARIANT v{};
    v.vt = VT_I4;
    v.lVal = i;
    ComPtr<IDispatch> disp;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    ComPtr<IShellItem> folder;
    if (FAILED(windows->Item(v, &disp)) || !disp || FAILED(disp.As(&sp)) ||
        FAILED(sp->QueryService(kTopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv)) ||
        FAILED(fv->GetFolder(IID_PPV_ARGS(&folder))))
      continue;
    HWND viewHwnd = nullptr;
    view->GetWindow(&viewHwnd);
    HWND top = viewHwnd ? GetAncestor(viewHwnd, GA_ROOT) : nullptr;
    if (!viewHwnd || !IsWindowVisible(viewHwnd) || !top || IsIconic(top)) continue;
    PWSTR path = nullptr;
    if (FAILED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) continue;
    bool same = util::Lower(path) == dir;
    CoTaskMemFree(path);
    if (!same) continue;

    // Explorer only lists a new file once its change notification arrives; until then this fails.
    bool selected = false;
    for (size_t k = 0; k < files.size(); ++k) {
      PIDLIST_ABSOLUTE abs = nullptr;
      if (FAILED(SHParseDisplayName(files[k].c_str(), nullptr, &abs, 0, nullptr))) continue;
      UINT flags = SVSI_SELECT | (k == 0 ? SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED : 0);
      if (SUCCEEDED(view->SelectItem(ILFindLastID(abs), flags)) && k == 0) selected = true;
      CoTaskMemFree(abs);
    }
    if (!selected) return false;
    if (window) *window = top;
    ComPtr<IShellItem> item;
    PWSTR display = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(files[0].c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display))) {
      bool found = SelectedItemRect(viewHwnd, display, where);
      CoTaskMemFree(display);
      if (found) return true;
    }
    // Selected but not located: aim at the middle of the view.
    GetWindowRect(viewHwnd, where);
    return true;
  }
  return false;
}

void RevealInExplorer(const std::vector<std::wstring>& paths) {
  if (paths.empty()) return;
  PIDLIST_ABSOLUTE folder = nullptr;
  if (FAILED(SHParseDisplayName(util::DirOf(paths[0]).c_str(), nullptr, &folder, 0, nullptr))) return;
  std::vector<PIDLIST_ABSOLUTE> items;
  std::vector<PCUITEMID_CHILD> children;
  for (auto& p : paths) {
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &item, 0, nullptr))) {
      items.push_back(item);
      children.push_back(ILFindLastID(item));
    }
  }
  SHOpenFolderAndSelectItems(folder, (UINT)children.size(), children.data(), 0);
  for (auto i : items) CoTaskMemFree(i);
  CoTaskMemFree(folder);
}

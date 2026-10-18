#include "util.h"

#include <shlobj.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace util {

std::wstring Utf8ToWide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string WideToUtf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
  return s;
}

std::wstring Upper(std::wstring s) {
  if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
  return s;
}

std::wstring DirOf(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring FileNameOf(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring ExtOf(const std::wstring& path) {
  std::wstring name = Lower(FileNameOf(path));
  if (name.size() > 7 && name.compare(name.size() - 7, 7, L".tar.gz") == 0) return L"tar.gz";
  size_t p = name.rfind(L'.');
  if (p == std::wstring::npos || p == 0) return {};
  return name.substr(p + 1);
}

std::wstring StemOf(const std::wstring& path) {
  std::wstring name = FileNameOf(path);
  if (IsDirectory(path)) return name;
  std::wstring ext = ExtOf(path);
  if (ext.empty()) return name;
  return name.substr(0, name.size() - ext.size() - 1);
}

std::wstring UniquePath(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext) {
  for (int i = 1;; ++i) {
    std::wstring name = stem;
    if (i > 1) name += L" (" + std::to_wstring(i) + L")";
    if (!ext.empty()) name += L"." + ext;
    std::wstring full = dir + L"\\" + name;
    if (!Exists(full)) return full;
  }
}

bool IsDirectory(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

unsigned long long FileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

unsigned long long FolderSize(const std::wstring& path) {
  unsigned long long total = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) total += FolderSize(path + L"\\" + fd.cFileName);
    } else {
      total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return total;
}

bool DeleteTree(const std::wstring& path) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      std::wstring child = path + L"\\" + fd.cFileName;
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        DeleteTree(child);
      } else {
        SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child.c_str());
        else DeleteFileW(child.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return RemoveDirectoryW(path.c_str()) != 0;
}

std::wstring FormatSize(double b) {
  constexpr double KB = 1024, MB = 1024 * 1024;
  wchar_t buf[64];
  if (b < MB) {
    swprintf(buf, 64, L"%lld KB", std::max(1LL, (long long)std::llround(b / KB)));
    return buf;
  }
  double mb = b / MB;
  if (mb >= 1024) {
    swprintf(buf, 64, L"%.2f GB", mb / 1024);
  } else if (mb < 100) {
    swprintf(buf, 64, L"%.1f", mb);
    std::wstring s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, L".0") == 0) s.resize(s.size() - 2);
    for (auto& c : s) if (c == L'.') c = L',';
    return s + L" MB";
  } else {
    swprintf(buf, 64, L"%lld MB", (long long)std::llround(mb));
  }
  std::wstring s = buf;
  for (auto& c : s) if (c == L'.') c = L',';
  return s;
}

std::wstring FormatNumber(long long n) {
  std::wstring digits = std::to_wstring(n < 0 ? -n : n), out;
  int count = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (count && count % 3 == 0) out.insert(out.begin(), L'.');
    out.insert(out.begin(), *it);
    ++count;
  }
  return n < 0 ? L"-" + out : out;
}

std::wstring AppDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  return DirOf(std::wstring(buf, n));
}

std::wstring DataDir() {
  static std::wstring dir;
  if (dir.empty()) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
      dir = std::wstring(p) + L"\\VongConvert";
      CoTaskMemFree(p);
    } else {
      dir = AppDir();
    }
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

// Standard Windows command-line quoting (the rules CommandLineToArgvW and the CRT follow).
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') { ++i; ++slashes; }
    if (i == a.size()) { r.append(slashes * 2, L'\\'); break; }
    if (a[i] == L'"') { r.append(slashes * 2 + 1, L'\\'); r.push_back(L'"'); }
    else { r.append(slashes, L'\\'); r.push_back(a[i]); }
  }
  r.push_back(L'"');
  return r;
}

void Log(const wchar_t* fmt, ...) {
  static std::mutex m;
  wchar_t msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vswprintf(msg, std::size(msg), fmt, ap);
  va_end(ap);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t line[2200];
  swprintf(line, std::size(line), L"%02d:%02d:%02d.%03d %ls\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
  OutputDebugStringW(line);
  std::lock_guard<std::mutex> lock(m);
  FILE* f = _wfopen((DataDir() + L"\\log.txt").c_str(), L"ab");
  if (!f) return;
  std::string u = WideToUtf8(line);
  fwrite(u.data(), 1, u.size(), f);
  fclose(f);
}

}  // namespace util

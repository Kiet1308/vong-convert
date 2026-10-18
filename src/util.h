#pragma once
#include "common.h"
#include <string_view>

namespace util {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
std::wstring Lower(std::wstring s);
std::wstring Upper(std::wstring s);

std::wstring DirOf(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
// Lower-case extension without the dot; ".tar.gz" counts as one extension.
std::wstring ExtOf(const std::wstring& path);
// File name without its extension.
std::wstring StemOf(const std::wstring& path);
// First free "dir\stem.ext", then "dir\stem (2).ext" and so on. An empty ext gives a folder name.
std::wstring UniquePath(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext);

bool IsDirectory(const std::wstring& path);
bool Exists(const std::wstring& path);
unsigned long long FileSize(const std::wstring& path);
unsigned long long FolderSize(const std::wstring& path);
bool DeleteTree(const std::wstring& path);

// "3,1 MB", "864 KB": Vietnamese decimal comma.
std::wstring FormatSize(double bytes);
std::wstring FormatNumber(long long n);  // "4.812"

std::wstring AppDir();
std::wstring DataDir();  // %LOCALAPPDATA%\VongConvert, created on demand
std::wstring QuoteArg(const std::wstring& arg);

void Log(const wchar_t* fmt, ...);

}  // namespace util

#include "jobs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cwctype>

#include <tlhelp32.h>

#include "util.h"

namespace {

using Args = std::vector<std::wstring>;

struct Job {
  int id = 0;
  HWND notify = nullptr;
  JobRequest req;
  JobResult result;
  int lastPermille = -100;
  ULONGLONG lastPostTick = 0;
};

std::mutex g_mutex;
std::map<int, std::shared_ptr<Job>> g_jobs;
std::atomic<int> g_nextId{1};

HANDLE KillOnCloseJob() {
  static HANDLE job = [] {
    HANDLE h = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
    return h;
  }();
  return job;
}

std::wstring FindTool(const wchar_t* exe) {
  for (auto dir : {util::AppDir(), util::AppDir() + L"\\tools"}) {
    std::wstring p = dir + L"\\" + exe;
    if (util::Exists(p)) return p;
  }
  wchar_t buf[MAX_PATH * 2];
  if (SearchPathW(nullptr, exe, nullptr, (DWORD)std::size(buf), buf, nullptr)) return buf;
  return {};
}

// Runs a console tool with stdout+stderr merged, calling onLine for every line.
// Only the pipe and NUL handles are inherited, so parallel jobs never keep each
// other's pipes open. Returns the exit code, or -1 if the process could not start.
int RunProcess(const std::wstring& exe, const Args& args, const std::function<void(const std::string&)>& onLine,
               const std::vector<std::pair<std::wstring, std::wstring>>& env = {}, DWORD timeoutMs = INFINITE) {
  // Extra environment variables carry paths to scripts without any quoting issues.
  std::wstring block;
  if (!env.empty()) {
    LPWCH cur = GetEnvironmentStringsW();
    for (LPWCH p = cur; *p; p += wcslen(p) + 1) block.append(p, wcslen(p) + 1);
    FreeEnvironmentStringsW(cur);
    for (auto& [k, v] : env) block += k + L"=" + v + L'\0';
    block.push_back(L'\0');
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  HANDLE inherit[2] = {wr, nul};
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrBuf(attrSize);
  auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
  InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nul;
  si.StartupInfo.hStdOutput = wr;
  si.StartupInfo.hStdError = wr;
  si.lpAttributeList = attrs;

  std::wstring cmd = util::QuoteArg(exe);
  for (auto& a : args) cmd += L" " + util::QuoteArg(a);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                           block.empty() ? nullptr : block.data(), nullptr, &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(wr);
  CloseHandle(nul);
  if (!ok) {
    util::Log(L"CreateProcess failed (%lu): %ls", GetLastError(), cmd.c_str());
    CloseHandle(rd);
    return -1;
  }
  AssignProcessToJobObject(KillOnCloseJob(), pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  // A stuck tool (an Office dialog nobody can see, say) is ended so the job can report failure.
  std::thread watchdog;
  HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (timeoutMs != INFINITE) {
    watchdog = std::thread([&] {
      HANDLE waits[2] = {pi.hProcess, finished};
      if (WaitForMultipleObjects(2, waits, FALSE, timeoutMs) == WAIT_TIMEOUT) {
        util::Log(L"timed out after %lu ms: %ls", timeoutMs, exe.c_str());
        TerminateProcess(pi.hProcess, 124);
      }
    });
  }

  std::string line;
  char buf[4096];
  DWORD n;
  while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
    for (DWORD i = 0; i < n; ++i) {
      char c = buf[i];
      if (c == '\n' || c == '\r' || c == '\b') {  // 7-Zip redraws its progress with backspaces
        if (!line.empty()) onLine(line);
        line.clear();
      } else {
        line.push_back(c);
      }
    }
  }
  if (!line.empty()) onLine(line);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  SetEvent(finished);
  if (watchdog.joinable()) watchdog.join();
  CloseHandle(finished);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return (int)code;
}

void Post(Job& job, LPARAM value) { PostMessageW(job.notify, WM_APP_JOB, job.id, value); }

void ReportProgress(Job& job, double fraction) {
  int permille = (int)std::clamp(fraction * 1000.0, 0.0, 999.0);
  ULONGLONG now = GetTickCount64();
  if (permille - job.lastPermille < 4 && now - job.lastPostTick < 120) return;
  job.lastPermille = permille;
  job.lastPostTick = now;
  Post(job, permille);
}

// ---------------------------------------------------------------- FFmpeg

struct FFmpegRun {
  Args pre, post;  // options before -i, options before the output name
};

double ParseClock(const std::string& s) {  // "00:04:12.03"
  int h = 0, m = 0;
  double sec = 0;
  if (sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3) return h * 3600.0 + m * 60.0 + sec;
  return 0;
}

// Runs FFmpeg once per variant until one succeeds (e.g. stream copy first, then re-encode).
bool RunFFmpeg(Job& job, const std::wstring& in, const std::wstring& out, const std::vector<FFmpegRun>& variants,
               int index, int count, std::wstring& error) {
  static const std::wstring ffmpeg = FindTool(L"ffmpeg.exe");
  if (ffmpeg.empty()) {
    error = L"Không tìm thấy FFmpeg. Đặt ffmpeg.exe cạnh VongConvert.exe.";
    return false;
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    Args a = {L"-hide_banner", L"-nostdin", L"-n", L"-loglevel", L"info", L"-nostats", L"-progress", L"pipe:1"};
    a.insert(a.end(), variants[v].pre.begin(), variants[v].pre.end());
    a.push_back(L"-i");
    a.push_back(in);
    a.insert(a.end(), variants[v].post.begin(), variants[v].post.end());
    a.push_back(out);

    double duration = 0;
    std::vector<std::string> tail;
    int code = RunProcess(ffmpeg, a, [&](const std::string& line) {
      if (duration == 0) {
        auto p = line.find("Duration: ");
        if (p != std::string::npos) duration = ParseClock(line.substr(p + 10, 11));
      }
      if (line.rfind("out_time_us=", 0) == 0 && duration > 0) {
        double t = atof(line.c_str() + 12) / 1e6;
        ReportProgress(job, (index + std::clamp(t / duration, 0.0, 1.0)) / count);
      } else if (line.find('=') == std::string::npos || line.find("Error") != std::string::npos) {
        tail.push_back(line);
        if (tail.size() > 6) tail.erase(tail.begin());
      }
    });
    if (code == 0 && util::Exists(out)) return true;
    DeleteFileW(out.c_str());
    std::wstring joined;
    for (auto& t : tail) joined += util::Utf8ToWide(t) + L" | ";
    util::Log(L"ffmpeg variant %zu failed (%d) for %ls: %ls", v, code, in.c_str(), joined.c_str());
  }
  error = L"FFmpeg không xử lý được file này.";
  return false;
}

const wchar_t* kFlatten = L"format=rgba,negate,premultiply=inplace=1,negate,format=rgb24";

bool IsImage2(const std::wstring& ext) { return ext == L"jpg" || ext == L"png" || ext == L"tiff" || ext == L"bmp"; }

// Encoder options for a still image. `quality` 0 means the default for that format.
Args ImageEncode(const std::wstring& ext, int quality = 0) {
  Args a = {L"-frames:v", L"1"};
  if (IsImage2(ext)) { a.push_back(L"-update"); a.push_back(L"1"); }
  if (ext == L"jpg") { a.push_back(L"-q:v"); a.push_back(std::to_wstring(quality ? quality : 2)); }
  if (ext == L"webp") a.insert(a.end(), {L"-c:v", L"libwebp", L"-quality", std::to_wstring(quality ? quality : 88)});
  if (ext == L"avif")
    a.insert(a.end(), {L"-c:v", L"libaom-av1", L"-still-picture", L"1", L"-crf", std::to_wstring(quality ? quality : 28),
                       L"-cpu-used", L"6"});
  if (ext == L"tiff") a.insert(a.end(), {L"-compression_algo", L"lzw"});
  return a;
}

// Filter chain for an image written as `ext`: tool filter first, then flattening onto white for formats without alpha.
std::wstring ImageFilter(const std::wstring& tool, const std::wstring& ext) {
  std::wstring f = tool;
  if (ext == L"jpg" || ext == L"bmp") f += (f.empty() ? L"" : L",") + std::wstring(kFlatten);
  return f;
}

Args WithFilter(Args a, const std::wstring& filter) {
  if (!filter.empty()) { a.insert(a.begin(), filter); a.insert(a.begin(), L"-vf"); }
  return a;
}

Args AudioEncode(const std::wstring& ext) {
  if (ext == L"mp3") return {L"-c:a", L"libmp3lame", L"-q:a", L"2"};
  if (ext == L"m4a" || ext == L"aac") return {L"-c:a", L"aac", L"-b:a", L"192k"};
  if (ext == L"flac") return {L"-c:a", L"flac"};
  if (ext == L"ogg") return {L"-c:a", L"libvorbis", L"-q:a", L"5"};
  if (ext == L"opus") return {L"-c:a", L"libopus", L"-b:a", L"128k"};
  if (ext == L"wav") return {L"-c:a", L"pcm_s16le"};
  if (ext == L"aiff") return {L"-c:a", L"pcm_s16be"};
  if (ext == L"wma") return {L"-c:a", L"wmav2", L"-b:a", L"192k"};
  return {};
}

// Video re-encode options suited to the container.
Args VideoEncode(const std::wstring& ext, int crf = 20, const wchar_t* preset = L"medium", bool copyAudio = false) {
  if (ext == L"webm")
    return {L"-c:v", L"libvpx-vp9", L"-crf", std::to_wstring(crf + 12), L"-b:v", L"0", L"-row-mt", L"1",
            L"-deadline", L"good", L"-cpu-used", L"4", L"-c:a", L"libopus", L"-b:a", L"128k"};
  if (ext == L"avi") return {L"-c:v", L"mpeg4", L"-q:v", L"4", L"-c:a", L"libmp3lame", L"-q:a", L"3"};
  if (ext == L"wmv") return {L"-c:v", L"wmv2", L"-q:v", L"4", L"-c:a", L"wmav2", L"-b:a", L"192k"};
  Args a = {L"-c:v", L"libx264", L"-preset", preset, L"-crf", std::to_wstring(crf), L"-pix_fmt", L"yuv420p"};
  if (copyAudio) a.insert(a.end(), {L"-c:a", L"copy"});
  else a.insert(a.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
  if (ext == L"mp4" || ext == L"mov" || ext == L"m4v") { a.push_back(L"-movflags"); a.push_back(L"+faststart"); }
  return a;
}

bool IsMp4Family(const std::wstring& ext) { return ext == L"mp4" || ext == L"mov" || ext == L"m4v"; }

// Output extension when a tool keeps the format: HEIC cannot be written, so it becomes JPG.
std::wstring KeepExt(Kind k, const std::wstring& ext) {
  if (k == Kind::Image && (ext == L"heic" || ext == L"heif")) return L"jpg";
  if (k == Kind::Video && !(IsMp4Family(ext) || ext == L"mkv" || ext == L"webm" || ext == L"avi")) return L"mp4";
  return ext;
}

// ---------------------------------------------------------------- text files

std::wstring ReadText(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::string bytes;
  char buf[65536];
  DWORD n;
  while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) bytes.append(buf, n);
  CloseHandle(h);
  if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE)
    return std::wstring((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
  if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
  int n2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
  UINT cp = n2 > 0 || bytes.empty() ? CP_UTF8 : 1258;  // legacy Vietnamese code page as the fallback
  int len = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
  std::wstring w(len, L'\0');
  MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), w.data(), len);
  if (cp == 1258 && !w.empty()) {
    // Code page 1258 stores tone marks as separate combining characters; compose them.
    int n = NormalizeString(NormalizationC, w.c_str(), (int)w.size(), nullptr, 0);
    if (n > 0) {
      std::wstring c(n, L'\0');
      n = NormalizeString(NormalizationC, w.c_str(), (int)w.size(), c.data(), n);
      if (n > 0) { c.resize(n); w = c; }
    }
  }
  return w;
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::string u = "\xEF\xBB\xBF" + util::WideToUtf8(text);
  DWORD w;
  BOOL ok = WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
  CloseHandle(h);
  return ok;
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
  std::vector<std::wstring> lines;
  std::wstring cur;
  for (wchar_t c : text) {
    if (c == L'\n') { lines.push_back(cur); cur.clear(); }
    else if (c != L'\r') cur.push_back(c);
  }
  lines.push_back(cur);
  return lines;
}

// Shifts every "a --> b" cue time by `seconds`; works for SRT (comma) and VTT (dot).
std::wstring ShiftCues(const std::wstring& text, double seconds) {
  auto shift = [&](const std::wstring& t) -> std::wstring {
    int h = 0, m = 0, s = 0, ms = 0;
    wchar_t sep = L',';
    std::wstring v = t;
    size_t colons = std::count(v.begin(), v.end(), L':');
    if (colons == 1) v = L"00:" + v;
    if (swscanf(v.c_str(), L"%d:%d:%d%lc%d", &h, &m, &s, &sep, &ms) < 5) return t;
    long long total = ((h * 60LL + m) * 60 + s) * 1000 + ms + (long long)std::llround(seconds * 1000);
    total = std::max(0LL, total);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld%lc%03lld", total / 3600000, total / 60000 % 60, total / 1000 % 60, sep,
             total % 1000);
    return buf;
  };
  std::wstring out;
  for (auto& line : SplitLines(text)) {
    size_t arrow = line.find(L"-->");
    if (arrow != std::wstring::npos) {
      std::wstring a = line.substr(0, arrow), rest = line.substr(arrow + 3);
      auto trim = [](std::wstring s) {
        size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
        return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
      };
      std::wstring b = trim(rest), settings;
      size_t sp = b.find(L' ');
      if (sp != std::wstring::npos) { settings = b.substr(sp); b = b.substr(0, sp); }
      out += shift(trim(a)) + L" --> " + shift(b) + settings + L"\r\n";
    } else {
      out += line + L"\r\n";
    }
  }
  return out;
}

std::wstring CuesToText(const std::wstring& text) {
  std::wstring out;
  bool lastBlank = true;
  for (auto& raw : SplitLines(text)) {
    std::wstring line = raw;
    if (line.find(L"-->") != std::wstring::npos || line.rfind(L"WEBVTT", 0) == 0 || line.rfind(L"NOTE", 0) == 0)
      continue;
    if (!line.empty() && std::all_of(line.begin(), line.end(), iswdigit)) continue;
    std::wstring clean;
    bool tag = false;
    for (wchar_t c : line) {
      if (c == L'<' || c == L'{') tag = true;
      else if (c == L'>' || c == L'}') tag = false;
      else if (!tag) clean.push_back(c);
    }
    if (clean.empty()) {
      if (!lastBlank) out += L"\r\n";
      lastBlank = true;
    } else {
      out += clean + L"\r\n";
      lastBlank = false;
    }
  }
  return out;
}

// ---------------------------------------------------------------- archives
// 7-Zip handles every format and Unicode names. Without it, ZIP still works through
// .NET's ZipFile in Windows PowerShell. Windows' own tar.exe is not used: it mangles
// any name outside the ANSI code page, which means most Vietnamese file names.

std::wstring SevenZip() {
  static const std::wstring path = [] {
    for (auto name : {L"7z.exe", L"7za.exe"}) {
      std::wstring p = FindTool(name);
      if (!p.empty()) return p;
    }
    for (auto var : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
      wchar_t buf[MAX_PATH];
      if (GetEnvironmentVariableW(var, buf, MAX_PATH)) {
        std::wstring p = std::wstring(buf) + L"\\7-Zip\\7z.exe";
        if (util::Exists(p)) return p;
      }
    }
    return std::wstring();
  }();
  return path;
}

// Runs 7-Zip and maps its "NN%" output onto [from, to] of this file's share of the job.
// When `lines` is given the normal output is kept so the caller can read the summary.
bool Run7z(Job& job, Args args, int index, int count, double from = 0, double to = 1,
           std::vector<std::string>* lines = nullptr) {
  std::wstring exe = SevenZip();
  if (exe.empty()) return false;
  args.insert(args.end(), {L"-bsp1", L"-y"});
  if (!lines) args.push_back(L"-bso0");
  int code = RunProcess(exe, args, [&](const std::string& line) {
    if (lines) lines->push_back(line);
    size_t p = line.rfind('%');
    if (p == std::string::npos) return;
    size_t b = p;
    while (b > 0 && std::isdigit((unsigned char)line[b - 1])) --b;
    if (b < p) ReportProgress(job, (index + from + (to - from) * atoi(line.c_str() + b) / 100.0) / count);
  });
  if (code) util::Log(L"7z exited with %d", code);
  return code == 0;
}

std::wstring Base64(const std::string& bytes) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::wstring out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    unsigned v = (unsigned char)bytes[i] << 16 | (unsigned char)bytes[i + 1] << 8 | (unsigned char)bytes[i + 2];
    for (int s : {18, 12, 6, 0}) out.push_back(t[(v >> s) & 63]);
  }
  if (i < bytes.size()) {
    unsigned v = (unsigned char)bytes[i] << 16 | (i + 1 < bytes.size() ? (unsigned char)bytes[i + 1] << 8 : 0);
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? t[(v >> 6) & 63] : L'=');
    out.push_back(L'=');
  }
  return out;
}

// Runs a Windows PowerShell script; paths travel in environment variables, never in the script text.
bool RunPowerShell(const std::wstring& script, const std::vector<std::pair<std::wstring, std::wstring>>& env,
                   const std::function<void(const std::string&)>& onLine = nullptr, DWORD timeoutMs = 600000) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::string utf16((const char*)script.data(), script.size() * sizeof(wchar_t));
  std::vector<std::string> tail;
  int code = RunProcess(ps, {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-EncodedCommand", Base64(utf16)},
                        [&](const std::string& line) {
                          if (onLine) onLine(line);
                          tail.push_back(line);
                          if (tail.size() > 8) tail.erase(tail.begin());
                        },
                        env, timeoutMs);
  if (code) {
    std::wstring joined;
    for (auto& t : tail) joined += util::Utf8ToWide(t) + L" | ";
    util::Log(L"powershell exited with %d: %ls", code, joined.c_str());
  }
  return code == 0;
}

const wchar_t* kZipFolder = LR"(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$src = $env:VC_SRC; $root = Split-Path -Leaf $src
$zip = [IO.Compression.ZipFile]::Open($env:VC_DST, 'Create')
try {
  foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -Force) {
    $rel = $root + '/' + $f.FullName.Substring($src.Length + 1).Replace('\', '/')
    if ($f.PSIsContainer) { [void]$zip.CreateEntry($rel + '/') }
    else { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $rel, 'Optimal') }
  }
} finally { $zip.Dispose() }
)";
const wchar_t* kUnzip = LR"(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($env:VC_SRC, $env:VC_DST)
)";
const wchar_t* kZipCount = LR"(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($env:VC_SRC)
try { foreach ($e in $z.Entries) { $s = $e.Open(); $s.CopyTo([IO.Stream]::Null); $s.Dispose() }; 'ENTRIES ' + $z.Entries.Count }
finally { $z.Dispose() }
)";

// ---------------------------------------------------------------- documents
// Office does the conversions through COM automation from Windows PowerShell. Office
// apps may already be open with the user's own files, so a script only quits the app
// when nothing else is open in it.

const wchar_t* kWordScript = LR"(
$ErrorActionPreference = 'Stop'
$w = New-Object -ComObject Word.Application
try {
  $w.Visible = $false; $w.DisplayAlerts = 0
  $d = $w.Documents.Open($env:VC_SRC, $false, $true, $false)
  $fmt = [int]$env:VC_FMT
  if ($fmt -eq 2) { $d.SaveAs2($env:VC_DST, 2, $false, '', $false, '', $false, $false, $false, $false, $false, 65001) }
  else { $d.SaveAs2($env:VC_DST, $fmt) }
  $d.Close(0)
} finally {
  if ($w.Documents.Count -eq 0) { $w.Quit() }
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($w)
}
)";

const wchar_t* kPowerPointScript = LR"(
$ErrorActionPreference = 'Stop'
$p = New-Object -ComObject PowerPoint.Application
try {
  $pres = $p.Presentations.Open($env:VC_SRC, -1, 0, 0)
  $pres.SaveAs($env:VC_DST, [int]$env:VC_FMT)
  $pres.Close()
} finally {
  if ($p.Presentations.Count -eq 0) { $p.Quit() }
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($p)
}
)";

const wchar_t* kExcelScript = LR"(
$ErrorActionPreference = 'Stop'
$x = New-Object -ComObject Excel.Application
try {
  $x.Visible = $false; $x.DisplayAlerts = $false
  $wb = $x.Workbooks.Open($env:VC_SRC, 0, $true)
  if ($env:VC_FMT -eq 'pdf') { $wb.ExportAsFixedFormat(0, $env:VC_DST) } else { $wb.SaveAs($env:VC_DST, [int]$env:VC_FMT) }
  $wb.Close($false)
} finally {
  if ($x.Workbooks.Count -eq 0) { $x.Quit() }
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($x)
}
)";

// Renders every page with Windows' built-in PDF engine (Windows.Data.Pdf).
const wchar_t* kPdfPagesScript = LR"(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$m = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 }
$asOp = $m | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
$asAction = $m | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
function Await($op, [Type]$t) { $task = $asOp.MakeGenericMethod($t).Invoke($null, @($op)); [void]$task.Wait(-1); $task.Result }
function AwaitAction($op) { [void]$asAction.Invoke($null, @($op)).Wait(-1) }
[void][Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
[void][Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime]
[void][Windows.Graphics.Imaging.BitmapEncoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($env:VC_SRC)) ([Windows.Storage.StorageFile])
$doc = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)) ([Windows.Data.Pdf.PdfDocument])
New-Item -ItemType Directory -Force -Path $env:VC_DST | Out-Null
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($env:VC_DST)) ([Windows.Storage.StorageFolder])
$n = $doc.PageCount
for ($i = 0; $i -lt $n; $i++) {
  $page = $doc.GetPage($i)
  $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions
  $opts.DestinationWidth = [uint32]($page.Size.Width * 300 / 96)
  if ($env:VC_EXT -eq 'jpg') { $opts.BitmapEncoderId = [Windows.Graphics.Imaging.BitmapEncoder]::JpegEncoderId }
  else { $opts.BitmapEncoderId = [Windows.Graphics.Imaging.BitmapEncoder]::PngEncoderId }
  $name = '{0} {1:D' + ([string]$n).Length + '}.{2}'
  $out = Await ($folder.CreateFileAsync(($name -f $env:VC_NAME, ($i + 1), $env:VC_EXT), 1)) ([Windows.Storage.StorageFile])
  $stream = Await ($out.OpenAsync(1)) ([Windows.Storage.Streams.IRandomAccessStream])
  AwaitAction ($page.RenderToStreamAsync($stream, $opts))
  $stream.Dispose(); $page.Dispose()
  'PAGE ' + ($i + 1) + ' ' + $n
}
)";

std::vector<DWORD> ProcessIds(const wchar_t* exeName) {
  std::vector<DWORD> ids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return ids;
  PROCESSENTRY32W pe{sizeof(pe)};
  for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
    if (!_wcsicmp(pe.szExeFile, exeName)) ids.push_back(pe.th32ProcessID);
  CloseHandle(snap);
  return ids;
}

bool HasVisibleWindow(DWORD pid) {
  struct Ctx { DWORD pid; bool found; } ctx{pid, false};
  EnumWindows(
      [](HWND h, LPARAM l) -> BOOL {
        auto* c = (Ctx*)l;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == c->pid && IsWindowVisible(h)) { c->found = true; return FALSE; }
        return TRUE;
      },
      (LPARAM)&ctx);
  return ctx.found;
}

// After a failed automation run, ends Office processes that appeared during it and show
// no window: the copies our script started and could not quit. The user's own windows stay.
void EndOrphanedOffice(const wchar_t* exeName, const std::vector<DWORD>& before) {
  for (DWORD pid : ProcessIds(exeName)) {
    if (std::find(before.begin(), before.end(), pid) != before.end() || HasVisibleWindow(pid)) continue;
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
      TerminateProcess(h, 1);
      CloseHandle(h);
      util::Log(L"ended orphaned %ls (%lu)", exeName, pid);
    }
  }
}

// Converts with LibreOffice into a scratch folder, then moves the result to `out`.
bool RunLibreOffice(const std::wstring& in, const std::wstring& filter, const std::wstring& out, int jobId) {
  std::wstring tmp = util::UniquePath(util::DataDir(), L"lo-" + std::to_wstring(jobId), L"");
  CreateDirectoryW(tmp.c_str(), nullptr);
  int code = RunProcess(LibreOfficePath(), {L"--headless", L"--norestore", L"--convert-to", filter, L"--outdir", tmp, in},
                        [](const std::string&) {});
  bool ok = false;
  std::wstring produced = tmp + L"\\" + util::StemOf(in) + L"." + util::ExtOf(out);
  if (code == 0 && util::Exists(produced)) ok = MoveFileExW(produced.c_str(), out.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
  util::DeleteTree(tmp);
  return ok;
}

// Picks Office or LibreOffice for a document conversion. Returns false with `error` set when neither can do it.
bool ConvertDocument(Job& job, Kind k, const std::wstring& in, const std::wstring& to, const std::wstring& out,
                     std::wstring& error) {
  struct Format { const wchar_t* ext; int office; const wchar_t* lo; };
  static const Format word[] = {{L"pdf", 17, L"pdf"}, {L"docx", 16, L"docx:MS Word 2007 XML"}, {L"odt", 23, L"odt"},
                                {L"rtf", 6, L"rtf"},  {L"txt", 2, L"txt:Text (encoded):UTF8"}, {L"html", 10, L"html"}};
  static const Format slides[] = {{L"pdf", 32, L"pdf"}, {L"pptx", 24, L"pptx"}, {L"jpg", 17, nullptr}, {L"png", 18, nullptr}};
  static const Format sheet[] = {{L"pdf", -1, L"pdf"}, {L"xlsx", 51, L"xlsx"}, {L"csv", 62, L"csv:Text - txt - csv (StarCalc):44,34,76"},
                                 {L"ods", 60, L"ods"}};
  const Format* f = nullptr;
  auto find = [&](const Format* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (to == list[i].ext) f = &list[i];
  };
  OfficeApp app = OfficeApp::Word;
  const wchar_t* script = kWordScript;
  if (k == Kind::Doc) find(word, std::size(word));
  if (k == Kind::Slides) { find(slides, std::size(slides)); app = OfficeApp::PowerPoint; script = kPowerPointScript; }
  if (k == Kind::Sheet) { find(sheet, std::size(sheet)); app = OfficeApp::Excel; script = kExcelScript; }
  if (!f) { error = L"Chưa hỗ trợ chuyển sang định dạng này."; return false; }
  bool ok = false;
  if (HasOfficeApp(app)) {
    static const wchar_t* exes[3] = {L"WINWORD.EXE", L"POWERPNT.EXE", L"EXCEL.EXE"};
    auto before = ProcessIds(exes[(int)app]);
    std::wstring fmt = f->office < 0 ? L"pdf" : std::to_wstring(f->office);
    ok = RunPowerShell(script, {{L"VC_SRC", in}, {L"VC_DST", out}, {L"VC_FMT", fmt}}, nullptr, 180000) &&
         util::Exists(out);
    if (!ok) {
      EndOrphanedOffice(exes[(int)app], before);
      error = L"Office không mở hoặc không lưu được file này.";
    }
  } else if (HasLibreOffice() && f->lo) {
    ok = RunLibreOffice(in, f->lo, out, job.id);
    if (!ok) error = L"LibreOffice không chuyển được file này.";
  } else {
    error = L"Cần Microsoft Office hoặc LibreOffice.";
  }
  return ok;
}

// ---------------------------------------------------------------- images to PDF

bool JpegInfo(const std::string& d, int& w, int& h, int& comps) {
  size_t i = 2;
  while (i + 9 < d.size()) {
    if ((unsigned char)d[i] != 0xFF) { ++i; continue; }
    unsigned char m = d[i + 1];
    if (m == 0xD8 || m == 0x01 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) { ++i; continue; }
    size_t len = ((unsigned char)d[i + 2] << 8) | (unsigned char)d[i + 3];
    bool sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
    if (sof) {
      h = ((unsigned char)d[i + 5] << 8) | (unsigned char)d[i + 6];
      w = ((unsigned char)d[i + 7] << 8) | (unsigned char)d[i + 8];
      comps = (unsigned char)d[i + 9];
      return w > 0 && h > 0;
    }
    i += 2 + len;
  }
  return false;
}

std::string ReadAll(const std::wstring& path) {
  std::string data;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return data;
  char buf[65536];
  DWORD n;
  while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) data.append(buf, n);
  CloseHandle(h);
  return data;
}

// One page per image, long side 842 pt (A4), the picture filling the page.
bool ImagesToPdf(Job& job, const std::vector<std::wstring>& images, const std::wstring& out, std::wstring& error) {
  struct Page { std::string jpeg; int w, h, comps; };
  std::vector<Page> pages;
  for (size_t i = 0; i < images.size(); ++i) {
    std::wstring tmp = util::UniquePath(util::DataDir(), L"pdf-" + std::to_wstring(job.id) + L"-" + std::to_wstring(i), L"jpg");
    std::wstring err;
    // Re-encoding through FFmpeg applies EXIF rotation and flattens transparency onto white.
    bool ok = RunFFmpeg(job, images[i], tmp, {{{}, WithFilter(ImageEncode(L"jpg", 2), ImageFilter(L"", L"jpg"))}}, (int)i,
                        (int)images.size(), err);
    Page p{};
    if (ok) p.jpeg = ReadAll(tmp);
    DeleteFileW(tmp.c_str());
    if (!ok || !JpegInfo(p.jpeg, p.w, p.h, p.comps)) continue;
    pages.push_back(std::move(p));
    ReportProgress(job, (i + 1.0) / images.size());
  }
  if (pages.empty()) { error = L"Không đọc được ảnh nào."; return false; }

  std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offsets(3 + pages.size() * 3, 0);
  auto object = [&](size_t num, const std::string& body) {
    offsets[num] = pdf.size();
    pdf += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
  };
  object(1, "<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (size_t i = 0; i < pages.size(); ++i) kids += std::to_string(3 + i * 3) + " 0 R ";
  object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>");
  for (size_t i = 0; i < pages.size(); ++i) {
    const Page& p = pages[i];
    double scale = 842.0 / std::max(p.w, p.h);
    char size[64];
    snprintf(size, sizeof(size), "%.2f 0 0 %.2f", p.w * scale, p.h * scale);
    char box[64];
    snprintf(box, sizeof(box), "[0 0 %.2f %.2f]", p.w * scale, p.h * scale);
    size_t page = 3 + i * 3, content = page + 1, image = page + 2;
    object(page, "<< /Type /Page /Parent 2 0 R /MediaBox " + std::string(box) + " /Resources << /XObject << /Im0 " +
                     std::to_string(image) + " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    std::string ops = "q " + std::string(size) + " 0 0 cm /Im0 Do Q";
    object(content, "<< /Length " + std::to_string(ops.size()) + " >>\nstream\n" + ops + "\nendstream");
    const char* cs = p.comps == 1 ? "/DeviceGray" : p.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB";
    object(image, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(p.w) + " /Height " + std::to_string(p.h) +
                      " /ColorSpace " + cs + " /BitsPerComponent 8 /Filter /DCTDecode /Length " +
                      std::to_string(p.jpeg.size()) + " >>\nstream\n" + p.jpeg + "\nendstream");
  }
  size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(offsets.size()) + "\n0000000000 65535 f \n";
  for (size_t i = 1; i < offsets.size(); ++i) {
    char line[32];
    snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
    pdf += line;
  }
  pdf += "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
         "\n%%EOF\n";
  HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) { error = L"Không ghi được file PDF."; return false; }
  DWORD written;
  BOOL ok = WriteFile(h, pdf.data(), (DWORD)pdf.size(), &written, nullptr);
  CloseHandle(h);
  if (!ok) { DeleteFileW(out.c_str()); error = L"Không ghi được file PDF."; }
  return ok;
}

// "x:y:w:h" in pixels of the decoded (already rotated) frame -> FFmpeg crop filter.
// Video sizes are kept even, which H.264 requires.
std::wstring CropFilter(const std::wstring& spec, bool even) {
  int x = 0, y = 0, w = 0, h = 0;
  if (swscanf(spec.c_str(), L"%d:%d:%d:%d", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) return L"null";
  if (even) { w &= ~1; h &= ~1; x &= ~1; y &= ~1; }
  return L"crop=" + std::to_wstring(std::max(w, 2)) + L":" + std::to_wstring(std::max(h, 2)) + L":" + std::to_wstring(x) +
         L":" + std::to_wstring(y);
}

// ---------------------------------------------------------------- one file

struct Outcome {
  bool ok = false;
  std::wstring output, error, note;
};

Outcome RunOne(Job& job, const std::wstring& in, int index, int count) {
  const auto& req = job.req;
  Outcome o;
  Kind k = KindOf(in);
  std::wstring ext = NormalExt(in), dir = util::DirOf(in), stem = util::StemOf(in);
  auto unique = [&](const std::wstring& suffix, const std::wstring& outExt) {
    return util::UniquePath(dir, suffix.empty() ? stem : stem + L" (" + suffix + L")", outExt);
  };
  auto ff = [&](const std::wstring& out, std::vector<FFmpegRun> variants) {
    o.output = out;
    o.ok = RunFFmpeg(job, in, out, variants, index, count, o.error);
  };

  if (req.mode == Mode::Convert) {
    const std::wstring& to = req.itemId;
    if (k == Kind::Image && to == L"mp4") {
      ff(unique(L"", L"mp4"), {{{}, {L"-vf", L"scale=trunc(iw/2)*2:trunc(ih/2)*2", L"-c:v", L"libx264", L"-crf", L"20",
                                     L"-pix_fmt", L"yuv420p", L"-movflags", L"+faststart"}}});
    } else if (k == Kind::Image) {
      ff(unique(L"", to), {{{}, WithFilter(ImageEncode(to), ImageFilter(L"", to))}});
    } else if (k == Kind::Video && to == L"gif") {
      ff(unique(L"", L"gif"),
         {{{}, {L"-t", L"10", L"-vf",
                L"fps=12,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse", L"-loop", L"0"}}});
    } else if (k == Kind::Video && (to == L"mp3" || to == L"m4a")) {
      std::vector<FFmpegRun> v;
      if (to == L"m4a") v.push_back({{}, {L"-vn", L"-c:a", L"copy"}});
      Args enc = {L"-vn"};
      auto a = AudioEncode(to);
      enc.insert(enc.end(), a.begin(), a.end());
      v.push_back({{}, enc});
      ff(unique(L"", to), v);
    } else if (k == Kind::Video) {
      std::vector<FFmpegRun> v;
      if (to == L"mkv") v.push_back({{}, {L"-map", L"0", L"-c", L"copy"}});
      if (IsMp4Family(to))
        v.push_back({{}, {L"-map", L"0:v?", L"-map", L"0:a?", L"-c", L"copy", L"-movflags", L"+faststart"}});
      Args enc = {L"-map", L"0:v:0?", L"-map", L"0:a?"};
      auto e = VideoEncode(to);
      enc.insert(enc.end(), e.begin(), e.end());
      v.push_back({{}, enc});
      ff(unique(L"", to), v);
    } else if (k == Kind::Audio) {
      Args enc = {L"-vn"};
      auto a = AudioEncode(to);
      enc.insert(enc.end(), a.begin(), a.end());
      ff(unique(L"", to), {{{}, enc}});
    } else if (k == Kind::Sub && to == L"txt") {
      o.output = unique(L"", L"txt");
      o.ok = WriteUtf8(o.output, CuesToText(ReadText(in)));
    } else if (k == Kind::Sub) {
      ff(unique(L"", to), {{{L"-sub_charenc", L"UTF-8"}, {}}, {{}, {}}});
    } else if (k == Kind::Pdf && (to == L"jpg" || to == L"png")) {
      o.output = unique(L"ảnh " + util::Upper(to), L"");
      o.ok = RunPowerShell(kPdfPagesScript, {{L"VC_SRC", in}, {L"VC_DST", o.output}, {L"VC_EXT", to}, {L"VC_NAME", stem}},
                           [&](const std::string& line) {
                             int page = 0, pages = 0;
                             if (sscanf(line.c_str(), "PAGE %d %d", &page, &pages) == 2 && pages > 0)
                               ReportProgress(job, (index + (double)page / pages) / count);
                           });
      if (!o.ok) {
        if (util::IsDirectory(o.output)) util::DeleteTree(o.output);
        o.error = L"Không đọc được file PDF này.";
      }
    } else if (k == Kind::Pdf || k == Kind::Doc || k == Kind::Sheet || k == Kind::Slides) {
      bool folder = k == Kind::Slides && (to == L"jpg" || to == L"png");
      o.output = folder ? unique(L"ảnh " + util::Upper(to), L"") : unique(L"", to);
      o.ok = ConvertDocument(job, k, in, to, o.output, o.error);
      if (!o.ok && folder && util::IsDirectory(o.output)) util::DeleteTree(o.output);
    } else if (k == Kind::Archive) {
      if (SevenZip().empty()) {
        o.error = L"Cần 7-Zip để đổi định dạng file nén.";
        return o;
      }
      std::wstring tmp = util::UniquePath(util::DataDir(), L"tmp-" + std::to_wstring(job.id), L"");
      CreateDirectoryW(tmp.c_str(), nullptr);
      o.output = unique(L"", to);
      bool ok = Run7z(job, {L"x", in, L"-o" + tmp}, index, count, 0, .5);
      if (ok && to == L"tar.gz") {
        // gzip holds a single file: build the tar first, named like the archive so it unpacks sensibly.
        std::wstring tarPath = util::UniquePath(util::DataDir(), stem, L"tar");
        ok = Run7z(job, {L"a", L"-ttar", tarPath, tmp + L"\\*"}, index, count, .5, .75) &&
             Run7z(job, {L"a", L"-tgzip", o.output, tarPath}, index, count, .75, 1);
        DeleteFileW(tarPath.c_str());
      } else if (ok) {
        std::wstring type = to == L"7z" ? L"-t7z" : to == L"tar" ? L"-ttar" : L"-tzip";
        Args a = {L"a", type, o.output, tmp + L"\\*"};
        if (to == L"zip") a.push_back(L"-mcu=on");
        ok = Run7z(job, a, index, count, .5, 1);
      }
      util::DeleteTree(tmp);
      o.ok = ok;
      if (!ok) { DeleteFileW(o.output.c_str()); o.error = L"Không đọc hoặc ghi được file nén."; }
    } else {
      o.error = L"Chưa hỗ trợ loại file này.";
    }
    return o;
  }

  // ---- tools
  const std::wstring& id = req.itemId;
  const std::wstring& sub = req.subId;
  if (k == Kind::Image) {
    std::wstring keep = KeepExt(k, ext);
    if (id == L"compress") {
      std::wstring outExt = (ext == L"webp" || ext == L"avif") ? ext : L"jpg";
      int lvl = sub == L"light" ? 0 : sub == L"medium" ? 1 : 2;
      static const int jpgQ[] = {4, 8, 14}, webpQ[] = {80, 65, 45}, avifQ[] = {32, 38, 44};
      int q = outExt == L"jpg" ? jpgQ[lvl] : outExt == L"webp" ? webpQ[lvl] : avifQ[lvl];
      ff(unique(L"nén", outExt), {{{}, WithFilter(ImageEncode(outExt, q), ImageFilter(L"", outExt))}});
    } else {
      std::wstring filter, suffix;
      if (id == L"resize") {
        if (sub == L"half") { filter = L"scale=trunc(iw/2):trunc(ih/2)"; suffix = L"50%"; }
        else {
          std::wstring n = sub == L"1920" ? L"1920" : L"1080";
          filter = L"scale='if(gte(iw,ih),min(" + n + L",iw),-2)':'if(gte(iw,ih),-2,min(" + n + L",ih))'";
          suffix = n + L"px";
        }
      } else if (id == L"rotate") {
        filter = sub == L"ccw" ? L"transpose=2" : sub == L"180" ? L"hflip,vflip" : L"transpose=1";
        suffix = L"xoay";
      } else if (id == L"crop") {
        filter = CropFilter(sub, false);
        suffix = L"cắt";
      } else if (id == L"flip") {
        filter = L"hflip";
        suffix = L"lật";
      }
      ff(unique(suffix, keep), {{{}, WithFilter(ImageEncode(keep), ImageFilter(filter, keep))}});
    }
  } else if (k == Kind::Video) {
    std::wstring keep = KeepExt(k, ext);
    if (id == L"compress") {
      int crf = sub == L"light" ? 23 : sub == L"medium" ? 28 : 32;
      Args a = {L"-map", L"0:v:0?", L"-map", L"0:a?"};
      auto e = VideoEncode(L"mp4", crf, L"veryfast");
      a.insert(a.end(), e.begin(), e.end());
      ff(unique(L"nén", L"mp4"), {{{}, a}});
    } else if (id == L"audio") {
      ff(unique(L"", L"m4a"), {{{}, {L"-vn", L"-c:a", L"copy"}}, {{}, {L"-vn", L"-c:a", L"aac", L"-b:a", L"192k"}}});
    } else if (id == L"mute") {
      Args reencode = {L"-an"};
      auto e = VideoEncode(keep);
      reencode.insert(reencode.end(), e.begin(), e.end());
      ff(unique(L"không tiếng", keep), {{{}, {L"-an", L"-c:v", L"copy"}}, {{}, reencode}});
    } else if (id == L"resize") {
      std::wstring h = sub.empty() ? L"720" : sub;
      Args a = {L"-vf", L"scale=-2:'min(" + h + L",ih)'"}, copyAudio = a;
      auto e = VideoEncode(keep), ec = VideoEncode(keep, 20, L"medium", true);
      a.insert(a.end(), e.begin(), e.end());
      copyAudio.insert(copyAudio.end(), ec.begin(), ec.end());
      ff(unique(h + L"p", keep), {{{}, copyAudio}, {{}, a}});
    } else if (id == L"crop") {
      Args a = {L"-vf", CropFilter(sub, true)}, copyAudio = a;
      auto e = VideoEncode(keep, 18), ec = VideoEncode(keep, 18, L"medium", true);
      a.insert(a.end(), e.begin(), e.end());
      copyAudio.insert(copyAudio.end(), ec.begin(), ec.end());
      ff(unique(L"cắt", keep), {{{}, copyAudio}, {{}, a}});
    } else if (id == L"frame") {
      ff(unique(L"khung hình", L"png"), {{{}, ImageEncode(L"png")}});
    } else if (id == L"rotate") {
      std::wstring deg = sub == L"ccw" ? L"90" : sub == L"180" ? L"180" : L"-90";
      std::wstring tr = sub == L"ccw" ? L"transpose=2" : sub == L"180" ? L"hflip,vflip" : L"transpose=1";
      Args reencode = {L"-vf", tr};
      auto e = VideoEncode(keep);
      reencode.insert(reencode.end(), e.begin(), e.end());
      std::vector<FFmpegRun> v;
      if (IsMp4Family(keep)) v.push_back({{L"-display_rotation", deg}, {L"-c", L"copy", L"-movflags", L"+faststart"}});
      v.push_back({{}, reencode});
      ff(unique(L"xoay", keep), v);
    }
  } else if (k == Kind::Audio) {
    if (id == L"compress") {
      std::wstring br = (sub.empty() ? L"192" : sub) + L"k";
      ff(unique(ext == L"mp3" ? L"nén" : L"", L"mp3"), {{{}, {L"-vn", L"-c:a", L"libmp3lame", L"-b:a", br}}});
    } else if (id == L"normalize") {
      Args a = {L"-vn", L"-af", L"loudnorm=I=-16:TP=-1.5:LRA=11", L"-ar", L"48000"};
      auto e = AudioEncode(ext);
      a.insert(a.end(), e.begin(), e.end());
      ff(unique(L"chuẩn hoá", ext), {{{}, a}});
    } else if (id == L"speed") {
      std::wstring f = sub.empty() ? L"1.25" : sub;
      Args a = {L"-vn", L"-af", L"atempo=" + f};
      auto e = AudioEncode(ext);
      a.insert(a.end(), e.begin(), e.end());
      std::wstring label = f;
      for (auto& c : label) if (c == L'.') c = L',';
      ff(unique(label + L"x", ext), {{{}, a}});
    }
  } else if (k == Kind::Archive) {
    bool have7z = !SevenZip().empty();
    if (!have7z && ext != L"zip") {
      o.error = L"Cần 7-Zip để mở loại file nén này.";
      return o;
    }
    if (id == L"extract") {
      o.output = util::UniquePath(dir, stem, L"");
      if (have7z) {
        CreateDirectoryW(o.output.c_str(), nullptr);
        o.ok = Run7z(job, {L"x", in, L"-o" + o.output}, index, count);
      } else {
        o.ok = RunPowerShell(kUnzip, {{L"VC_SRC", in}, {L"VC_DST", o.output}});
      }
      if (!o.ok) {
        if (util::IsDirectory(o.output)) util::DeleteTree(o.output);
        o.error = L"Không giải nén được file này.";
      }
    } else if (id == L"test") {
      std::vector<std::string> lines;
      long long files = -1, folders = 0;
      if (have7z) {
        o.ok = Run7z(job, {L"t", in}, index, count, 0, 1, &lines);
        for (auto& l : lines) {
          if (l.rfind("Files: ", 0) == 0) files = atoll(l.c_str() + 7);
          if (l.rfind("Folders: ", 0) == 0) folders = atoll(l.c_str() + 9);
        }
      } else {
        o.ok = RunPowerShell(kZipCount, {{L"VC_SRC", in}}, [&](const std::string& l) { lines.push_back(l); });
        for (auto& l : lines)
          if (l.rfind("ENTRIES ", 0) == 0) files = atoll(l.c_str() + 8);
      }
      if (o.ok) o.note = files >= 0 ? util::FormatNumber(files + folders) + L" mục, không có lỗi" : L"Không có lỗi";
      else o.error = L"File nén bị lỗi hoặc không đọc được.";
    }
  } else if (k == Kind::Sub) {
    std::wstring text = ReadText(in);
    if (id == L"shift") {
      double sec = sub.empty() ? 0.5 : _wtof(sub.c_str());
      std::wstring label = sub.empty() ? L"+0.5" : sub;
      for (auto& c : label) if (c == L'.') c = L',';
      o.output = unique(label + L"s", ext);
      o.ok = WriteUtf8(o.output, ShiftCues(text, sec));
    } else if (id == L"utf8") {
      o.output = unique(L"UTF-8", ext);
      o.ok = WriteUtf8(o.output, text);
    }
    if (!o.ok) o.error = L"Không ghi được file phụ đề.";
  } else if (k == Kind::Folder) {
    if (id == L"zip") {
      std::wstring name = util::FileNameOf(in);
      o.output = util::UniquePath(util::DirOf(in), name, L"zip");
      if (!SevenZip().empty()) o.ok = Run7z(job, {L"a", L"-tzip", L"-mcu=on", o.output, in}, index, count);
      else o.ok = RunPowerShell(kZipFolder, {{L"VC_SRC", in}, {L"VC_DST", o.output}});
      if (!o.ok) { DeleteFileW(o.output.c_str()); o.error = L"Không nén được thư mục."; }
    }
  }
  if (!o.ok && o.error.empty()) o.error = L"Không làm được việc này.";
  return o;
}

void RunJob(std::shared_ptr<Job> job) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto targets = TargetsFor(job->req.mode, job->req.files);
  int count = (int)targets.size();
  Kind k = count ? KindOf(targets[0]) : Kind::Other;
  bool toPdf = job->req.mode == Mode::Convert && k == Kind::Image && job->req.itemId == L"pdf";
  bool office = k == Kind::Doc || k == Kind::Slides || k == Kind::Sheet || (k == Kind::Pdf && job->req.itemId != L"jpg" &&
                                                                              job->req.itemId != L"png");
  bool indeterminate = (k == Kind::Image && !toPdf) || k == Kind::Sub || office ||
                       ((k == Kind::Archive || k == Kind::Folder) && SevenZip().empty());
  if (indeterminate) Post(*job, kJobIndeterminate);

  int okCount = 0;
  std::wstring firstError, note;
  if (toPdf) {
    // Several pictures dropped together become one PDF, one page each.
    std::wstring stem = util::StemOf(targets[0]);
    if (count > 1) stem += L" (" + std::to_wstring(count) + L" trang)";
    std::wstring out = util::UniquePath(util::DirOf(targets[0]), stem, L"pdf"), error;
    if (ImagesToPdf(*job, targets, out, error)) {
      okCount = count;
      job->result.outputs.push_back(out);
    } else {
      firstError = error;
    }
    count = 0;  // skip the per-file loop
  }
  for (int i = 0; i < count; ++i) {
    Outcome o = RunOne(*job, targets[i], i, count);
    if (o.ok) {
      ++okCount;
      if (!o.output.empty()) job->result.outputs.push_back(o.output);
      if (!o.note.empty()) note = o.note;
    } else if (firstError.empty()) {
      firstError = o.error;
    }
    if (!indeterminate) ReportProgress(*job, (i + 1.0) / count);
  }

  auto& r = job->result;
  r.ok = okCount > 0;
  auto items = ItemsFor(job->req.mode, job->req.files);
  std::wstring label;
  for (auto& it : items)
    if (it.id == job->req.itemId) {
      label = it.label;
      for (auto& s : it.subs)
        if (s.id == job->req.subId) label += L" " + util::Lower(s.label);
    }
  if (r.ok) {
    if (job->req.mode == Mode::Convert)
      r.title = count > 1 ? L"Đã chuyển " + std::to_wstring(okCount) + L" file sang " + label : L"Đã chuyển sang " + label;
    else
      r.title = L"Xong · " + label;
    if (!r.outputs.empty()) {
      const auto& first = r.outputs[0];
      r.detail = util::FileNameOf(first);
      if (!util::IsDirectory(first)) r.detail += L" · " + util::FormatSize((double)util::FileSize(first));
      if (r.outputs.size() > 1) r.detail += L" và " + std::to_wstring(r.outputs.size() - 1) + L" file khác";
    }
    if (!note.empty()) r.detail = note;
    if (okCount < count) r.detail += L" · " + std::to_wstring(count - okCount) + L" file lỗi";
  } else {
    r.title = count > 1 ? L"Không xử lý được " + std::to_wstring(count) + L" file" : L"Không xử lý được " + util::FileNameOf(targets.empty() ? std::wstring() : targets[0]);
    r.detail = firstError;
  }
  util::Log(L"job %d %ls: %ls / %ls", job->id, r.ok ? L"ok" : L"failed", r.title.c_str(), r.detail.c_str());
  Post(*job, r.ok ? kJobDone : kJobFailed);
  if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace

bool MakePreview(const std::wstring& in, const std::wstring& outPng, int boxW, int boxH, int* fullW, int* fullH) {
  std::wstring ffmpeg = FindTool(L"ffmpeg.exe");
  if (ffmpeg.empty()) return false;
  std::wstring bw = std::to_wstring(boxW), bh = std::to_wstring(boxH);
  // Only as many pixels as the screen will show, written without compression: it is read back at once.
  Args a = {L"-hide_banner", L"-nostdin", L"-y", L"-loglevel", L"verbose", L"-i", in, L"-frames:v", L"1", L"-vf",
            L"scale='min(" + bw + L",iw)':'min(" + bh + L",ih)':force_original_aspect_ratio=decrease",
            L"-compression_level", L"0", L"-update", L"1", outPng};
  *fullW = *fullH = 0;
  int code = RunProcess(ffmpeg, a, [&](const std::string& line) {
    // "[Parsed_scale_0 @ ...] w:4032 h:3024 fmt:... -> w:2048 h:1536 ..." gives the size after auto-rotation.
    if (*fullW || line.find("Parsed_scale") == std::string::npos) return;
    size_t arrow = line.find(" -> w:");
    size_t w = line.find("] w:");
    if (arrow == std::string::npos || w == std::string::npos || w > arrow) return;
    sscanf(line.c_str() + w + 4, "%d h:%d", fullW, fullH);
  });
  return code == 0 && util::Exists(outPng) && *fullW > 0;
}

int StartJob(HWND notify, const JobRequest& request) {
  if (TargetsFor(request.mode, request.files).empty()) return 0;
  auto job = std::make_shared<Job>();
  job->id = g_nextId++;
  job->notify = notify;
  job->req = request;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs[job->id] = job;
  }
  util::Log(L"job %d start: mode %d item %ls sub %ls, %zu file(s)", job->id, (int)request.mode, request.itemId.c_str(),
            request.subId.c_str(), request.files.size());
  std::thread([job] { RunJob(job); }).detach();
  return job->id;
}

JobResult TakeJobResult(int id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return {};
  JobResult r = it->second->result;
  g_jobs.erase(it);
  return r;
}

void ShutdownJobs() { CloseHandle(KillOnCloseJob()); }

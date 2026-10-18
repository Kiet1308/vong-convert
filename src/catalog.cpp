#include "catalog.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "settings.h"
#include "util.h"

namespace icons {
const char* compress = "M4 14h6v6M20 10h-6V4M14 10l7-7M3 21l7-7";
const char* resize = "M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7";
const char* rotate = "M3 12a9 9 0 1 0 2.6-6.4L3 8M3 3v5h5";
const char* crop = "M6 2v14a2 2 0 0 0 2 2h14M18 22V8a2 2 0 0 0-2-2H2";
const char* flip = "M12 3v18M8 7L3 17h5zM16 7l5 10h-5z";
const char* scissors = "M3 6a3 3 0 1 0 6 0a3 3 0 1 0-6 0M3 18a3 3 0 1 0 6 0a3 3 0 1 0-6 0M20 4L8.1 15.9M14.5 14.5L20 20M8.1 8.1L12 12";
const char* music = "M9 18V5l12-2v13M3 18a3 3 0 1 0 6 0a3 3 0 1 0-6 0M15 16a3 3 0 1 0 6 0a3 3 0 1 0-6 0";
const char* mute = "M11 5L6 9H2v6h4l5 4zM23 9l-6 6M17 9l6 6";
const char* camera = "M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2zM8 13a4 4 0 1 0 8 0a4 4 0 1 0-8 0";
const char* bars = "M4 10v4M8 6v12M12 9v6M16 4v16M20 10v4";
const char* gauge = "M12 14l4-4M3.3 19a10 10 0 1 1 17.4 0";
const char* unbox = "M21 8v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V8M1 3h22v5H1zM10 12h4";
const char* check = "M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0M8 12l3 3 5-6";
const char* zip = "M21 8v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V8M1 3h22v5H1zM12 11v6";
const char* clock = "M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0M12 6v6l4 2";
const char* code = "M8 8l-4 4 4 4M16 8l4 4-4 4M14 4l-4 16";
const char* share = "M4 12v7a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-7M16 6l-4-4-4 4M12 2v13";
const char* pin = "M12 17v5M9 3h6l-1 6 4 4v2H6v-2l4-4z";
const char* copy = "M11 9h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-8a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2zM5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1";
const char* move = "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM9 13h7M13 10l3 3-3 3";
const char* link = "M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7";
const char* term = "M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zM6 9l4 3-4 3M12 15h6";
}  // namespace icons

namespace {

const std::map<std::wstring, Kind>& KindTable() {
  static const std::map<std::wstring, Kind> t = {
      {L"jpg", Kind::Image},   {L"png", Kind::Image},    {L"webp", Kind::Image},   {L"avif", Kind::Image},
      {L"heic", Kind::Image},  {L"heif", Kind::Image},   {L"tiff", Kind::Image},   {L"bmp", Kind::Image},
      {L"gif", Kind::Image},   {L"mp4", Kind::Video},    {L"mov", Kind::Video},    {L"mkv", Kind::Video},
      {L"webm", Kind::Video},  {L"avi", Kind::Video},    {L"wmv", Kind::Video},    {L"m4v", Kind::Video},
      {L"3gp", Kind::Video},   {L"flv", Kind::Video},    {L"mts", Kind::Video},    {L"m2ts", Kind::Video},
      {L"ts", Kind::Video},    {L"mpg", Kind::Video},    {L"mpeg", Kind::Video},   {L"mp3", Kind::Audio},
      {L"m4a", Kind::Audio},   {L"wav", Kind::Audio},    {L"flac", Kind::Audio},   {L"ogg", Kind::Audio},
      {L"opus", Kind::Audio},  {L"aac", Kind::Audio},    {L"aiff", Kind::Audio},   {L"wma", Kind::Audio},
      {L"amr", Kind::Audio},   {L"pdf", Kind::Pdf},      {L"doc", Kind::Doc},      {L"docx", Kind::Doc},
      {L"odt", Kind::Doc},     {L"rtf", Kind::Doc},      {L"txt", Kind::Doc},      {L"md", Kind::Doc},
      {L"html", Kind::Doc},    {L"epub", Kind::Doc},     {L"ppt", Kind::Slides},   {L"pptx", Kind::Slides},
      {L"odp", Kind::Slides},  {L"xls", Kind::Sheet},    {L"xlsx", Kind::Sheet},   {L"ods", Kind::Sheet},
      {L"csv", Kind::Sheet},   {L"tsv", Kind::Sheet},    {L"zip", Kind::Archive},  {L"7z", Kind::Archive},
      {L"rar", Kind::Archive}, {L"tar", Kind::Archive},  {L"tar.gz", Kind::Archive}, {L"srt", Kind::Sub},
      {L"vtt", Kind::Sub},     {L"ass", Kind::Sub},      {L"ssa", Kind::Sub},
  };
  return t;
}

const std::map<std::wstring, std::wstring>& Labels() {
  static const std::map<std::wstring, std::wstring> t = {
      {L"webp", L"WebP"}, {L"webm", L"WebM"}, {L"opus", L"Opus"}, {L"tar.gz", L"TAR.GZ"}};
  return t;
}

std::wstring LabelFor(const std::wstring& ext) {
  auto it = Labels().find(ext);
  return it != Labels().end() ? it->second : util::Upper(ext);
}

bool IsWordInput(const std::wstring& ext) {
  return ext == L"doc" || ext == L"docx" || ext == L"odt" || ext == L"rtf" || ext == L"txt" || ext == L"html";
}

std::vector<std::wstring> ConvertTargets(Kind k, const std::wstring& ext) {
  std::vector<std::wstring> list;
  switch (k) {
    case Kind::Image: list = {L"jpg", L"png", L"webp", L"avif", L"tiff", L"bmp", L"gif", L"pdf"}; break;
    case Kind::Video: list = {L"mp4", L"mov", L"mkv", L"webm", L"avi", L"gif", L"mp3", L"m4a"}; break;
    case Kind::Audio: list = {L"mp3", L"m4a", L"flac", L"ogg", L"opus", L"wav", L"aiff", L"wma"}; break;
    case Kind::Sub: list = {L"srt", L"vtt", L"txt"}; break;
    case Kind::Archive: list = {L"zip", L"7z", L"tar", L"tar.gz"}; break;
    case Kind::Pdf:
      // Rendered by Windows' own PDF engine. Word's PDF import hangs when it runs unattended,
      // so PDF to Word is left out.
      list = {L"jpg", L"png"};
      break;
    case Kind::Doc:
      if (IsWordInput(ext) && (HasOfficeApp(OfficeApp::Word) || HasLibreOffice()))
        list = {L"pdf", L"docx", L"odt", L"rtf", L"txt", L"html"};
      break;
    case Kind::Slides:
      if (HasOfficeApp(OfficeApp::PowerPoint)) list = {L"pdf", L"pptx", L"jpg", L"png"};
      else if (HasLibreOffice()) list = {L"pdf", L"pptx"};
      break;
    case Kind::Sheet:
      if (HasOfficeApp(OfficeApp::Excel) || HasLibreOffice()) list = {L"pdf", L"xlsx", L"csv", L"ods"};
      break;
    default: break;
  }
  if (k == Kind::Image && ext == L"gif") list.insert(list.end() - 1, L"mp4");
  std::erase(list, ext);
  if (list.size() > 8) list.resize(8);
  return list;
}

const wchar_t* Note(Kind k, const std::wstring& to) {
  if (k == Kind::Video && (to == L"mp3" || to == L"m4a")) return L"Chỉ lấy âm thanh";
  if (k == Kind::Video && to == L"gif") return L"Ảnh động, 10 giây đầu";
  if (k == Kind::Image && to == L"mp4") return L"Ảnh động thành video";
  if (k == Kind::Image && to == L"pdf") return L"Nhiều ảnh gộp thành một file";
  if (k == Kind::Sub && to == L"txt") return L"Chỉ lấy lời thoại";
  if (k == Kind::Pdf && (to == L"jpg" || to == L"png")) return L"Mỗi trang một ảnh, 300 DPI";
  if (k == Kind::Slides && (to == L"jpg" || to == L"png")) return L"Mỗi slide một ảnh";
  if (k == Kind::Sheet && to == L"csv") return L"Sheet đang chọn, UTF-8";
  static const std::map<std::wstring, const wchar_t*> notes = {
      {L"jpg", L"Nhẹ, mở được ở mọi nơi"}, {L"png", L"Không mất chất lượng"},
      {L"webp", L"Nhẹ, hợp cho web"},       {L"avif", L"Nhẹ nhất, chất lượng cao"},
      {L"tiff", L"Chất lượng cao, để in"},  {L"bmp", L"Không nén"},
      {L"gif", L"Ảnh động"},                {L"mp4", L"Phổ biến nhất"},
      {L"mov", L"Hợp cho máy Apple"},       {L"mkv", L"Giữ nhiều phụ đề, âm thanh"},
      {L"webm", L"Cho web"},                {L"avi", L"Cho máy đời cũ"},
      {L"mp3", L"Phổ biến nhất"},           {L"m4a", L"Nhẹ, nghe tốt"},
      {L"flac", L"Nén không mất chất lượng"}, {L"ogg", L"Định dạng mở"},
      {L"opus", L"Nhẹ nhất cho giọng nói"}, {L"wav", L"Không nén"},
      {L"aiff", L"Không nén"},              {L"wma", L"Windows Media"},
      {L"srt", L"Phổ biến nhất"},           {L"vtt", L"Phụ đề cho web"},
      {L"zip", L"Mở được ở mọi nơi"},       {L"7z", L"Nén mạnh hơn"},
      {L"tar", L"Gói lại, không nén"},      {L"tar.gz", L"Cho Linux, macOS"},
      {L"pdf", L"Để in hoặc gửi đi"},       {L"docx", L"Word"},
      {L"odt", L"LibreOffice, Google Docs"}, {L"rtf", L"Mở được ở mọi nơi"},
      {L"txt", L"Chỉ lấy chữ"},             {L"html", L"Trang web"},
      {L"pptx", L"PowerPoint"},             {L"xlsx", L"Excel"},
      {L"ods", L"LibreOffice"},
  };
  auto it = notes.find(to);
  return it != notes.end() ? it->second : L"";
}

// Rough bytes-per-content weights, used only for the "~size" hint.
double Weight(Kind k, const std::wstring& ext) {
  static const std::map<std::wstring, double> image = {{L"jpg", 1},   {L"heic", .55}, {L"heif", .55}, {L"webp", .68},
                                                       {L"avif", .5}, {L"png", 2.5},  {L"tiff", 4.4}, {L"bmp", 6.2},
                                                       {L"gif", 1.6}};
  static const std::map<std::wstring, double> video = {{L"mp4", 1},  {L"mov", 1.08}, {L"mkv", 1},    {L"webm", .82},
                                                       {L"avi", 1.55}, {L"wmv", 1.2}, {L"m4v", 1},   {L"3gp", .6},
                                                       {L"flv", 1.1}, {L"ts", 1.2},  {L"mts", 1.3}, {L"m2ts", 1.3},
                                                       {L"mpg", 1.6}, {L"mpeg", 1.6}};
  static const std::map<std::wstring, double> audio = {{L"wav", 1},     {L"aiff", 1.01}, {L"flac", .56}, {L"mp3", .136},
                                                       {L"m4a", .12},   {L"aac", .12},   {L"ogg", .11},  {L"opus", .08},
                                                       {L"wma", .12},   {L"amr", .02}};
  static const std::map<std::wstring, double> sub = {{L"srt", 1}, {L"vtt", 1.04}, {L"txt", .62}, {L"ass", 1.6}, {L"ssa", 1.5}};
  static const std::map<std::wstring, double> archive = {{L"zip", 1}, {L"7z", .86}, {L"tar", 1.28}, {L"tar.gz", 1.02}, {L"rar", .9}};
  const std::map<std::wstring, double>* t = nullptr;
  switch (k) {
    case Kind::Image: t = &image; break;
    case Kind::Video: t = &video; break;
    case Kind::Audio: t = &audio; break;
    case Kind::Sub: t = &sub; break;
    case Kind::Archive: t = &archive; break;
    default: return 0;
  }
  auto it = t->find(ext);
  return it != t->end() ? it->second : 0;
}

std::vector<SubItem> Levels() { return {{L"light", L"Nhẹ"}, {L"medium", L"Vừa"}, {L"strong", L"Mạnh"}}; }
std::vector<SubItem> Angles() { return {{L"ccw", L"↺ 90°"}, {L"180", L"180°"}, {L"cw", L"↻ 90°"}}; }

std::vector<Item> ToolsFor(Kind k) {
  switch (k) {
    case Kind::Image:
      return {
          {L"compress", L"Nén", L"Giảm dung lượng", icons::compress, Levels(), 1},
          {L"resize", L"Đổi cỡ", L"Thu nhỏ kích thước ảnh", icons::resize,
           {{L"half", L"50%"}, {L"1920", L"1920px"}, {L"1080", L"1080px"}}, 0},
          {L"crop", L"Cắt", L"Mở khung để chọn vùng cắt", icons::crop, {}, 0},
          {L"rotate", L"Xoay", L"Xoay ảnh", icons::rotate, Angles(), 2},
          {L"flip", L"Lật", L"Lật ngang như soi gương", icons::flip, {}, 0},
      };
    case Kind::Video:
      return {
          {L"compress", L"Nén", L"Giảm dung lượng (MP4)", icons::compress, Levels(), 1},
          {L"audio", L"Tách âm", L"Lấy âm thanh ra M4A", icons::music, {}, 0},
          {L"mute", L"Tắt tiếng", L"Bỏ âm thanh khỏi video", icons::mute, {}, 0},
          {L"resize", L"Đổi cỡ", L"Giảm độ phân giải", icons::resize,
           {{L"1080", L"1080p"}, {L"720", L"720p"}, {L"480", L"480p"}}, 1},
          {L"frame", L"Khung hình", L"Chụp khung hình đầu tiên", icons::camera, {}, 0},
          {L"rotate", L"Xoay", L"Xoay video, không mã hoá lại", icons::rotate, Angles(), 2},
          {L"crop", L"Cắt khung", L"Mở khung để chọn vùng giữ lại", icons::crop, {}, 0},
      };
    case Kind::Audio:
      return {
          {L"compress", L"Nén", L"Nén thành MP3", icons::compress,
           {{L"320", L"320k"}, {L"192", L"192k"}, {L"128", L"128k"}}, 1},
          {L"normalize", L"Chuẩn hoá", L"Cân bằng âm lượng", icons::bars, {}, 0},
          {L"speed", L"Tốc độ", L"Đổi tốc độ phát", icons::gauge,
           {{L"0.75", L"0,75×"}, {L"1.25", L"1,25×"}, {L"1.5", L"1,5×"}}, 1},
      };
    case Kind::Archive:
      return {
          {L"extract", L"Giải nén", L"Giải nén ngay tại đây", icons::unbox, {}, 0},
          {L"test", L"Kiểm tra", L"Kiểm tra file có lỗi không", icons::check, {}, 0},
      };
    case Kind::Sub:
      return {
          {L"shift", L"Dời giờ", L"Dời thời gian phụ đề", icons::clock,
           {{L"-1", L"−1 giây"}, {L"+0.5", L"+0,5 giây"}, {L"+1", L"+1 giây"}}, 1},
          {L"utf8", L"Sửa font", L"Sửa lỗi font tiếng Việt (UTF-8)", icons::code, {}, 0},
      };
    case Kind::Folder:
      return {{L"zip", L"Nén ZIP", L"Nén thư mục thành ZIP", icons::zip, {}, 0}};
    default:
      return {};
  }
}

std::vector<Item> Actions() {
  return {
      {L"share", L"Chia sẻ", L"Gửi tới thiết bị ở gần", icons::share, {}, 0},
      {L"float", L"Ghim nổi", L"Giữ file ở góc màn hình để kéo đi sau", icons::pin, {}, 0},
      {L"copy", L"Copy", L"Dán ở đâu cũng được", icons::copy, {}, 0},
      {L"move", L"Di chuyển", L"Chọn thư mục đích", icons::move, {}, 0},
      {L"path", L"Đường dẫn", L"Copy đường dẫn đầy đủ", icons::link, {}, 0},
      {L"terminal", L"Terminal", L"Mở Terminal ở thư mục này", icons::term, {}, 0},
  };
}

double TotalSize(const std::vector<std::wstring>& files) {
  double s = 0;
  for (auto& f : files) s += util::IsDirectory(f) ? 0 : (double)util::FileSize(f);
  return s;
}

}  // namespace

bool HasOfficeApp(OfficeApp app) {
  static int cache[3] = {-1, -1, -1};
  int& c = cache[(int)app];
  if (c < 0) {
    static const wchar_t* ids[3] = {L"Word.Application\\CLSID", L"PowerPoint.Application\\CLSID",
                                    L"Excel.Application\\CLSID"};
    HKEY key;
    c = RegOpenKeyExW(HKEY_CLASSES_ROOT, ids[(int)app], 0, KEY_READ, &key) == ERROR_SUCCESS;
    if (c) RegCloseKey(key);
  }
  return c == 1;
}

std::wstring LibreOfficePath() {
  static const std::wstring path = [] {
    for (auto var : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
      wchar_t buf[MAX_PATH];
      if (GetEnvironmentVariableW(var, buf, MAX_PATH)) {
        std::wstring p = std::wstring(buf) + L"\\LibreOffice\\program\\soffice.exe";
        if (util::Exists(p)) return p;
      }
    }
    return std::wstring();
  }();
  return path;
}

bool HasLibreOffice() { return !LibreOfficePath().empty(); }

std::wstring NormalExt(const std::wstring& path) {
  std::wstring e = util::ExtOf(path);
  if (e == L"jpeg" || e == L"jfif" || e == L"jpe") return L"jpg";
  if (e == L"tif") return L"tiff";
  if (e == L"aif") return L"aiff";
  if (e == L"htm") return L"html";
  if (e == L"tgz") return L"tar.gz";
  return e;
}

Kind KindOf(const std::wstring& path) {
  if (util::IsDirectory(path)) return Kind::Folder;
  auto it = KindTable().find(NormalExt(path));
  return it != KindTable().end() ? it->second : Kind::Other;
}

std::vector<std::wstring> TargetsFor(Mode mode, const std::vector<std::wstring>& files) {
  if (mode == Mode::Actions || files.empty()) return files;
  Kind k = KindOf(files[0]);
  std::vector<std::wstring> out;
  for (auto& f : files)
    if (KindOf(f) == k) out.push_back(f);
  return out;
}

std::vector<Item> ItemsFor(Mode mode, const std::vector<std::wstring>& files) {
  if (files.empty()) return {};
  if (mode == Mode::Actions) return Actions();
  Kind k = KindOf(files[0]);
  if (mode == Mode::Tools) return ToolsFor(k);
  std::vector<Item> items;
  for (auto& ext : ConvertTargets(k, NormalExt(files[0]))) items.push_back({ext, LabelFor(ext), L"", nullptr, {}, 0});
  return items;
}

HubText Describe(Mode mode, const std::vector<std::wstring>& files, const Item& item, const SubItem* sub,
                 bool defaultSub) {
  HubText t;
  auto targets = TargetsFor(mode, files);
  std::wstring count = targets.size() > 1 ? std::to_wstring(targets.size()) + L" file · " : L"";
  if (mode == Mode::Convert) {
    Kind k = KindOf(files[0]);
    t.h2 = item.label;
    t.h3 = Note(k, item.id);
    double est = 0;
    for (auto& f : targets) {
      double from = Weight(k, NormalExt(f)), to = Weight(k, item.id);
      if (from > 0 && to > 0) est += util::FileSize(f) / from * to;
    }
    if (est > 0) t.h4 = count + L"~" + util::FormatSize(est);
    else if (!count.empty()) t.h4 = std::to_wstring(targets.size()) + L" file";
    return t;
  }
  if (mode == Mode::Tools) {
    t.h2 = item.label + (sub ? L" · " + sub->label : L"");
    t.h3 = defaultSub ? L"Kéo ra xa để chọn mức" : item.title;
    double factor = 0;
    if (item.id == L"compress" && sub && KindOf(files[0]) != Kind::Audio) {
      bool video = KindOf(files[0]) == Kind::Video;
      factor = sub->id == L"light" ? (video ? .7 : .78) : sub->id == L"medium" ? (video ? .44 : .52) : (video ? .24 : .3);
    }
    if (item.id == L"resize" && sub && KindOf(files[0]) == Kind::Video)
      factor = sub->id == L"1080" ? .62 : sub->id == L"720" ? .34 : .17;
    if (factor > 0) {
      double size = TotalSize(targets) * factor;
      t.h4 = count + L"~" + util::FormatSize(size) + L" (−" + std::to_wstring((int)std::lround((1 - factor) * 100)) + L"%)";
    } else if (!count.empty()) {
      t.h4 = std::to_wstring(targets.size()) + L" file";
    }
    return t;
  }
  t.h2 = item.label;
  t.h3 = item.title;
  if (files.size() > 1) t.h4 = std::to_wstring(files.size()) + L" mục";
  return t;
}

HubText EmptyText(Mode mode, const std::vector<std::wstring>& files) {
  HubText t;
  Kind k = files.empty() ? Kind::Other : KindOf(files[0]);
  t.h2 = L"Chưa có";
  bool office = k == Kind::Doc || k == Kind::Slides || k == Kind::Sheet;
  if (office && mode == Mode::Convert)
    t.h3 = k == Kind::Doc ? L"Cần Word hoặc LibreOffice cho loại file này" : L"Cần Microsoft Office hoặc LibreOffice";
  else if (k == Kind::Folder && mode == Mode::Convert)
    t.h3 = L"Thư mục không đổi định dạng được";
  else if (mode == Mode::Tools)
    t.h3 = L"Chưa có công cụ cho loại file này";
  else
    t.h3 = L"Chưa hỗ trợ loại file này";
  t.h4 = std::wstring(L"Giữ ") + KeyForMode(Mode::Actions) + L" để xem thao tác";
  return t;
}

const wchar_t* ModeTitle(Mode m) {
  switch (m) {
    case Mode::Convert: return L"Chuyển đổi";
    case Mode::Tools: return L"Công cụ";
    default: return L"Thao tác";
  }
}



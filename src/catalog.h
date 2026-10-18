#pragma once
#include "common.h"

enum class Mode { Convert = 0, Tools = 1, Actions = 2 };
enum class Kind { Image, Video, Audio, Pdf, Doc, Slides, Sheet, Archive, Sub, Folder, Other };

struct SubItem {
  std::wstring id, label;
};

// One segment of the wheel.
struct Item {
  std::wstring id;     // target extension for Convert, operation id for Tools/Actions
  std::wstring label;  // short text on the segment
  std::wstring title;  // longer description shown in the hub
  const char* icon = nullptr;  // SVG path data on a 24x24 grid; none for formats
  std::vector<SubItem> subs;   // outer ring choices (levels, sizes, angles)
  int def = 0;                 // sub used when the user drops on the segment itself
};

Kind KindOf(const std::wstring& path);
// Lower-case extension with aliases folded (jpeg -> jpg, tif -> tiff, tgz -> tar.gz).
std::wstring NormalExt(const std::wstring& path);

std::vector<Item> ItemsFor(Mode mode, const std::vector<std::wstring>& files);
// The dropped files an item will run on: for Convert and Tools only those of the
// same kind as the first file, for Actions all of them.
std::vector<std::wstring> TargetsFor(Mode mode, const std::vector<std::wstring>& files);

struct HubText {
  std::wstring h2, h3, h4;
};
HubText Describe(Mode mode, const std::vector<std::wstring>& files, const Item& item, const SubItem* sub,
                 bool defaultSub);
// Hub text when a mode has nothing to offer for these files.
HubText EmptyText(Mode mode, const std::vector<std::wstring>& files);

const wchar_t* ModeTitle(Mode m);

// Office apps reachable through COM automation, and LibreOffice as a fallback.
enum class OfficeApp { Word = 0, PowerPoint = 1, Excel = 2 };
bool HasOfficeApp(OfficeApp app);
bool HasLibreOffice();
std::wstring LibreOfficePath();

#include "svgpath.h"

#include <cctype>
#include <cstdlib>

namespace {

struct Reader {
  const char* s;
  void Skip() { while (*s && (std::isspace((unsigned char)*s) || *s == ',')) ++s; }
  bool AtNumber() {
    Skip();
    return *s && (std::isdigit((unsigned char)*s) || *s == '-' || *s == '+' || *s == '.');
  }
  bool Num(float& v) {
    Skip();
    char* end = nullptr;
    v = std::strtof(s, &end);
    if (end == s) return false;
    s = end;
    return true;
  }
  bool Flag(int& f) {
    Skip();
    if (*s != '0' && *s != '1') return false;
    f = *s - '0';
    ++s;
    return true;
  }
};

}  // namespace

ComPtr<ID2D1PathGeometry> BuildSvgPath(ID2D1Factory* factory, const char* data) {
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;

  Reader r{data};
  char cmd = 0;
  float cx = 0, cy = 0, sx = 0, sy = 0;
  bool open = false;
  auto begin = [&](float x, float y) {
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
    open = true;
    sx = x; sy = y;
  };
  auto ensure = [&]() { if (!open) begin(cx, cy); };

  for (;;) {
    r.Skip();
    if (!*r.s) break;
    if (std::isalpha((unsigned char)*r.s)) cmd = *r.s++;
    else if (!cmd) break;
    bool rel = std::islower((unsigned char)cmd);
    float ox = rel ? cx : 0, oy = rel ? cy : 0;
    switch (cmd) {
      case 'M': case 'm': {
        float x, y;
        if (!r.Num(x) || !r.Num(y)) goto done;
        cx = ox + x; cy = oy + y;
        begin(cx, cy);
        cmd = rel ? 'l' : 'L';  // extra coordinate pairs are line-tos
        break;
      }
      case 'L': case 'l': {
        float x, y;
        if (!r.Num(x) || !r.Num(y)) goto done;
        ensure();
        cx = ox + x; cy = oy + y;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'H': case 'h': {
        float x;
        if (!r.Num(x)) goto done;
        ensure();
        cx = ox + x;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'V': case 'v': {
        float y;
        if (!r.Num(y)) goto done;
        ensure();
        cy = oy + y;
        sink->AddLine(D2D1::Point2F(cx, cy));
        break;
      }
      case 'C': case 'c': {
        float x1, y1, x2, y2, x, y;
        if (!r.Num(x1) || !r.Num(y1) || !r.Num(x2) || !r.Num(y2) || !r.Num(x) || !r.Num(y)) goto done;
        ensure();
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(ox + x1, oy + y1), D2D1::Point2F(ox + x2, oy + y2),
                                            D2D1::Point2F(ox + x, oy + y)));
        cx = ox + x; cy = oy + y;
        break;
      }
      case 'A': case 'a': {
        float rx, ry, rot, x, y;
        int large, sweep;
        if (!r.Num(rx) || !r.Num(ry) || !r.Num(rot) || !r.Flag(large) || !r.Flag(sweep) || !r.Num(x) || !r.Num(y)) goto done;
        ensure();
        float ex = ox + x, ey = oy + y;
        if (rx == 0 || ry == 0) {
          sink->AddLine(D2D1::Point2F(ex, ey));
        } else {
          D2D1_ARC_SEGMENT arc{D2D1::Point2F(ex, ey), D2D1::SizeF(std::abs(rx), std::abs(ry)), rot,
                               sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                               large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
          sink->AddArc(arc);
        }
        cx = ex; cy = ey;
        break;
      }
      case 'Z': case 'z':
        if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
        cx = sx; cy = sy;
        if (r.AtNumber()) goto done;  // malformed
        cmd = 0;
        break;
      default:
        goto done;
    }
  }
done:
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  if (FAILED(sink->Close())) return nullptr;
  return path;
}

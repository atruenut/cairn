#include "arrange.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace cairn {

std::vector<Box> arrangeBoxes(const std::vector<Box>& in, Arrange mode) {
  std::vector<Box> out = in;
  if (in.size() < 2) return out;
  int minX = in[0].x, minY = in[0].y, maxR = in[0].x + in[0].w, maxB = in[0].y + in[0].h;
  for (const Box& b : in) {
    minX = std::min(minX, b.x);
    minY = std::min(minY, b.y);
    maxR = std::max(maxR, b.x + b.w);
    maxB = std::max(maxB, b.y + b.h);
  }
  switch (mode) {
    case Arrange::Left: for (Box& b : out) b.x = minX; break;
    case Arrange::Right: for (Box& b : out) b.x = maxR - b.w; break;
    case Arrange::Top: for (Box& b : out) b.y = minY; break;
    case Arrange::Bottom: for (Box& b : out) b.y = maxB - b.h; break;
    case Arrange::CenterH: for (Box& b : out) b.x = (minX + maxR - b.w) / 2; break;
    case Arrange::CenterV: for (Box& b : out) b.y = (minY + maxB - b.h) / 2; break;
    case Arrange::SpreadH:
    case Arrange::SpreadV: {
      if (in.size() < 3) break;
      bool h = mode == Arrange::SpreadH;
      auto pos = [&](const Box& b) { return h ? b.x : b.y; };
      auto len = [&](const Box& b) { return h ? b.w : b.h; };
      std::vector<size_t> order(in.size());
      std::iota(order.begin(), order.end(), size_t(0));
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pos(in[a]) < pos(in[b]); });
      const Box& first = in[order.front()];
      const Box& last = in[order.back()];
      long span = (long)pos(last) + len(last) - pos(first);  // outer edge to outer edge
      long used = 0;
      for (const Box& b : in) used += len(b);
      double gap = (double)(span - used) / (double)(in.size() - 1);
      double cur = pos(first) + len(first) + gap;
      for (size_t k = 1; k + 1 < order.size(); ++k) {
        Box& b = out[order[k]];
        (h ? b.x : b.y) = (int)std::lround(cur);
        cur += len(in[order[k]]) + gap;
      }
      break;
    }
  }
  return out;
}

bool shapeSegment(int kind, int x, int y, int w, int h, Segment& o) {
  int cx = x + w / 2, cy = y + h / 2;
  switch (kind) {
    case 4: o = {x, cy, x + w, cy, false}; return true;
    case 5: o = {cx, y, cx, y + h, false}; return true;
    case 6: o = {x, y, x + w, y + h, false}; return true;
    case 7: o = {x, y + h, x + w, y, false}; return true;
    case 8: o = {x, cy, x + w, cy, true}; return true;
    case 9: o = {cx, y, cx, y + h, true}; return true;
  }
  return false;
}

void arrowHead(const Segment& s, int len, int half, Pt out[3]) {
  double dx = s.x2 - s.x1, dy = s.y2 - s.y1, d = std::sqrt(dx * dx + dy * dy);
  out[0] = {s.x2, s.y2};
  if (d < 1e-9) {
    out[1] = out[2] = out[0];
    return;
  }
  double ux = dx / d, uy = dy / d;
  double bx = s.x2 - ux * len, by = s.y2 - uy * len;
  out[1] = {(int)std::lround(bx - uy * half), (int)std::lround(by + ux * half)};
  out[2] = {(int)std::lround(bx + uy * half), (int)std::lround(by - ux * half)};
}

namespace {
double distToSegment(double px, double py, double x1, double y1, double x2, double y2) {
  double dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.0, 1.0) : 0.0;
  return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
}
bool insideTriangle(double px, double py, const double v[6]) {
  auto side = [&](double ax, double ay, double bx, double by) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
  double a = side(v[0], v[1], v[2], v[3]), b = side(v[2], v[3], v[4], v[5]), c = side(v[4], v[5], v[0], v[1]);
  return (a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0);
}
}  // namespace

bool shapeContains(int kind, double w, double h, double px, double py, double tol) {
  Segment s;
  if (shapeSegment(kind, 0, 0, (int)w, (int)h, s)) return distToSegment(px, py, s.x1, s.y1, s.x2, s.y2) <= tol;
  if (kind == 1) {
    double a = w / 2 + tol, b = h / 2 + tol, dx = px - w / 2, dy = py - h / 2;
    return a > 0 && b > 0 && (dx * dx) / (a * a) + (dy * dy) / (b * b) <= 1.0;
  }
  if (kind == 2) {
    double a = w / 2 + tol, b = h / 2 + tol;
    return a > 0 && b > 0 && std::fabs(px - w / 2) / a + std::fabs(py - h / 2) / b <= 1.0;
  }
  if (kind == 3) {
    const double v[6] = {w / 2, 0, w, h, 0, h};
    if (insideTriangle(px, py, v)) return true;
    return distToSegment(px, py, v[0], v[1], v[2], v[3]) <= tol || distToSegment(px, py, v[2], v[3], v[4], v[5]) <= tol ||
           distToSegment(px, py, v[4], v[5], v[0], v[1]) <= tol;
  }
  return true;
}

}  // namespace cairn

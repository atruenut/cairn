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

}  // namespace cairn

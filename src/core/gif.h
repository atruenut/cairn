// Animated GIF writer (GIF89a, LZW, one shared palette). OS-free so it is unit-tested natively.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cairn {

struct GifFrame {
  std::vector<uint32_t> px;  // w*h pixels, 0x00RRGGBB
  int delayMs = 100;         // GIF stores hundredths of a second, so this is rounded (minimum 20 ms)
};

// Encodes the frames (all w x h) into `out`. loops = 0 repeats forever. When the page uses more than 256 colours
// the palette is chosen by median cut, with no dithering (pages are mostly flat colour, so this looks right).
// Frames after the first only store the rectangle that changed. Returns false for empty or mismatched input.
bool encodeGif(int w, int h, const std::vector<GifFrame>& frames, int loops, std::string& out);

}  // namespace cairn

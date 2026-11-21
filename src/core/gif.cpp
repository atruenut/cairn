#include "gif.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace cairn {
namespace {

struct Box {
  std::vector<std::pair<uint32_t, uint32_t>> c;  // colour, count
};

std::vector<uint32_t> buildPalette(const std::vector<GifFrame>& frames) {
  std::unordered_map<uint32_t, uint32_t> hist;
  for (const GifFrame& f : frames)
    for (uint32_t p : f.px) ++hist[p & 0xFFFFFF];
  std::vector<std::pair<uint32_t, uint32_t>> all(hist.begin(), hist.end());
  std::sort(all.begin(), all.end());  // deterministic output
  std::vector<uint32_t> pal;
  if (all.size() <= 256) {
    for (auto& kv : all) pal.push_back(kv.first);
    return pal;
  }
  std::vector<Box> boxes(1);
  boxes[0].c = all;
  auto channel = [](uint32_t c, int ch) { return (int)((c >> (16 - 8 * ch)) & 255); };
  while (boxes.size() < 256) {
    // split the box with the widest colour range
    size_t bi = 0;
    int bestRange = -1, bestCh = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].c.size() < 2) continue;
      for (int ch = 0; ch < 3; ++ch) {
        int lo = 255, hi = 0;
        for (auto& kv : boxes[i].c) { lo = std::min(lo, channel(kv.first, ch)); hi = std::max(hi, channel(kv.first, ch)); }
        if (hi - lo > bestRange) { bestRange = hi - lo; bi = i; bestCh = ch; }
      }
    }
    if (bestRange < 0) break;
    auto& v = boxes[bi].c;
    std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
      int x = channel(a.first, bestCh), y = channel(b.first, bestCh);
      return x != y ? x < y : a.first < b.first;
    });
    uint64_t total = 0, run = 0;
    for (auto& kv : v) total += kv.second;
    size_t cut = 1;
    for (size_t i = 0; i + 1 < v.size(); ++i) {
      run += v[i].second;
      cut = i + 1;
      if (run * 2 >= total) break;
    }
    Box nb;
    nb.c.assign(v.begin() + (long)cut, v.end());
    v.resize(cut);
    boxes.push_back(std::move(nb));
  }
  for (const Box& b : boxes) {
    uint64_t r = 0, g = 0, bl = 0, n = 0;
    for (auto& kv : b.c) {
      r += (uint64_t)channel(kv.first, 0) * kv.second;
      g += (uint64_t)channel(kv.first, 1) * kv.second;
      bl += (uint64_t)channel(kv.first, 2) * kv.second;
      n += kv.second;
    }
    if (!n) continue;
    pal.push_back((uint32_t)(((r / n) << 16) | ((g / n) << 8) | (bl / n)));
  }
  return pal;
}

struct BitWriter {
  std::string& out;
  std::string block;
  uint32_t acc = 0;
  int nbits = 0;
  explicit BitWriter(std::string& o) : out(o) {}
  void flushBlock() {
    if (block.empty()) return;
    out += (char)block.size();
    out += block;
    block.clear();
  }
  void put(uint32_t code, int bits) {
    acc |= code << nbits;
    nbits += bits;
    while (nbits >= 8) {
      block += (char)(acc & 255);
      if (block.size() == 255) flushBlock();
      acc >>= 8;
      nbits -= 8;
    }
  }
  void finish() {
    if (nbits > 0) block += (char)(acc & 255);
    flushBlock();
    out += '\0';
  }
};

void lzw(const std::vector<uint8_t>& idx, int minCode, std::string& out) {
  out += (char)minCode;
  BitWriter bw(out);
  const int clear = 1 << minCode, eoi = clear + 1;
  int next = eoi + 1, bits = minCode + 1;
  std::unordered_map<uint32_t, int> dict;
  bw.put((uint32_t)clear, bits);
  if (idx.empty()) {
    bw.put((uint32_t)eoi, bits);
    bw.finish();
    return;
  }
  int prefix = idx[0];
  for (size_t i = 1; i < idx.size(); ++i) {
    uint32_t key = ((uint32_t)prefix << 8) | idx[i];
    auto it = dict.find(key);
    if (it != dict.end()) {
      prefix = it->second;
      continue;
    }
    bw.put((uint32_t)prefix, bits);
    dict[key] = next;
    if (next == (1 << bits)) ++bits;
    ++next;
    if (next == 4096) {
      bw.put((uint32_t)clear, bits);
      dict.clear();
      next = eoi + 1;
      bits = minCode + 1;
    }
    prefix = idx[i];
  }
  bw.put((uint32_t)prefix, bits);
  bw.put((uint32_t)eoi, bits);
  bw.finish();
}

void le16(std::string& o, int v) {
  o += (char)(v & 255);
  o += (char)((v >> 8) & 255);
}

}  // namespace

bool encodeGif(int w, int h, const std::vector<GifFrame>& frames, int loops, std::string& out) {
  if (w < 1 || h < 1 || w > 65535 || h > 65535 || frames.empty()) return false;
  const size_t n = (size_t)w * (size_t)h;
  for (const GifFrame& f : frames)
    if (f.px.size() != n) return false;
  std::vector<uint32_t> pal = buildPalette(frames);
  if (pal.empty()) return false;
  int tableBits = 1;
  while ((1 << tableBits) < (int)pal.size()) ++tableBits;
  int tableSize = 1 << tableBits;
  pal.resize((size_t)tableSize, 0);

  out.clear();
  out += "GIF89a";
  le16(out, w);
  le16(out, h);
  out += (char)(0x80 | (7 << 4) | (tableBits - 1));
  out += '\0';
  out += '\0';
  for (uint32_t c : pal) {
    out += (char)((c >> 16) & 255);
    out += (char)((c >> 8) & 255);
    out += (char)(c & 255);
  }
  if (frames.size() > 1) {
    out += "\x21\xFF\x0B" "NETSCAPE2.0" "\x03\x01";
    le16(out, loops);
    out += '\0';
  }

  const size_t realColors = pal.size();
  (void)realColors;
  std::unordered_map<uint32_t, uint8_t> cache;
  auto nearest = [&](uint32_t c) -> uint8_t {
    auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    int best = 0;
    long bestD = -1;
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    for (int i = 0; i < tableSize; ++i) {
      int pr = (int)((pal[(size_t)i] >> 16) & 255), pg = (int)((pal[(size_t)i] >> 8) & 255), pb = (int)(pal[(size_t)i] & 255);
      long d = (long)(r - pr) * (r - pr) * 3 + (long)(g - pg) * (g - pg) * 4 + (long)(b - pb) * (b - pb) * 2;
      if (bestD < 0 || d < bestD) { bestD = d; best = i; }
    }
    cache[c] = (uint8_t)best;
    return (uint8_t)best;
  };

  std::vector<uint8_t> prev, cur(n);
  int minCode = std::max(2, tableBits);
  for (size_t fi = 0; fi < frames.size(); ++fi) {
    const GifFrame& f = frames[fi];
    for (size_t i = 0; i < n; ++i) cur[i] = nearest(f.px[i] & 0xFFFFFF);
    int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (fi > 0) {
      x0 = w; y0 = h; x1 = -1; y1 = -1;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (cur[(size_t)y * w + x] != prev[(size_t)y * w + x]) {
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
          }
      if (x1 < 0) { x0 = y0 = x1 = y1 = 0; }  // nothing changed: a 1-pixel frame still carries the delay
    }
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<uint8_t> sub((size_t)cw * ch);
    for (int y = 0; y < ch; ++y)
      for (int x = 0; x < cw; ++x) sub[(size_t)y * cw + x] = cur[(size_t)(y0 + y) * w + (x0 + x)];
    int cs = std::max(2, (f.delayMs + 5) / 10);
    out += "\x21\xF9\x04";
    out += (char)0x04;  // leave the previous frame in place (so the cropped rectangle is drawn over it)
    le16(out, cs);
    out += '\0';
    out += '\0';
    out += (char)0x2C;
    le16(out, x0);
    le16(out, y0);
    le16(out, cw);
    le16(out, ch);
    out += '\0';
    lzw(sub, minCode, out);
    prev = cur;
  }
  out += (char)0x3B;
  return true;
}

}  // namespace cairn

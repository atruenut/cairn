#include "media.h"

#include <cstring>

namespace cairn {

bool sniffAudio(const std::string& b, AudioKind& out) {
  auto at = [&](size_t i, const char* s, size_t n) { return b.size() >= i + n && std::memcmp(b.data() + i, s, n) == 0; };
  auto byte = [&](size_t i) { return (unsigned char)b[i]; };
  if (b.size() < 12) return false;
  if (at(0, "RIFF", 4) && at(8, "WAVE", 4)) return out = {"wav", "audio/wav"}, true;
  if (at(0, "MThd", 4)) return out = {"mid", "audio/midi"}, true;
  if (at(4, "ftyp", 4) && (at(8, "M4A ", 4) || at(8, "M4B ", 4))) return out = {"m4a", "audio/mp4"}, true;
  static const unsigned char asf[8] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11};
  if (std::memcmp(b.data(), asf, 8) == 0) return out = {"wma", "audio/x-ms-wma"}, true;
  if (at(0, "ID3", 3) && byte(3) != 0xFF) return out = {"mp3", "audio/mpeg"}, true;
  // A bare MPEG audio frame header: 11 sync bits, a real version and layer, a usable bitrate and rate.
  if (byte(0) == 0xFF && (byte(1) & 0xE0) == 0xE0) {
    int version = (byte(1) >> 3) & 3, layer = (byte(1) >> 1) & 3, bitrate = (byte(2) >> 4) & 15, rate = (byte(2) >> 2) & 3;
    if (version != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && rate != 3) return out = {"mp3", "audio/mpeg"}, true;
  }
  return false;
}

}  // namespace cairn

// Image decoding via WIC (the codecs Windows already ships: PNG, JPEG, GIF, BMP,
// TIFF, ICO, and WebP/HEIC where the OS has the extension), plus small file and
// text helpers shared by drop and paste.
#pragma once

#include <string>
#include <vector>

#include "util.h"

namespace cairn {

struct Decoded {
  HBITMAP bmp = nullptr;  // top-down 32-bit premultiplied BGRA DIB; caller owns it
  int w = 0, h = 0;
};

// Decodes the first frame. Returns false for anything WIC can't read or that is
// unreasonably large (guards against decompression bombs).
bool decodeImage(const std::string& bytes, Decoded& out);

// An animated GIF, fully composed: every frame is a complete canvas-sized
// premultiplied BGRA DIB (disposal modes already applied). Caller owns the bitmaps.
struct Animation {
  int w = 0, h = 0;
  std::vector<HBITMAP> frames;
  std::vector<int> delaysMs;  // same length as frames
};

// Decodes a GIF with 2+ frames. Returns false for anything else (or if it would need
// too much memory), in which case the caller falls back to decodeImage's first frame.
bool decodeAnimation(const std::string& bytes, Animation& out);

// Resamples a 32-bit premultiplied DIB (from decodeImage) to dw x dh with WIC's scaler.
// GDI's StretchBlt can't be used here: it leaves the alpha channel undefined.
bool scaleImage(HBITMAP src, int sw, int sh, int dw, int dh, HBITMAP& out);

// MIME type from magic bytes (not from the extension: names lie).
std::string sniffMime(const std::string& bytes);

// Reads a whole file; false if missing, unreadable, or larger than maxBytes.
bool readWholeFile(const std::wstring& path, std::string& out, size_t maxBytes);

// Text file bytes (UTF-8, UTF-16 with BOM, or legacy ANSI) -> UTF-8 with \n newlines.
std::string textFromBytes(const std::string& bytes);

bool isImageExtension(const std::wstring& path);
bool isTextExtension(const std::wstring& path);
std::wstring fileNameOf(const std::wstring& path);

}  // namespace cairn

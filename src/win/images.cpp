#include "images.h"

#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>

namespace cairn {

namespace {

template <class T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() { if (p) p->Release(); }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

constexpr UINT kMaxDim = 20000;
constexpr unsigned long long kMaxPixels = 120ull * 1000 * 1000;  // ~480 MB as 32-bit

}  // namespace

bool decodeImage(const std::string& bytes, Decoded& out) {
  if (bytes.empty() || bytes.size() > 0x7fffffffu) return false;

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory, (void**)&factory)))
    return false;
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream))) return false;
  if (FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return false;

  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
    return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;

  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > kMaxDim || h > kMaxDim ||
      (unsigned long long)w * h > kMaxPixels)
    return false;

  ComPtr<IWICFormatConverter> conv;
  if (FAILED(factory->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = (LONG)w;
  bi.bmiHeader.biHeight = -(LONG)h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) return false;
  if (FAILED(conv->CopyPixels(nullptr, w * 4, w * 4 * h, (BYTE*)bits))) {
    DeleteObject(bmp);
    return false;
  }
  out.bmp = bmp;
  out.w = (int)w;
  out.h = (int)h;
  return true;
}

namespace {
bool metaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT& v) {
  if (!q) return false;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  bool ok = false;
  if (SUCCEEDED(q->GetMetadataByName(name, &pv))) {
    if (pv.vt == VT_UI1) { v = pv.bVal; ok = true; }
    else if (pv.vt == VT_UI2) { v = pv.uiVal; ok = true; }
    else if (pv.vt == VT_UI4) { v = pv.ulVal; ok = true; }
  }
  PropVariantClear(&pv);
  return ok;
}
constexpr UINT kMaxFrames = 2000;
constexpr unsigned long long kMaxAnimPixels = 48ull * 1000 * 1000;  // all frames together, ~190 MB
}  // namespace

bool decodeAnimation(const std::string& bytes, Animation& out) {
  if (bytes.empty() || bytes.size() > 0x7fffffffu || sniffMime(bytes) != "image/gif") return false;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory, (void**)&factory)))
    return false;
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())))
    return false;
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
    return false;
  UINT n = 0;
  if (FAILED(decoder->GetFrameCount(&n)) || n < 2 || n > kMaxFrames) return false;

  UINT cw = 0, ch = 0;
  {
    ComPtr<IWICMetadataQueryReader> dq;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&dq))) {
      metaUInt(dq.p, L"/logscrdesc/Width", cw);
      metaUInt(dq.p, L"/logscrdesc/Height", ch);
    }
  }
  if (!cw || !ch) {
    ComPtr<IWICBitmapFrameDecode> f0;
    if (FAILED(decoder->GetFrame(0, &f0)) || FAILED(f0->GetSize(&cw, &ch))) return false;
  }
  if (!cw || !ch || cw > kMaxDim || ch > kMaxDim || (unsigned long long)cw * ch * n > kMaxAnimPixels) return false;

  std::vector<uint32_t> canvas((size_t)cw * ch, 0), saved, buf;
  Animation a;
  a.w = (int)cw;
  a.h = (int)ch;
  auto fail = [&]() {
    for (HBITMAP b : a.frames) DeleteObject(b);
    return false;
  };

  for (UINT i = 0; i < n; ++i) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(i, &frame))) return fail();
    UINT fw = 0, fh = 0;
    if (FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh || fw > kMaxDim || fh > kMaxDim) return fail();
    UINT left = 0, top = 0, delay = 0, disposal = 0;
    {
      ComPtr<IWICMetadataQueryReader> fq;
      if (SUCCEEDED(frame->GetMetadataQueryReader(&fq))) {
        metaUInt(fq.p, L"/imgdesc/Left", left);
        metaUInt(fq.p, L"/imgdesc/Top", top);
        metaUInt(fq.p, L"/grctlext/Delay", delay);
        metaUInt(fq.p, L"/grctlext/Disposal", disposal);
      }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
      return fail();
    buf.assign((size_t)fw * fh, 0);
    if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)(buf.size() * 4), (BYTE*)buf.data()))) return fail();

    if (disposal == 3) saved = canvas;  // "restore previous" needs the canvas as it was before this frame
    for (UINT y = 0; y < fh && top + y < ch; ++y) {
      for (UINT x = 0; x < fw && left + x < cw; ++x) {
        uint32_t s = buf[(size_t)y * fw + x];
        uint32_t sa = s >> 24;
        if (sa == 0) continue;  // GIF transparency: leave what's underneath
        uint32_t& d = canvas[(size_t)(top + y) * cw + (left + x)];
        if (sa == 255) { d = s; continue; }
        uint32_t inv = 255 - sa, r = 0;
        for (int sh = 0; sh < 32; sh += 8)
          r |= (std::min<uint32_t>(255, ((s >> sh) & 255) + (((d >> sh) & 255) * inv + 127) / 255)) << sh;
        d = r;
      }
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)cw;
    bi.bmiHeader.biHeight = -(LONG)ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return fail();
    std::memcpy(bits, canvas.data(), canvas.size() * 4);
    a.frames.push_back(bmp);
    int ms = (int)delay * 10;
    a.delaysMs.push_back(ms < 20 ? 100 : ms);  // browsers treat 0/10 ms as 100 ms; so do we

    if (disposal == 2) {
      for (UINT y = 0; y < fh && top + y < ch; ++y)
        for (UINT x = 0; x < fw && left + x < cw; ++x) canvas[(size_t)(top + y) * cw + (left + x)] = 0;
    } else if (disposal == 3 && saved.size() == canvas.size()) {
      canvas = saved;
    }
  }
  out = std::move(a);
  return true;
}

bool savePng(HBITMAP dib, const std::wstring& path) {
  BITMAP bm{};
  if (!dib || !GetObjectW(dib, sizeof bm, &bm) || !bm.bmBits || bm.bmBitsPixel != 32 || bm.bmWidth <= 0 || bm.bmHeight == 0)
    return false;
  UINT w = (UINT)bm.bmWidth, h = (UINT)std::abs(bm.bmHeight);
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                              (void**)&factory)))
    return false;
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream))) return false;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;  // never overwrite
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
  if (FAILED(frame->Initialize(props.p)) || FAILED(frame->SetSize(w, h))) return false;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&fmt))) return false;
  UINT stride = w * 4;
  if (FAILED(frame->WritePixels(h, stride, stride * h, (BYTE*)bm.bmBits))) return false;
  if (FAILED(frame->Commit()) || FAILED(enc->Commit())) {
    DeleteFileW(path.c_str());
    return false;
  }
  return true;
}

bool scaleImage(HBITMAP src, int sw, int sh, int dw, int dh, HBITMAP& out) {
  BITMAP bm{};
  if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !GetObjectW(src, sizeof bm, &bm) ||
      !bm.bmBits || bm.bmBitsPixel != 32 || bm.bmWidth != sw || std::abs(bm.bmHeight) != sh)
    return false;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory, (void**)&factory)))
    return false;
  ComPtr<IWICBitmap> bmp;
  if (FAILED(factory->CreateBitmapFromMemory((UINT)sw, (UINT)sh, GUID_WICPixelFormat32bppPBGRA, (UINT)sw * 4,
                                             (UINT)sw * 4 * (UINT)sh, (BYTE*)bm.bmBits, &bmp)))
    return false;
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
  // Fant is the right filter for shrinking; cubic for enlarging.
  WICBitmapInterpolationMode mode = ((long long)dw * dh < (long long)sw * sh)
                                        ? WICBitmapInterpolationModeFant
                                        : WICBitmapInterpolationModeCubic;
  if (FAILED(scaler->Initialize(bmp.p, (UINT)dw, (UINT)dh, mode))) return false;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = dw;
  bi.bmiHeader.biHeight = -dh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP nb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!nb || !bits) return false;
  if (FAILED(scaler->CopyPixels(nullptr, (UINT)dw * 4, (UINT)dw * 4 * (UINT)dh, (BYTE*)bits))) {
    DeleteObject(nb);
    return false;
  }
  out = nb;
  return true;
}

std::string sniffMime(const std::string& b) {
  auto starts = [&](const char* s, size_t n) { return b.size() >= n && b.compare(0, n, s, n) == 0; };
  if (starts("\x89PNG\r\n\x1a\n", 8)) return "image/png";
  if (starts("\xff\xd8\xff", 3)) return "image/jpeg";
  if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "image/gif";
  if (starts("BM", 2)) return "image/bmp";
  if (starts("II*\0", 4) || starts("MM\0*", 4)) return "image/tiff";
  if (starts("\0\0\1\0", 4)) return "image/x-icon";
  if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "image/webp";
  return "application/octet-stream";
}

bool readWholeFile(const std::wstring& path, std::string& out, size_t maxBytes) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart >= 0 && (unsigned long long)sz.QuadPart <= maxBytes;
  if (ok) {
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (ok && done < out.size()) {
      DWORD got = 0, want = (DWORD)std::min<size_t>(out.size() - done, 1u << 24);
      ok = ReadFile(f, &out[done], want, &got, nullptr) && got > 0;
      done += got;
    }
    if (done != out.size()) ok = false;
  }
  CloseHandle(f);
  if (!ok) out.clear();
  return ok;
}

std::string textFromBytes(const std::string& b) {
  std::wstring w;
  if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {
    w.assign((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
  } else {
    size_t skip = (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
                   (unsigned char)b[2] == 0xBF) ? 3 : 0;
    const char* p = b.data() + skip;
    int n = (int)(b.size() - skip);
    if (n > 0) {
      int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, nullptr, 0);
      UINT cp = CP_UTF8;
      if (wn <= 0) {  // not valid UTF-8: assume the legacy code page
        cp = CP_ACP;
        wn = MultiByteToWideChar(cp, 0, p, n, nullptr, 0);
      }
      if (wn > 0) {
        w.resize((size_t)wn);
        MultiByteToWideChar(cp, 0, p, n, &w[0], wn);
      }
    }
  }
  std::wstring clean;
  clean.reserve(w.size());
  for (size_t i = 0; i < w.size(); ++i) {
    if (w[i] == L'\r') {
      if (i + 1 < w.size() && w[i + 1] == L'\n') continue;
      clean += L'\n';
    } else {
      clean += w[i];
    }
  }
  return narrow(clean);
}

static std::wstring lowerExt(const std::wstring& path) {
  size_t d = path.find_last_of(L'.'), s = path.find_last_of(L"\\/");
  if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return L"";
  std::wstring e = path.substr(d + 1);
  for (wchar_t& c : e) c = (wchar_t)std::towlower(c);
  return e;
}

bool isImageExtension(const std::wstring& p) {
  static const wchar_t* ex[] = {L"png", L"jpg", L"jpeg", L"jpe", L"gif", L"bmp", L"tif", L"tiff", L"ico", L"webp", L"heic"};
  std::wstring e = lowerExt(p);
  for (const wchar_t* x : ex)
    if (e == x) return true;
  return false;
}

bool isTextExtension(const std::wstring& p) {
  static const wchar_t* ex[] = {L"txt", L"md", L"markdown", L"log", L"csv"};
  std::wstring e = lowerExt(p);
  for (const wchar_t* x : ex)
    if (e == x) return true;
  return false;
}

std::wstring fileNameOf(const std::wstring& path) {
  size_t i = path.find_last_of(L"\\/");
  return i == std::wstring::npos ? path : path.substr(i + 1);
}

}  // namespace cairn

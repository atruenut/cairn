// Small Win32 helpers shared by the UI layer.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <string>

namespace cairn {

inline std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

// Document colors are 0xRRGGBB; GDI wants 0x00BBGGRR.
inline COLORREF toColorRef(int rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
inline int fromColorRef(COLORREF c) { return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }

inline int dpiFor(HWND h) {
  UINT d = h ? GetDpiForWindow(h) : 0;
  return d ? (int)d : 96;
}

// Edit controls want \r\n; the document stores \n.
inline std::wstring toEditText(const std::string& utf8) {
  std::wstring in = widen(utf8), out;
  out.reserve(in.size() + 8);
  for (wchar_t c : in) {
    if (c == L'\n') out += L'\r';
    out += c;
  }
  return out;
}
inline std::string fromEditText(const std::wstring& w) {
  std::wstring out;
  out.reserve(w.size());
  for (wchar_t c : w)
    if (c != L'\r') out += c;
  return narrow(out);
}

}  // namespace cairn

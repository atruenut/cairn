// Finds web links inside plain text. Header-only and templated so the same code
// serves UTF-8 (tests, core) and UTF-16 (the Win32 canvas); it only ever compares
// against ASCII. Recognised: http://, https://, mailto:, and bare www. (opened as https).
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cairn {

namespace urls_detail {

template <class C> inline bool isSpace(C c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
template <class C> inline bool isAlnum(C c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
template <class C> inline C lower(C c) { return (c >= 'A' && c <= 'Z') ? (C)(c - 'A' + 'a') : c; }

template <class S> inline bool startsWithAt(const S& s, size_t i, const char* lit) {
  for (size_t k = 0; lit[k]; ++k)
    if (i + k >= s.size() || lower(s[i + k]) != (typename S::value_type)lit[k]) return false;
  return true;
}

}  // namespace urls_detail

// Half-open [start, end) character ranges of links in `s`, in order, non-overlapping.
template <class S>
std::vector<std::pair<size_t, size_t>> findUrls(const S& s) {
  using namespace urls_detail;
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0;
  while (i < s.size()) {
    bool boundary = (i == 0) || !isAlnum(s[i - 1]);
    size_t plen = 0;
    if (boundary) {
      if (startsWithAt(s, i, "https://")) plen = 8;
      else if (startsWithAt(s, i, "http://")) plen = 7;
      else if (startsWithAt(s, i, "mailto:")) plen = 7;
      else if (startsWithAt(s, i, "www.")) plen = 4;
    }
    if (!plen) { ++i; continue; }
    size_t e = i + plen;
    while (e < s.size() && !isSpace(s[e]) && s[e] != '<' && s[e] != '>' && s[e] != '"') ++e;
    // Trim punctuation that belongs to the sentence, not the link.
    while (e > i + plen) {
      auto c = s[e - 1];
      if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'') { --e; continue; }
      if (c == ')' || c == ']' || c == '}') {
        auto open = c == ')' ? '(' : c == ']' ? '[' : '{';
        int bal = 0;
        for (size_t k = i; k < e; ++k) {
          if (s[k] == open) ++bal;
          else if (s[k] == c) --bal;
        }
        if (bal < 0) { --e; continue; }
      }
      break;
    }
    bool hasBody = false;  // "https://" alone, or "www." alone, is not a link
    for (size_t k = i + plen; k < e; ++k)
      if (isAlnum(s[k])) { hasBody = true; break; }
    if (hasBody) {
      out.emplace_back(i, e);
      i = e;
    } else {
      i += plen;
    }
  }
  return out;
}

// What to hand to the OS for a matched link ("www.x.org" -> "https://www.x.org").
template <class S>
S urlTarget(const S& link) {
  if (urls_detail::startsWithAt(link, 0, "www.")) {
    S r;
    for (const char* c = "https://"; *c; ++c) r.push_back((typename S::value_type)*c);
    return r + link;
  }
  return link;
}

}  // namespace cairn

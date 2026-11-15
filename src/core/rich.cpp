#include <cstdio>
#include "rich.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace cairn {

// ------------------------------------------------------------------ storage

static std::string pctEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (c <= 0x20 || c >= 0x7F || c == '%' || c == ',' || c == '|') {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    } else {
      o += (char)c;
    }
  }
  return o;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static std::string pctDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
      o += (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::string encodeRuns(const Runs& runs) {
  std::string o;
  for (const Run& r : runs) {
    if (r.isDefault() || r.len == 0) continue;
    if (!o.empty()) o += '|';
    o += std::to_string(r.start) + "," + std::to_string(r.len) + "," + std::to_string(r.flags) + "," +
         (r.color >= 0 ? std::to_string(r.color) : "") + "," + (r.size ? std::to_string(r.size) : "") + "," +
         pctEncode(r.url);
  }
  return o;
}

static bool parseNum(const std::string& s, long long& out) {
  if (s.empty() || s.size() > 12) return false;
  char* e = nullptr;
  out = std::strtoll(s.c_str(), &e, 10);
  return e && *e == 0;
}

Runs decodeRuns(const std::string& s, size_t textLen) {
  Runs out;
  size_t i = 0;
  while (i <= s.size() && !s.empty() && out.size() < 100000) {
    size_t e = s.find('|', i);
    if (e == std::string::npos) e = s.size();
    std::string item = s.substr(i, e - i);
    i = e + 1;
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
      size_t c = item.find(',', p);
      if (c == std::string::npos) { f.push_back(item.substr(p)); break; }
      f.push_back(item.substr(p, c - p));
      p = c + 1;
    }
    if (f.size() != 6) continue;
    long long st, ln, fl, col = -1, sz = 0;
    if (!parseNum(f[0], st) || !parseNum(f[1], ln) || !parseNum(f[2], fl)) continue;
    if (!f[3].empty() && !parseNum(f[3], col)) continue;
    if (!f[4].empty() && !parseNum(f[4], sz)) continue;
    if (st < 0 || ln <= 0 || fl < 0 || col < -1 || col > 0xFFFFFF || sz < 0) continue;
    Run r;
    r.start = (size_t)st;
    r.len = (size_t)ln;
    r.flags = (unsigned)fl & (kBold | kItalic | kUnderline | kStrike | kMono);
    r.color = (int)col;
    r.size = (int)std::clamp<long long>(sz, 0, 400);
    r.url = pctDecode(f[5]);
    out.push_back(std::move(r));
  }
  normalizeRuns(out, textLen);
  return out;
}

void normalizeRuns(Runs& runs, size_t textLen) {
  std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.start < b.start; });
  Runs out;
  size_t cursor = 0;
  for (Run& r : runs) {
    if (r.start < cursor) {  // overlap: keep the earlier run, trim this one's front
      size_t cut = cursor - r.start;
      if (cut >= r.len) continue;
      r.start += cut;
      r.len -= cut;
    }
    if (r.start >= textLen) continue;
    if (r.end() > textLen) r.len = textLen - r.start;
    if (r.len == 0 || r.isDefault()) continue;
    if (!out.empty() && out.back().end() == r.start && out.back().sameStyle(r)) {
      out.back().len += r.len;
    } else {
      out.push_back(r);
    }
    cursor = r.end();
  }
  runs = std::move(out);
}

// ------------------------------------------------------------------ editing

static bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }

void shiftRunsForEdit(Runs& runs, const std::string& oldT, const std::string& newT) {
  if (oldT == newT) return;
  size_t mn = std::min(oldT.size(), newT.size());
  size_t p = 0;
  while (p < mn && oldT[p] == newT[p]) ++p;
  while (p > 0 && ((p < oldT.size() && isCont((unsigned char)oldT[p])) ||
                   (p < newT.size() && isCont((unsigned char)newT[p]))))
    --p;
  size_t s = 0;
  while (s < mn - p && oldT[oldT.size() - 1 - s] == newT[newT.size() - 1 - s]) ++s;
  while (s > 0 && ((oldT.size() - s < oldT.size() && isCont((unsigned char)oldT[oldT.size() - s])) ||
                   (newT.size() - s < newT.size() && isCont((unsigned char)newT[newT.size() - s]))))
    --s;
  size_t oldEnd = oldT.size() - s, ins = newT.size() - s - p;
  long long delta = (long long)ins - (long long)(oldEnd - p);
  bool pureInsert = oldEnd == p;

  Runs out;
  for (const Run& r : runs) {
    size_t a = r.start, b = r.end();
    if (b < p) { out.push_back(r); continue; }
    if (b == p) {  // typing right after a styled run continues it (links don't grow)
      Run x = r;
      if (pureInsert && ins > 0 && r.url.empty()) x.len += ins;
      out.push_back(x);
      continue;
    }
    if (a >= oldEnd) {  // entirely after the change: just moves
      Run x = r;
      x.start = (size_t)((long long)a + delta);
      out.push_back(x);
      continue;
    }
    if (a < p && b > oldEnd) {  // the change is inside this run: it stays one run
      Run x = r;
      x.len = (size_t)((long long)r.len + delta);
      out.push_back(x);
      continue;
    }
    if (a < p) {  // keeps its front part
      Run x = r;
      x.len = p - a;
      out.push_back(x);
    }
    if (b > oldEnd) {  // keeps its back part
      Run x = r;
      x.start = (size_t)((long long)oldEnd + delta);
      x.len = b - oldEnd;
      out.push_back(x);
    }
  }
  normalizeRuns(out, newT.size());
  runs = std::move(out);
}

static void splitAt(Runs& runs, size_t pos) {
  for (size_t i = 0; i < runs.size(); ++i) {
    if (runs[i].start < pos && pos < runs[i].end()) {
      Run tail = runs[i];
      tail.start = pos;
      tail.len = runs[i].end() - pos;
      runs[i].len = pos - runs[i].start;
      runs.insert(runs.begin() + (long)i + 1, tail);
      return;
    }
  }
}

void restyleRange(Runs& runs, size_t a, size_t b, size_t textLen, const std::function<void(Run&)>& fn) {
  b = std::min(b, textLen);
  if (a >= b) return;
  normalizeRuns(runs, textLen);
  splitAt(runs, a);
  splitAt(runs, b);
  Runs out;
  size_t cursor = a;
  auto gap = [&](size_t to) {
    if (cursor < to) {
      Run g;
      g.start = cursor;
      g.len = to - cursor;
      fn(g);
      out.push_back(g);
    }
  };
  for (const Run& r : runs) {
    if (r.end() <= a || r.start >= b) { out.push_back(r); continue; }
    gap(r.start);
    Run x = r;
    fn(x);
    out.push_back(x);
    cursor = r.end();
  }
  gap(b);
  normalizeRuns(out, textLen);
  runs = std::move(out);
}

bool rangeAllHave(const Runs& runs, size_t a, size_t b, unsigned flag) {
  size_t cursor = a;
  for (const Run& r : runs) {
    if (r.end() <= a) continue;
    if (r.start >= b) break;
    if (r.start > cursor || !(r.flags & flag)) return false;
    cursor = r.end();
  }
  return cursor >= b;
}

void toggleFlag(Runs& runs, size_t a, size_t b, size_t textLen, unsigned flag) {
  bool all = rangeAllHave(runs, a, std::min(b, textLen), flag);
  restyleRange(runs, a, b, textLen, [&](Run& r) { r.flags = all ? (r.flags & ~flag) : (r.flags | flag); });
}

void overlayRuns(Runs& base, const Runs& over, size_t offset, size_t overLen, size_t textLen) {
  restyleRange(base, offset, offset + overLen, textLen, [](Run& r) { r = Run{r.start, r.len, 0, -1, 0, ""}; });
  for (Run r : over) {
    r.start += offset;
    base.push_back(r);
  }
  normalizeRuns(base, textLen);
}

std::string sanitizeLinkTarget(const std::string& typed) {
  std::string s = typed;
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  if (s.empty() || s.size() > 2048) return "";
  for (unsigned char ch : s)
    if (ch <= 0x20 || ch == 0x7F) return "";
  std::string lo = s;
  for (char& ch : lo) ch = (char)std::tolower((unsigned char)ch);
  auto has = [&](const char* p) { return lo.compare(0, std::strlen(p), p) == 0; };
  if (has("https://") || has("http://")) return lo.size() > 8 ? s : "";
  if (has("mailto:")) return lo.size() > 7 ? s : "";
  if (lo.find(':') != std::string::npos) return "";  // some other scheme
  if (lo.find('.') == std::string::npos || lo[0] == '.' || lo[0] == '/') return "";
  return "https://" + s;
}

// ------------------------------------------------------------------ HTML

namespace {

std::string lowerStr(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

void putUtf8(std::string& o, unsigned long cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}

// Decodes &entity; at s[i] ('&'); returns chars consumed (0 if not an entity).
size_t decodeEntity(const std::string& s, size_t i, std::string& out) {
  static const std::map<std::string, unsigned long> named = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
      {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014},
      {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
      {"bull", 0x2022}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"euro", 0x20AC},
      {"pound", 0xA3}, {"yen", 0xA5}, {"times", 0xD7}, {"deg", 0xB0}, {"larr", 0x2190},
      {"rarr", 0x2192}, {"para", 0xB6}, {"sect", 0xA7}, {"cent", 0xA2}};
  size_t semi = s.find(';', i);
  if (semi == std::string::npos || semi - i > 10 || semi == i + 1) return 0;
  std::string name = s.substr(i + 1, semi - i - 1);
  if (name[0] == '#') {
    unsigned long cp = 0;
    bool ok = name.size() > 1;
    if (ok && (name[1] == 'x' || name[1] == 'X')) {
      ok = name.size() > 2;
      for (size_t k = 2; ok && k < name.size(); ++k) {
        int h = hexv(name[k]);
        if (h < 0) ok = false; else cp = cp * 16 + (unsigned)h;
      }
    } else {
      for (size_t k = 1; ok && k < name.size(); ++k) {
        if (!std::isdigit((unsigned char)name[k])) ok = false; else cp = cp * 10 + (unsigned)(name[k] - '0');
      }
    }
    if (!ok) return 0;
    if (cp == 0xA0) cp = ' ';
    putUtf8(out, cp);
    return semi - i + 1;
  }
  auto it = named.find(name);
  if (it == named.end()) return 0;
  putUtf8(out, it->second);
  return semi - i + 1;
}

struct Frame {
  std::string tag;
  unsigned flags = 0;
  int color = -1;
  int size = 0;
  std::string url;
  bool skip = false;   // hidden subtree
  bool pre = false;
  bool ordered = false;
  int counter = 0;
};

bool parseColor(std::string v, int& out) {
  v = lowerStr(v);
  while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
  while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
  static const std::map<std::string, int> names = {
      {"black", 0}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
      {"gray", 0x808080}, {"grey", 0x808080}, {"orange", 0xFFA500}, {"purple", 0x800080},
      {"yellow", 0xFFFF00}, {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000},
      {"brown", 0xA52A2A}, {"pink", 0xFFC0CB}, {"crimson", 0xDC143C}, {"darkred", 0x8B0000},
      {"darkgreen", 0x006400}, {"darkblue", 0x00008B}};
  if (!v.empty() && v[0] == '#') {
    std::string h = v.substr(1);
    for (char c : h) if (hexv(c) < 0) return false;
    if (h.size() == 3) { out = (hexv(h[0]) * 17 << 16) | (hexv(h[1]) * 17 << 8) | (hexv(h[2]) * 17); return true; }
    if (h.size() == 6 || h.size() == 8) {
      out = (int)(std::strtoul(h.substr(0, 6).c_str(), nullptr, 16) & 0xFFFFFF);
      return true;
    }
    return false;
  }
  if (v.compare(0, 4, "rgb(") == 0 || v.compare(0, 5, "rgba(") == 0) {
    size_t o = v.find('(');
    int comp[3] = {0, 0, 0}, n = 0;
    const char* p = v.c_str() + o + 1;
    while (n < 3 && *p) {
      char* e = nullptr;
      double d = std::strtod(p, &e);
      if (e == p) return false;
      if (*e == '%') { d = d * 2.55; ++e; }
      comp[n++] = (int)std::clamp(d + 0.5, 0.0, 255.0);
      while (*e == ' ' || *e == ',' || *e == '/') ++e;
      p = e;
    }
    if (n < 3) return false;
    out = (comp[0] << 16) | (comp[1] << 8) | comp[2];
    return true;
  }
  auto it = names.find(v);
  if (it == names.end()) return false;
  out = it->second;
  return true;
}

// Near-black/near-white greys are what every page uses for body text; keeping them would
// pin pasted text to one colour regardless of the object's own colour (and its background).
bool isNeutralTextColor(int c) {
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int mx = std::max({r, g, b}), mn = std::min({r, g, b});
  int lum = (r * 299 + g * 587 + b * 114) / 1000;
  return (mx - mn) < 40 && (lum < 90 || lum > 225);
}

void applyStyleAttr(const std::string& style, Frame& f) {
  size_t i = 0;
  while (i < style.size()) {
    size_t e = style.find(';', i);
    if (e == std::string::npos) e = style.size();
    std::string decl = style.substr(i, e - i);
    i = e + 1;
    size_t c = decl.find(':');
    if (c == std::string::npos) continue;
    std::string k = lowerStr(decl.substr(0, c)), v = lowerStr(decl.substr(c + 1));
    while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
    while (!k.empty() && std::isspace((unsigned char)k.front())) k.erase(k.begin());
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    if (k == "color") {
      int col;
      if (parseColor(v, col) && !isNeutralTextColor(col)) f.color = col;
    } else if (k == "font-weight") {
      if (v == "bold" || v == "bolder" || (std::atoi(v.c_str()) >= 600)) f.flags |= kBold;
    } else if (k == "font-style") {
      if (v == "italic" || v == "oblique") f.flags |= kItalic;
    } else if (k == "text-decoration" || k == "text-decoration-line") {
      if (v.find("underline") != std::string::npos) f.flags |= kUnderline;
      if (v.find("line-through") != std::string::npos) f.flags |= kStrike;
    } else if (k == "font-family") {
      if (v.find("mono") != std::string::npos || v.find("consolas") != std::string::npos ||
          v.find("courier") != std::string::npos)
        f.flags |= kMono;
    } else if (k == "display") {
      if (v == "none") f.skip = true;
    } else if (k == "font-size") {
      double num = std::atof(v.c_str());
      int pct = 0;
      if (v.find("px") != std::string::npos) pct = (int)(num * 100 / 16 + 0.5);
      else if (v.find("pt") != std::string::npos) pct = (int)(num * 100 / 12 + 0.5);
      else if (v.find("em") != std::string::npos || v.find('%') != std::string::npos) {
        pct = v.find('%') != std::string::npos ? (int)(num + 0.5) : (int)(num * 100 + 0.5);
      }
      if (pct > 0) {
        pct = std::clamp(pct, 50, 300);
        if (std::abs(pct - 100) >= 10) f.size = pct;
      }
    }
  }
}

struct Attr { std::string k, v; };

}  // namespace

std::string htmlFragmentFromClipboard(const std::string& clip, std::string* sourceUrl) {
  auto header = [&](const char* key) -> long long {
    size_t p = clip.find(key);
    if (p == std::string::npos || p > 2000) return -1;
    return std::atoll(clip.c_str() + p + std::strlen(key));
  };
  if (sourceUrl) {
    size_t p = clip.find("SourceURL:");
    if (p != std::string::npos && p < 2000) {
      size_t e = clip.find_first_of("\r\n", p);
      *sourceUrl = clip.substr(p + 10, e == std::string::npos ? std::string::npos : e - p - 10);
    }
  }
  long long a = header("StartFragment:"), b = header("EndFragment:");
  if (a >= 0 && b > a && (size_t)b <= clip.size()) return clip.substr((size_t)a, (size_t)(b - a));
  size_t m1 = clip.find("<!--StartFragment-->"), m2 = clip.find("<!--EndFragment-->");
  if (m1 != std::string::npos && m2 != std::string::npos && m2 > m1) return clip.substr(m1 + 20, m2 - m1 - 20);
  return clip;
}

RichText htmlToRich(const std::string& htmlIn, const std::string& sourceUrl) {
  static const size_t kMaxIn = 4u << 20, kMaxText = 1u << 20, kMaxDepth = 256;
  const std::string& html = htmlIn.size() > kMaxIn ? htmlIn.substr(0, kMaxIn) : htmlIn;

  std::string origin;
  {
    size_t sch = sourceUrl.find("://");
    if (sch != std::string::npos) {
      size_t slash = sourceUrl.find('/', sch + 3);
      origin = sourceUrl.substr(0, slash == std::string::npos ? sourceUrl.size() : slash);
    }
  }

  RichText rt;
  std::string& text = rt.text;
  std::vector<Frame> stack(1);
  int pendingNl = 0;  // newlines owed before the next text
  bool atLineStart = true, lastSpace = true;
  int listDepth = 0;

  auto effective = [&](Run& r) {
    r = Run{};
    for (const Frame& f : stack) {
      r.flags |= f.flags;
      if (f.color >= 0) r.color = f.color;
      if (f.size) r.size = f.size;
      if (!f.url.empty()) r.url = f.url;
    }
  };
  auto skipping = [&]() {
    for (const Frame& f : stack) if (f.skip) return true;
    return false;
  };
  auto inPre = [&]() {
    for (const Frame& f : stack) if (f.pre) return true;
    return false;
  };
  auto emit = [&](const std::string& s) {
    if (s.empty() || text.size() + s.size() > kMaxText) return;
    if (pendingNl > 0 && !text.empty()) {
      size_t have = 0;
      while (have < text.size() && text[text.size() - 1 - have] == '\n') ++have;
      for (size_t k = have; k < (size_t)pendingNl; ++k) text += '\n';
    }
    pendingNl = 0;
    Run r;
    effective(r);
    size_t start = text.size();
    text += s;
    if (!r.isDefault()) {
      r.start = start;
      r.len = s.size();
      if (!rt.runs.empty() && rt.runs.back().end() == start && rt.runs.back().sameStyle(r)) rt.runs.back().len += s.size();
      else rt.runs.push_back(r);
    }
    atLineStart = text.back() == '\n';
    lastSpace = text.back() == ' ' || atLineStart;
  };
  auto needNl = [&](int n) { pendingNl = std::max(pendingNl, n); };

  size_t i = 0;
  while (i < html.size()) {
    if (html[i] == '<') {
      if (html.compare(i, 4, "<!--") == 0) {
        size_t e = html.find("-->", i + 4);
        i = e == std::string::npos ? html.size() : e + 3;
        continue;
      }
      size_t e = i + 1;
      char q = 0;
      while (e < html.size() && (q || html[e] != '>')) {
        if (q) { if (html[e] == q) q = 0; }
        else if (html[e] == '"' || html[e] == '\'') q = html[e];
        ++e;
      }
      std::string raw = html.substr(i + 1, e - i - 1);
      i = e < html.size() ? e + 1 : html.size();
      if (raw.empty() || raw[0] == '!' || raw[0] == '?') continue;
      bool closing = raw[0] == '/';
      if (closing) raw.erase(0, 1);
      bool selfClose = !raw.empty() && raw.back() == '/';
      size_t ne = 0;
      while (ne < raw.size() && !std::isspace((unsigned char)raw[ne]) && raw[ne] != '/') ++ne;
      std::string tag = lowerStr(raw.substr(0, ne));
      if (tag.empty()) continue;

      std::vector<Attr> attrs;
      if (!closing) {
        size_t p = ne;
        while (p < raw.size()) {
          while (p < raw.size() && (std::isspace((unsigned char)raw[p]) || raw[p] == '/')) ++p;
          size_t ks = p;
          while (p < raw.size() && !std::isspace((unsigned char)raw[p]) && raw[p] != '=' && raw[p] != '/') ++p;
          if (p == ks) break;
          Attr a;
          a.k = lowerStr(raw.substr(ks, p - ks));
          while (p < raw.size() && std::isspace((unsigned char)raw[p])) ++p;
          if (p < raw.size() && raw[p] == '=') {
            ++p;
            while (p < raw.size() && std::isspace((unsigned char)raw[p])) ++p;
            if (p < raw.size() && (raw[p] == '"' || raw[p] == '\'')) {
              char qq = raw[p++];
              size_t vs = p;
              while (p < raw.size() && raw[p] != qq) ++p;
              a.v = raw.substr(vs, p - vs);
              if (p < raw.size()) ++p;
            } else {
              size_t vs = p;
              while (p < raw.size() && !std::isspace((unsigned char)raw[p])) ++p;
              a.v = raw.substr(vs, p - vs);
            }
          }
          attrs.push_back(std::move(a));
        }
      }

      static const char* voids[] = {"br", "hr", "img", "input", "meta", "link", "wbr", "area", "base", "col", "source", "embed", "param", "track"};
      bool isVoid = false;
      for (const char* v : voids) if (tag == v) isVoid = true;
      static const char* blocks[] = {"div", "section", "article", "header", "footer", "nav", "main", "aside", "table", "tr", "ul", "ol", "dl", "dt", "dd", "figure", "figcaption", "form", "fieldset", "address", "details", "summary"};
      bool isBlock = false;
      for (const char* v : blocks) if (tag == v) isBlock = true;
      bool isPara = tag == "p" || tag == "blockquote" || tag == "pre" || tag == "hr";
      bool isHead = tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';

      if (closing) {
        for (size_t k = stack.size(); k-- > 1;) {
          if (stack[k].tag == tag) {
            if (tag == "ul" || tag == "ol") listDepth = std::max(0, listDepth - 1);
            stack.resize(k);
            break;
          }
        }
        if (isPara || isHead) needNl(2);
        else if (isBlock || tag == "li") needNl(1);
        else if ((tag == "td" || tag == "th") && !skipping() && !atLineStart && !text.empty()) emit("  ");
        continue;
      }

      if (skipping()) {
        if (!isVoid && !selfClose && stack.size() < kMaxDepth) { Frame f; f.tag = tag; f.skip = true; stack.push_back(f); }
        continue;
      }
      if (tag == "script" || tag == "style" || tag == "head" || tag == "title" || tag == "noscript" || tag == "template" || tag == "svg") {
        if (!selfClose) {
          std::string needle = "</" + tag;  // skip the raw contents wholesale
          size_t k = i;
          for (; k + needle.size() <= html.size(); ++k) {
            size_t m = 0;
            while (m < needle.size() && std::tolower((unsigned char)html[k + m]) == needle[m]) ++m;
            if (m == needle.size()) break;
          }
          if (k + needle.size() > html.size()) { i = html.size(); }
          else {
            size_t gt = html.find('>', k);
            i = gt == std::string::npos ? html.size() : gt + 1;
          }
        }
        continue;
      }

      if (tag == "br") { emit("\n"); pendingNl = 0; continue; }
      if (tag == "hr") { needNl(2); continue; }

      Frame f;
      f.tag = tag;
      if (isPara) needNl(2);
      else if (isBlock) needNl(1);
      if (isHead) {
        f.flags |= kBold;
        static const int sz[] = {0, 160, 140, 120, 110, 100, 100};
        f.size = sz[tag[1] - '0'];
        if (f.size == 100) f.size = 0;
        needNl(2);
      }
      if (tag == "b" || tag == "strong" || tag == "th") f.flags |= kBold;
      if (tag == "i" || tag == "em" || tag == "cite" || tag == "dfn" || tag == "var") f.flags |= kItalic;
      if (tag == "u" || tag == "ins") f.flags |= kUnderline;
      if (tag == "s" || tag == "strike" || tag == "del") f.flags |= kStrike;
      if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt" || tag == "pre") f.flags |= kMono;
      if (tag == "pre") f.pre = true;
      if (tag == "ul" || tag == "ol") {
        f.ordered = tag == "ol";
        ++listDepth;
      }
      for (const Attr& a : attrs) {
        if (a.k == "style") applyStyleAttr(a.v, f);
        else if (a.k == "hidden") f.skip = true;
        else if (a.k == "color" && tag == "font") {
          int col;
          if (parseColor(a.v, col) && !isNeutralTextColor(col)) f.color = col;
        } else if (a.k == "href" && tag == "a") {
          std::string h = a.v, dec;
          for (size_t k = 0; k < h.size();) {  // hrefs carry entities too
            if (h[k] == '&') { size_t n = decodeEntity(h, k, dec); if (n) { k += n; continue; } }
            dec += h[k++];
          }
          std::string lo = lowerStr(dec);
          while (!dec.empty() && std::isspace((unsigned char)dec.front())) { dec.erase(dec.begin()); lo.erase(lo.begin()); }
          if (lo.compare(0, 8, "https://") == 0 || lo.compare(0, 7, "http://") == 0 || lo.compare(0, 7, "mailto:") == 0) f.url = dec;
          else if (lo.compare(0, 2, "//") == 0) f.url = "https:" + dec;
          else if (!lo.empty() && lo[0] == '/' && !origin.empty()) f.url = origin + dec;
          for (char c : f.url) if ((unsigned char)c <= 0x20) { f.url.clear(); break; }
        }
      }
      if (f.url.size() > 2048) f.url.clear();
      if (tag == "li") {
        // bullet or number from the nearest list frame
        std::string marker = "• ";
        for (size_t k = stack.size(); k-- > 0;) {
          if (stack[k].tag == "ul" || stack[k].tag == "ol") {
            if (stack[k].ordered) marker = std::to_string(++stack[k].counter) + ". ";
            break;
          }
        }
        needNl(1);
        std::string indent((size_t)std::max(0, listDepth - 1) * 2, ' ');
        stack.push_back(f);
        // the marker itself is plain (unstyled) text
        std::vector<Frame> saved(stack.begin() + 1, stack.end());
        stack.resize(1);
        emit(indent + marker);
        stack.insert(stack.end(), saved.begin(), saved.end());
        lastSpace = true;
        continue;
      }
      if (!isVoid && !selfClose && stack.size() < kMaxDepth) stack.push_back(f);
    } else {
      size_t e = html.find('<', i);
      if (e == std::string::npos) e = html.size();
      std::string chunk = html.substr(i, e - i);
      i = e;
      if (skipping()) continue;
      std::string out;
      bool pre = inPre();
      for (size_t k = 0; k < chunk.size();) {
        char c = chunk[k];
        if (c == '&') {
          std::string ent;
          size_t n = decodeEntity(chunk, k, ent);
          if (n) {
            out += ent;
            k += n;
            continue;
          }
        }
        if (pre) {
          if (c != '\r') out += c;
          ++k;
          continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
          bool prevSpace = out.empty() ? (lastSpace || atLineStart || pendingNl > 0) : (out.back() == ' ' || out.back() == '\n');
          if (!prevSpace) out += ' ';
          ++k;
          continue;
        }
        out += c;
        ++k;
      }
      if (!out.empty()) {
        if (pendingNl > 0 || atLineStart) {  // no leading blanks after a break
          size_t s = 0;
          while (s < out.size() && out[s] == ' ' && !pre) ++s;
          out.erase(0, s);
        }
        emit(out);
      }
    }
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  normalizeRuns(rt.runs, text.size());
  return rt;
}

namespace {
void escapeHtml(const std::string& s, size_t a, size_t b, std::string& out) {
  for (size_t i = a; i < b; ++i) {
    char ch = s[i];
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\r': break;
      case '\n': out += "<br>"; break;
      default: out += ch;
    }
  }
}
}  // namespace

std::string richToHtml(const std::string& text, const Runs& runsIn) {
  Runs runs = runsIn;
  normalizeRuns(runs, text.size());
  std::string out;
  size_t pos = 0;
  auto plain = [&](size_t upTo) { escapeHtml(text, pos, upTo, out); pos = upTo; };
  for (const Run& r : runs) {
    if (r.start > pos) plain(r.start);
    std::string open, close;
    std::string url = r.url.empty() ? "" : sanitizeLinkTarget(r.url);
    if (!url.empty()) {
      std::string safe;
      escapeHtml(url, 0, url.size(), safe);
      open += "<a href=\"" + safe + "\">";
      close = "</a>" + close;
    }
    std::string style;
    if (r.color >= 0) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "color:#%06X;", r.color & 0xFFFFFF);
      style += buf;
    }
    if (r.size > 0) style += "font-size:" + std::to_string(r.size) + "%;";
    if (!style.empty()) {
      open += "<span style=\"" + style + "\">";
      close = "</span>" + close;
    }
    static const struct { unsigned f; const char *o, *c; } tags[] = {
        {kBold, "<b>", "</b>"}, {kItalic, "<i>", "</i>"}, {kUnderline, "<u>", "</u>"},
        {kStrike, "<s>", "</s>"}, {kMono, "<code>", "</code>"}};
    for (const auto& t : tags)
      if (r.flags & t.f) { open += t.o; close = std::string(t.c) + close; }
    out += open;
    pos = r.start;
    plain(r.end());
    out += close;
  }
  if (pos < text.size()) plain(text.size());
  return out;
}

std::string wrapClipboardHtml(const std::string& fragment) {
  // The header holds fixed-width byte offsets, so build it twice: once to measure.
  auto build = [&](size_t hs, size_t he, size_t fs, size_t fe) {
    char hdr[256];
    std::snprintf(hdr, sizeof hdr,
                  "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n",
                  hs, he, fs, fe);
    return std::string(hdr);
  };
  const std::string pre = "<html><body>\r\n<!--StartFragment-->", post = "<!--EndFragment-->\r\n</body></html>";
  size_t hlen = build(0, 0, 0, 0).size();
  size_t fs = hlen + pre.size(), fe = fs + fragment.size(), he = fe + post.size();
  return build(hlen, he, fs, fe) + pre + fragment + post;
}

namespace {
template <size_t N>
int stepLadder(const int (&ladder)[N], int cur, int dir) {
  if (dir > 0) {
    for (int v : ladder) if (v > cur) return v;
    return ladder[N - 1];
  }
  for (size_t i = N; i-- > 0;) if (ladder[i] < cur) return ladder[i];
  return ladder[0];
}
}  // namespace

int stepFontPt(int cur, int dir) {
  static const int ladder[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 36, 48, 60, 72, 96};
  return stepLadder(ladder, cur, dir);
}

int stepSizePercent(int cur, int dir) {
  static const int ladder[] = {50, 70, 85, 100, 125, 150, 200, 300, 400};
  if (cur <= 0) cur = 100;
  return stepLadder(ladder, cur, dir);
}

}  // namespace cairn

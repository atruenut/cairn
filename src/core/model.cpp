#include "model.h"

#include <algorithm>

namespace cairn {

// Values may contain newlines, so escape \ \n \r. Keys are plain identifiers.
static std::string esc(const std::string& v) {
  std::string o;
  o.reserve(v.size());
  for (char c : v) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

static std::string unesc(const std::string& v) {
  std::string o;
  o.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      if (n == 'n') o += '\n';
      else if (n == 'r') o += '\r';
      else o += n;  // covers \\ and any unknown escape
    } else {
      o += v[i];
    }
  }
  return o;
}

std::string encodeProps(const PropMap& m) {
  std::string out;
  for (const auto& kv : m) {
    out += kv.first;
    out += '=';
    out += esc(kv.second);
    out += '\n';
  }
  return out;
}

PropMap decodeProps(const std::string& s) {
  PropMap m;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(pos, nl - pos);
    pos = nl + 1;
    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    m[line.substr(0, eq)] = unesc(line.substr(eq + 1));
  }
  return m;
}

static std::string lowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

std::vector<std::string> splitTags(const std::string& stored) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i <= stored.size(); ++i) {
    char c = i < stored.size() ? stored[i] : ',';
    if (c == ',' || c == ';' || c == '\n' || c == '\r') {
      size_t a = 0, b = cur.size();
      while (a < b && (cur[a] == ' ' || cur[a] == '\t' || cur[a] == '#')) ++a;
      while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
      if (b > a) out.push_back(cur.substr(a, b - a));
      cur.clear();
    } else if (c == '\t') {
      cur += ' ';
    } else if ((unsigned char)c >= 0x20) {
      if (c == ' ' && !cur.empty() && cur.back() == ' ') continue;  // collapse runs of spaces
      cur += c;
    }
  }
  return out;
}

std::string normalizeTags(const std::string& typed) {
  std::vector<std::string> seen, out;
  for (std::string t : splitTags(typed)) {
    // keep whole UTF-8 characters when trimming the length
    if (t.size() > 40) {
      size_t cut = 40;
      while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) --cut;
      t.resize(cut);
    }
    for (char& c : t)
      if (c == ' ') c = '-';  // one word, so "#tag" can name it in a search
    std::string key = lowerAscii(t);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    out.push_back(t);
    if (out.size() >= 24) break;
  }
  std::string r;
  for (size_t i = 0; i < out.size(); ++i) r += (i ? ", " : "") + out[i];
  return r;
}

}  // namespace cairn

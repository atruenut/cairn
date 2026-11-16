#include "table.h"

#include <algorithm>
#include <cctype>

namespace cairn {
namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

// Shortens s to at most max bytes without leaving half of a multi-byte character.
void cutUtf8(std::string& s, size_t max) {
  if (s.size() <= max) return;
  size_t n = max;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;  // s[n] would start in the middle of a character
  s.resize(n);
}

bool isRuleLine(const std::string& line) {
  bool any = false;
  for (char c : line) {
    if (c == '-') any = true;
    else if (c != '|' && c != ':' && c != ' ' && c != '\t') return false;
  }
  return any;
}

std::string escCell(const std::string& c) {
  std::string o;
  for (char ch : c) {
    if (ch == '|' || ch == '\\') o += '\\';
    o += ch == '\n' || ch == '\r' ? ' ' : ch;
  }
  return o;
}

}  // namespace

std::string cellInline(const std::string& cell) {
  std::string o;
  for (char ch : cell) o += (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
  return o;
}

Table parseTable(const std::string& text, bool skipRules) {
  Table t;
  size_t pos = 0;
  while (pos <= text.size() && t.rows.size() < kTableMaxRows) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) continue;
    if (skipRules && isRuleLine(line)) continue;
    std::vector<std::string> cells;
    std::string cur;
    bool lead = true;  // still before the first non-space character
    bool closed = false;  // the line ended with an unescaped bar
    for (size_t i = 0; i < line.size(); ++i) {
      char c = line[i];
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '|' || line[i + 1] == '\\')) {
        cur += line[++i];
        lead = false;
        closed = false;
      } else if (c == '|') {
        if (!(lead && cells.empty())) cells.push_back(trim(cur));  // a leading bar opens nothing
        cur.clear();
        lead = false;
        closed = true;
      } else {
        if (c != ' ' && c != '\t') { lead = false; closed = false; }
        cur += c;
      }
    }
    if (!closed || !trim(cur).empty()) cells.push_back(trim(cur));
    if (cells.size() > kTableMaxCols) cells.resize(kTableMaxCols);
    for (std::string& c : cells) cutUtf8(c, kTableMaxCell);
    t.rows.push_back(std::move(cells));
  }
  size_t w = 0;
  for (auto& r : t.rows) w = std::max(w, r.size());
  for (auto& r : t.rows) r.resize(w);
  return t;
}

std::string formatTable(const Table& t) {
  std::string o;
  for (const auto& r : t.rows) {
    o += "|";
    for (const std::string& c : r) o += " " + escCell(c) + " |";
    o += "\n";
  }
  return o;
}

std::string tableToMarkdown(const Table& t, bool header) {
  (void)header;  // GitHub tables always have a header row; the first row is used
  if (t.rows.empty() || t.cols() == 0) return "";
  std::string o;
  auto row = [&](const std::vector<std::string>& r) {
    o += "|";
    for (const std::string& c : r) {
      std::string s;
      for (char ch : cellInline(c)) {
        if (ch == '|' || ch == '\\') s += '\\';
        s += ch;
      }
      o += " " + s + " |";
    }
    o += "\n";
  };
  row(t.rows[0]);
  o += "|";
  for (size_t i = 0; i < t.cols(); ++i) o += " --- |";
  o += "\n";
  for (size_t i = 1; i < t.rows.size(); ++i) row(t.rows[i]);
  return o;
}

std::string tableToHtml(const Table& t, bool header) {
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '&') o += "&amp;";
      else if (c == '<') o += "&lt;";
      else if (c == '>') o += "&gt;";
      else if (c == '"') o += "&quot;";
      else o += c;
    }
    return o;
  };
  std::string o = "<table>";
  for (size_t i = 0; i < t.rows.size(); ++i) {
    o += "<tr>";
    const char* tag = (header && i == 0) ? "th" : "td";
    for (const std::string& c : t.rows[i]) o += std::string("<") + tag + ">" + esc(c) + "</" + tag + ">";
    o += "</tr>";
  }
  return o + "</table>";
}

bool tableInsertRow(Table& t, size_t at) {
  if (at > t.rows.size() || t.rows.size() >= kTableMaxRows) return false;
  t.rows.insert(t.rows.begin() + (long)at, std::vector<std::string>(std::max<size_t>(1, t.cols())));
  return true;
}

bool tableDeleteRow(Table& t, size_t at) {
  if (at >= t.rows.size() || t.rows.size() <= 1) return false;
  t.rows.erase(t.rows.begin() + (long)at);
  return true;
}

bool tableInsertCol(Table& t, size_t at) {
  if (t.rows.empty()) t.rows.emplace_back();
  if (at > t.cols() || t.cols() >= kTableMaxCols) return false;
  for (auto& r : t.rows) r.insert(r.begin() + (long)at, std::string());
  return true;
}

bool tableDeleteCol(Table& t, size_t at) {
  if (at >= t.cols() || t.cols() <= 1) return false;
  for (auto& r : t.rows) r.erase(r.begin() + (long)at);
  return true;
}

bool tableSetCell(Table& t, size_t row, size_t col, const std::string& text) {
  if (row >= t.rows.size() || col >= t.rows[row].size()) return false;
  std::string c = cellInline(text);
  cutUtf8(c, kTableMaxCell);
  t.rows[row][col] = trim(c);
  return true;
}

}  // namespace cairn

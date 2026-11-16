#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "document.h"

namespace cairn {
namespace {

constexpr size_t kMaxInput = 2u << 20;
constexpr size_t kSearchWindow = 1000;  // how far an opener looks for its closer
constexpr int kMaxDepth = 6;
constexpr size_t kMaxBlock = 50000;
constexpr int kMaxPages = 200, kMaxObjectsPerPage = 400;

bool isPunct(char c) { return std::ispunct((unsigned char)c) != 0; }
bool isAlnumAscii(char c) { return std::isalnum((unsigned char)c) != 0; }

// ------------------------------------------------------------------ export
std::string escapeMd(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool lineStart = (i == 0 || s[i - 1] == '\n');
    if (c == '\\' || c == '*' || c == '_' || c == '`' || c == '[' || c == ']' || c == '~') o += '\\';
    else if (lineStart && (c == '#' || c == '>')) o += '\\';
    else if (lineStart && (c == '-' || c == '+') && i + 1 < s.size() && s[i + 1] == ' ') o += '\\';
    else if (lineStart && std::isdigit((unsigned char)c)) {  // "1. " would become a list
      size_t j = i;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
      if (j + 1 < s.size() && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') {
        o.append(s, i, j - i);
        o += '\\';
        i = j - 1 + 0;
        continue;
      }
    }
    o += c;
  }
  return o;
}

std::string encodeUrl(const std::string& u) {
  std::string o;
  for (char c : u) {
    if (c == ' ') o += "%20";
    else if (c == '(') o += "%28";
    else if (c == ')') o += "%29";
    else if (c == '\n' || c == '\r') continue;
    else o += c;
  }
  return o;
}

// One line-free piece of text with one style.
std::string styledPiece(const std::string& piece, unsigned flags, const std::string& url) {
  size_t a = 0, b = piece.size();
  while (a < b && std::isspace((unsigned char)piece[a])) ++a;
  while (b > a && std::isspace((unsigned char)piece[b - 1])) --b;
  if (a == b) return piece;  // only whitespace: nothing to mark
  std::string lead = piece.substr(0, a), trail = piece.substr(b), core = piece.substr(a, b - a);
  std::string safe = url.empty() ? "" : sanitizeLinkTarget(url);
  std::string inner;
  if (flags & kMono) {
    std::string fence = core.find('`') == std::string::npos ? "`" : "``";
    inner = fence + (core.front() == '`' || core.back() == '`' ? " " + core + " " : core) + fence;
  } else {
    inner = escapeMd(core);
  }
  if (flags & kStrike) inner = "~~" + inner + "~~";
  if ((flags & kBold) && (flags & kItalic)) inner = "***" + inner + "***";
  else if (flags & kBold) inner = "**" + inner + "**";
  else if (flags & kItalic) inner = "*" + inner + "*";
  if (!safe.empty()) inner = "[" + inner + "](" + encodeUrl(safe) + ")";
  return lead + inner + trail;
}

std::string slugOf(const std::string& title) {
  std::string s;
  for (char c : title) {
    if (isAlnumAscii(c)) s += (char)std::tolower((unsigned char)c);
    else if (c == ' ' || c == '-') s += '-';
    else if ((unsigned char)c >= 0x80) s += c;
  }
  return s;
}

// ------------------------------------------------------------------ inline parse
struct Ctx {
  std::string out;
  Runs runs;
};

void emit(Ctx& c, const std::string& s, unsigned flags, const std::string& url) {
  if (s.empty()) return;
  size_t start = c.out.size();
  c.out += s;
  if (!flags && url.empty()) return;
  if (!c.runs.empty() && c.runs.back().end() == start && c.runs.back().flags == flags && c.runs.back().url == url) {
    c.runs.back().len += s.size();
    return;
  }
  Run r;
  r.start = start;
  r.len = s.size();
  r.flags = flags;
  r.url = url;
  c.runs.push_back(r);
}

// Closing ")" of a link target that starts at `from`, allowing balanced inner parentheses.
size_t findCloseParen(const std::string& s, size_t from, size_t end) {
  size_t limit = std::min(end, from + kSearchWindow);
  int depth = 0;
  for (size_t k = from; k < limit; ++k) {
    if (s[k] == '(') ++depth;
    else if (s[k] == ')') {
      if (depth == 0) return k;
      --depth;
    }
  }
  return std::string::npos;
}

size_t findIn(const std::string& s, const std::string& what, size_t from, size_t end) {
  size_t limit = std::min(end, from + kSearchWindow);
  size_t j = s.find(what, from);
  return (j != std::string::npos && j + what.size() <= limit) ? j : std::string::npos;
}

void parseInl(const std::string& s, size_t i, size_t end, unsigned flags, const std::string& url, Ctx& c, int depth) {
  std::string lit;
  auto flush = [&] {
    emit(c, lit, flags, url);
    lit.clear();
  };
  while (i < end) {
    char ch = s[i];
    if (ch == '\\' && i + 1 < end && isPunct(s[i + 1])) {
      lit += s[i + 1];
      i += 2;
      continue;
    }
    if (ch == '`') {
      size_t ticks = 1;
      while (i + ticks < end && s[i + ticks] == '`') ++ticks;
      std::string fence(ticks, '`');
      size_t j = findIn(s, fence, i + ticks, end);
      if (j != std::string::npos && j > i + ticks) {
        flush();
        std::string code = s.substr(i + ticks, j - i - ticks);
        if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
        emit(c, code, flags | kMono, url);
        i = j + ticks;
        continue;
      }
      lit.append(ticks, '`');
      i += ticks;
      continue;
    }
    if (ch == '!' && i + 1 < end && s[i + 1] == '[') {  // image: keep only a visible placeholder
      size_t j = findIn(s, "]", i + 2, end);
      if (j != std::string::npos && j + 1 < end && s[j + 1] == '(') {
        size_t k = findCloseParen(s, j + 2, end);
        if (k != std::string::npos) {
          lit += "[image: " + s.substr(i + 2, j - i - 2) + "]";
          i = k + 1;
          continue;
        }
      }
    }
    if (ch == '[' && depth < kMaxDepth) {
      size_t j = findIn(s, "]", i + 1, end);
      if (j != std::string::npos && j + 1 < end && s[j + 1] == '(') {
        size_t k = findCloseParen(s, j + 2, end);
        if (k != std::string::npos) {
          std::string target = sanitizeLinkTarget(s.substr(j + 2, k - j - 2));
          flush();
          parseInl(s, i + 1, j, flags, target.empty() ? url : target, c, depth + 1);
          i = k + 1;
          continue;
        }
      }
    }
    if ((ch == '*' || ch == '_' || ch == '~') && depth < kMaxDepth) {
      size_t run = 1;
      while (i + run < end && s[i + run] == ch) ++run;
      bool leftOk = (ch != '_') || i == 0 || !isAlnumAscii(s[i - 1]);
      size_t take = 0;
      unsigned add = 0;
      if (ch == '~') {
        if (run >= 2) { take = 2; add = kStrike; }
      } else if (run >= 3) { take = 3; add = kBold | kItalic; }
      else if (run == 2) { take = 2; add = kBold; }
      else { take = 1; add = kItalic; }
      if (take && leftOk && i + take < end && !std::isspace((unsigned char)s[i + take])) {
        std::string delim(take, ch);
        size_t from = i + take;
        size_t j = std::string::npos;
        while (true) {
          j = findIn(s, delim, from, end);
          if (j == std::string::npos) break;
          bool prevOk = j > i + take && !std::isspace((unsigned char)s[j - 1]);
          bool exact = (j + take >= end || s[j + take] != ch) || take == 3;  // not the start of a longer run
          bool rightOk = (ch != '_') || j + take >= end || !isAlnumAscii(s[j + take]);
          if (prevOk && exact && rightOk) break;
          from = j + 1;
        }
        if (j != std::string::npos) {
          flush();
          parseInl(s, i + take, j, flags | add, url, c, depth + 1);
          i = j + take;
          continue;
        }
      }
      lit.append(run, ch);
      i += run;
      continue;
    }
    lit += ch;
    ++i;
  }
  flush();
}

// ------------------------------------------------------------------ import
struct Block {
  std::string text;
  Runs runs;
  int sizePt = 14;
  bool bold = false;
};

struct PendingPage {
  std::string title, tags;
  std::vector<Block> blocks;
  bool empty() const { return blocks.empty(); }
};

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

bool isRule(const std::string& t) {
  if (t.size() < 3) return false;
  char c = t[0];
  if (c != '-' && c != '*' && c != '_') return false;
  for (char x : t) if (x != c && x != ' ') return false;
  return true;
}

// "- x", "* x", "+ x", "1. x": returns the marker length and fills `bullet`.
size_t listMarker(const std::string& line, size_t& indent, std::string& bullet) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  indent = i;
  if (i + 1 < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '+') && line[i + 1] == ' ') {
    bullet = "\xE2\x80\xA2 ";
    return i + 2;
  }
  size_t j = i;
  while (j < line.size() && std::isdigit((unsigned char)line[j])) ++j;
  if (j > i && j - i < 10 && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
    bullet = line.substr(i, j - i) + ". ";
    return j + 2;
  }
  return 0;
}

void addBlock(PendingPage& pg, const std::string& text, bool mono, int sizePt, bool bold) {
  std::string t = text;
  while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
  if (t.empty()) return;
  for (size_t at = 0; at < t.size();) {  // very long blocks become several objects
    size_t n = std::min(kMaxBlock, t.size() - at);
    if (at + n < t.size()) {
      size_t nl = t.rfind('\n', at + n);
      if (nl != std::string::npos && nl > at) n = nl - at;
      while (at + n < t.size() && ((unsigned char)t[at + n] & 0xC0) == 0x80) --n;  // never split a character
      if (n == 0) n = std::min(kMaxBlock, t.size() - at);
    }
    std::string piece = t.substr(at, n);
    at += n;
    while (at < t.size() && t[at] == '\n') ++at;
    Block b;
    b.sizePt = sizePt;
    b.bold = bold;
    if (mono) {
      b.text = piece;
      Run r;
      r.len = piece.size();
      r.flags = kMono;
      b.runs.push_back(r);
    } else {
      MdInline in = parseMarkdownInline(piece);
      b.text = in.text;
      b.runs = in.runs;
    }
    if (!b.text.empty()) pg.blocks.push_back(std::move(b));
  }
}

std::vector<PendingPage> parseBlocks(const std::string& md, const std::string& fallbackTitle) {
  std::vector<std::string> lines;
  {
    std::string cur;
    for (size_t i = 0; i < md.size(); ++i) {
      if (md[i] == '\r') continue;
      if (md[i] == '\n') { lines.push_back(cur); cur.clear(); }
      else cur += md[i];
    }
    lines.push_back(cur);
  }
  std::vector<PendingPage> pages;
  PendingPage cur;
  cur.title = fallbackTitle;
  bool curIsFallback = true, justHeaded = false;
  auto closePage = [&] {
    if (!cur.empty() || !curIsFallback) pages.push_back(cur);
    cur = PendingPage();
  };

  std::string para;
  auto flushPara = [&] {
    addBlock(cur, para, false, 14, false);
    para.clear();
  };

  for (size_t li = 0; li < lines.size(); ++li) {
    const std::string& line = lines[li];
    std::string t = trim(line);
    if (justHeaded) {
      justHeaded = false;
      if (t.size() > 5 && (t[0] == 'T' || t[0] == 't') && (t.compare(1, 4, "ags:") == 0 || t.compare(1, 4, "AGS:") == 0)) {
        cur.tags = trim(t.substr(5));
        continue;
      }
    }
    if (startsWith(t, "```") || startsWith(t, "~~~")) {  // fenced code
      flushPara();
      std::string fence = t.substr(0, 3), code;
      size_t j = li + 1;
      for (; j < lines.size(); ++j) {
        if (startsWith(trim(lines[j]), fence.c_str())) break;
        code += lines[j] + "\n";
      }
      addBlock(cur, code, true, 14, false);
      li = j;
      continue;
    }
    if (t.empty()) { flushPara(); continue; }
    if (t[0] == '#') {
      size_t lv = 0;
      while (lv < t.size() && t[lv] == '#') ++lv;
      if (lv <= 6 && (lv == t.size() || t[lv] == ' ')) {
        flushPara();
        std::string head = trim(t.substr(lv));
        while (!head.empty() && head.back() == '#') head.pop_back();
        head = trim(head);
        if (lv == 1) {
          closePage();
          curIsFallback = false;
          cur.title = parseMarkdownInline(head).text;
          if (cur.title.empty()) cur.title = "Untitled";
          justHeaded = true;
        } else {
          addBlock(cur, head, false, lv == 2 ? 20 : lv == 3 ? 17 : 15, true);
        }
        continue;
      }
    }
    if (isRule(t)) { flushPara(); continue; }
    size_t indent = 0;
    std::string bullet;
    if (size_t m = listMarker(line, indent, bullet)) {  // a run of list items is one block
      flushPara();
      std::string items;
      size_t j = li;
      for (; j < lines.size(); ++j) {
        size_t ind2 = 0;
        std::string b2;
        size_t m2 = listMarker(lines[j], ind2, b2);
        if (!m2) break;
        items += std::string(std::min<size_t>(ind2 / 2, 6) * 2, ' ') + b2 + lines[j].substr(m2) + "\n";
      }
      addBlock(cur, items, false, 14, false);
      li = j - 1;
      (void)m;
      continue;
    }
    if (t[0] == '|') {  // table rows stay readable as fixed-width text
      flushPara();
      std::string rows;
      size_t j = li;
      for (; j < lines.size() && !trim(lines[j]).empty() && trim(lines[j])[0] == '|'; ++j) rows += trim(lines[j]) + "\n";
      addBlock(cur, rows, true, 13, false);
      li = j - 1;
      continue;
    }
    if (t[0] == '>') {
      flushPara();
      std::string q;
      size_t j = li;
      for (; j < lines.size(); ++j) {
        std::string u = trim(lines[j]);
        if (u.empty() || u[0] != '>') break;
        q += trim(u.substr(1)) + "\n";
      }
      addBlock(cur, q, false, 14, false);
      li = j - 1;
      continue;
    }
    para += (para.empty() ? "" : "\n") + t;
  }
  flushPara();
  closePage();
  return pages;
}

int estimateLines(const std::string& text, int widthPx, int pt, int sizePct) {
  double px = pt * 4.0 / 3.0 * sizePct / 100.0;
  int cpl = std::max(8, (int)((widthPx - 8) / (px * 0.55)));
  int lines = 0;
  size_t i = 0;
  while (i <= text.size()) {
    size_t nl = text.find('\n', i);
    size_t len = (nl == std::string::npos ? text.size() : nl) - i;
    // count characters, not bytes
    int chars = 0;
    for (size_t k = i; k < i + len; ++k) if (((unsigned char)text[k] & 0xC0) != 0x80) ++chars;
    lines += std::max(1, (chars + cpl - 1) / cpl);
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  return lines;
}

}  // namespace

std::string richToMarkdown(const std::string& text, const Runs& runsIn) {
  Runs runs = runsIn;
  normalizeRuns(runs, text.size());
  auto piece = [&](size_t a, size_t b, unsigned flags, const std::string& url) {
    std::string out, seg = text.substr(a, b - a);
    size_t p = 0;
    while (p <= seg.size()) {  // markers cannot span a line break, so style each line on its own
      size_t nl = seg.find('\n', p);
      std::string line = seg.substr(p, (nl == std::string::npos ? seg.size() : nl) - p);
      out += (flags || !url.empty()) ? styledPiece(line, flags, url) : escapeMd(line);
      if (nl == std::string::npos) break;
      out += '\n';
      p = nl + 1;
    }
    return out;
  };
  std::string out;
  size_t pos = 0;
  for (const Run& r : runs) {
    if (r.start > pos) out += piece(pos, r.start, 0, "");
    out += piece(r.start, r.end(), r.flags & (kBold | kItalic | kStrike | kMono), r.url);
    pos = r.end();
  }
  if (pos < text.size()) out += piece(pos, text.size(), 0, "");
  return out;
}

MdInline parseMarkdownInline(const std::string& markdown) {
  Ctx c;
  parseInl(markdown.size() > kMaxInput ? markdown.substr(0, kMaxInput) : markdown, 0,
           std::min(markdown.size(), kMaxInput), 0, "", c, 0);
  normalizeRuns(c.runs, c.out.size());
  return {c.out, c.runs};
}

std::string exportMarkdown(const Document& doc) {
  std::string out;
  for (const Page& p : doc.pages()) {
    std::string title;
    for (char ch : p.title) title += (ch == '\n' || ch == '\r') ? ' ' : ch;
    out += "# " + title + "\n\n";
    if (!p.tags.empty()) out += "Tags: " + p.tags + "\n\n";
    std::vector<const Object*> order;
    for (const Object& o : p.objects)
      if (o.type == ObjType::Text || o.type == ObjType::Button || o.type == ObjType::Image) order.push_back(&o);
    std::stable_sort(order.begin(), order.end(), [](const Object* a, const Object* b) {
      return a->y != b->y ? a->y < b->y : a->x < b->x;
    });
    for (const Object* o : order) {
      std::string block;
      if (o->type == ObjType::Text) {
        std::string text = o->prop("text");
        block = richToMarkdown(text, decodeRuns(o->prop("runs"), text.size()));
        if (o->propInt("bold", 0) && !block.empty() && block.find('\n') == std::string::npos) block = "**" + block + "**";
      } else if (o->type == ObjType::Button) {
        std::string label = escapeMd(o->prop("label"));
        long long target = o->propInt("target", 0);
        std::string web = o->prop("open").empty() ? "" : sanitizeLinkTarget(o->prop("open"));
        if (target > 0 && doc.page(target)) block = "[" + label + "](#" + slugOf(doc.page(target)->title) + ")";
        else if (!web.empty()) block = "[" + label + "](" + encodeUrl(web) + ")";
        else block = "[" + label + "]";
      } else {
        block = "*[image: " + escapeMd(o->prop("name", "image")) + "]*";
      }
      while (!block.empty() && (block.back() == '\n' || block.back() == ' ')) block.pop_back();
      if (!block.empty()) out += block + "\n\n";
    }
  }
  return out;
}

MdImportResult importMarkdown(Document& doc, const std::string& md, const std::string& fallbackTitle) {
  MdImportResult res;
  if (md.size() > kMaxInput) return res;
  std::vector<PendingPage> pages = parseBlocks(md, fallbackTitle.empty() ? "Imported" : fallbackTitle);
  const int kW = 900, kMargin = 40, kTextW = kW - 2 * kMargin;
  for (const PendingPage& pg : pages) {
    if (res.pages >= kMaxPages) break;
    std::vector<Object> objs;
    int y = 30;
    for (const Block& b : pg.blocks) {
      if ((int)objs.size() >= kMaxObjectsPerPage) break;
      int lines = estimateLines(b.text, kTextW, b.sizePt, 100);
      int h = (int)(lines * b.sizePt * 4.0 / 3.0 * 1.5) + 14;
      Object o;
      o.type = ObjType::Text;
      o.x = kMargin;
      o.y = y;
      o.w = kTextW;
      o.h = std::min(h, 4000);
      o.setProp("text", b.text);
      if (!b.runs.empty()) o.setProp("runs", encodeRuns(b.runs));
      if (b.sizePt != 14) o.setPropInt("size", b.sizePt);
      if (b.bold) o.setPropInt("bold", 1);
      objs.push_back(std::move(o));
      y += h + 10;
    }
    int64_t id = doc.addPageWithObjects(pg.title, kW, std::max(600, y + 40), pg.tags, std::move(objs), "Import Markdown");
    if (!res.firstPage) res.firstPage = id;
    ++res.pages;
    res.objects += (int)std::min<size_t>(pg.blocks.size(), kMaxObjectsPerPage);
  }
  return res;
}

}  // namespace cairn

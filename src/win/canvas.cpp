#include "canvas.h"
#include "table.h"
#include "media.h"
#include <mmsystem.h>

#include <richedit.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include <shellapi.h>
#include <wincodec.h>

#include "images.h"
#include "propform.h"
#include "rich.h"
#include "urls.h"

namespace cairn {

namespace {

const wchar_t* kClass = L"CairnCanvas";
constexpr UINT WM_EDIT_COMMIT = WM_APP + 1;  // wParam: 1 = give focus back to the canvas; lParam: the editor that sent it
constexpr UINT WM_EDIT_CANCEL = WM_APP + 2;
constexpr UINT WM_EDIT_FORMAT = WM_APP + 3;  // wParam: Canvas::FormatCmd
constexpr UINT WM_EDIT_UNDO = WM_APP + 6;    // wParam: 1 = redo
constexpr UINT WM_EDIT_SIZE = WM_APP + 5;    // wParam: +1 / -1
constexpr UINT WM_EDIT_CELLNAV = WM_APP + 7;  // wParam: +1 next cell / -1 previous (Tab, Shift+Tab)
constexpr UINT WM_EDIT_PASTE = WM_APP + 4;   // rich (HTML) paste into the open editor
constexpr int kGrid = 8;
constexpr int kMinSize = 8;
constexpr COLORREF kAccent = RGB(0x2F, 0x6F, 0xDE);

WNDPROC g_oldEditProc = nullptr;
// A table-cell editor carries the window property kCellProp (Tab moves between cells). It is per window,
// not global, because two panes can each have had an editor in the last instant.
const wchar_t* kCellProp = L"cairnCell";
bool g_editRich = false;  // the live editor is a RichEdit (text objects), not a plain EDIT

int cpOfByte(const std::string& s, size_t b) { return (int)utf16UnitsBefore(s, b); }

// Reads the editor's text with "\r\n" line ends, whichever control it is.
std::wstring readEditText(HWND e, bool rich) {
  if (!e) return L"";
  if (rich) {
    GETTEXTLENGTHEX gl{GTL_USECRLF | GTL_NUMCHARS, 1200};
    int n = (int)SendMessageW(e, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    if (n < 0) n = 0;
    std::wstring w((size_t)n + 2, L'\0');
    GETTEXTEX gt{};
    gt.cb = (DWORD)((n + 1) * sizeof(wchar_t));
    gt.flags = GT_USECRLF;
    gt.codepage = 1200;
    int got = (int)SendMessageW(e, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)&w[0]);
    w.resize((size_t)std::max(0, got));
    return w;
  }
  int n = GetWindowTextLengthW(e);
  std::wstring w((size_t)n + 1, L'\0');
  GetWindowTextW(e, &w[0], n + 1);
  w.resize((size_t)n);
  return w;
}

namespace {
// Equal-width columns and rows that fill the object; the last column/row absorbs the remainder.
RECT tableCellRect(const RECT& r, size_t rows, size_t cols, size_t ri, size_t ci) {
  int cw = std::max(1, (int)((r.right - r.left) / (long)cols)), rh = std::max(1, (int)((r.bottom - r.top) / (long)rows));
  RECT c;
  c.left = r.left + (int)ci * cw;
  c.top = r.top + (int)ri * rh;
  c.right = ci + 1 == cols ? r.right : c.left + cw;
  c.bottom = ri + 1 == rows ? r.bottom : c.top + rh;
  return c;
}

bool tableCellAt(const RECT& r, size_t rows, size_t cols, int px, int py, size_t& ri, size_t& ci) {
  if (!rows || !cols || px < r.left || px >= r.right || py < r.top || py >= r.bottom) return false;
  int cw = std::max(1, (int)((r.right - r.left) / (long)cols)), rh = std::max(1, (int)((r.bottom - r.top) / (long)rows));
  ci = std::min<size_t>(cols - 1, (size_t)((px - r.left) / cw));
  ri = std::min<size_t>(rows - 1, (size_t)((py - r.top) / rh));
  return true;
}
}  // namespace

// Subclass for the inline editor: Esc cancels, Enter (single-line) or
// Ctrl+Enter (multi-line) commits, losing focus commits quietly.
LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  HWND parent = GetParent(h);
  bool multi = (GetWindowLongPtrW(h, GWL_STYLE) & ES_MULTILINE) != 0;
  bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  switch (m) {
    case WM_KILLFOCUS:
      PostMessageW(parent, WM_EDIT_COMMIT, 0, (LPARAM)h);
      break;
    case WM_KEYDOWN:
      if (ctrl && multi) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int cmd = 0;
        if (!shift && w == 'B') cmd = 1;        // Canvas::FmtBold
        else if (!shift && w == 'I') cmd = 2;   // FmtItalic
        else if (!shift && w == 'U') cmd = 3;   // FmtUnderline
        else if (shift && w == 'X') cmd = 4;    // FmtStrike
        else if (!shift && w == 'K') cmd = 5;   // FmtLink
        if (g_editRich && (w == 'Z' || w == 'Y')) {  // the editor keeps its own history
          PostMessageW(parent, WM_EDIT_UNDO, (WPARAM)((w == 'Y' || shift) ? 1 : 0), 0);
          return 0;
        }
        if (w == VK_OEM_6 || w == VK_OEM_4) {   // Ctrl+] bigger, Ctrl+[ smaller
          PostMessageW(parent, WM_EDIT_SIZE, (WPARAM)(w == VK_OEM_6 ? 1 : -1), 0);
          return 0;
        }
        if (cmd) {
          PostMessageW(parent, WM_EDIT_FORMAT, (WPARAM)cmd, 0);
          return 0;
        }
      }
      if (g_editRich && ctrl && multi &&
          (w == 'E' || w == 'L' || w == 'R' || w == 'J' || w == '1' || w == '2' || w == '5' || w == 'T'))
        return 0;  // RichEdit's own paragraph shortcuts mean nothing in a Cairn text box
      if (GetPropW(h, kCellProp) && w == VK_TAB) {
        PostMessageW(parent, WM_EDIT_CELLNAV, (WPARAM)((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1), 0);
        return 0;
      }
      if (w == VK_ESCAPE) {
        PostMessageW(parent, WM_EDIT_CANCEL, 0, 0);
        return 0;
      }
      if (w == VK_RETURN && (!multi || ctrl)) {
        PostMessageW(parent, WM_EDIT_COMMIT, 1, (LPARAM)h);
        return 0;
      }
      break;
    case WM_PASTE:
      // Web page text arrives as HTML; keep its formatting unless Shift is held (plain paste).
      if (multi && !(GetKeyState(VK_SHIFT) & 0x8000) && IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format"))) {
        SendMessageW(parent, WM_EDIT_PASTE, 0, 0);
        return 0;
      }
      if (g_editRich) {  // never let RichEdit bring its own formatting (RTF, objects) along
        SendMessageW(h, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
        return 0;
      }
      break;
    case WM_CHAR:  // swallow the characters those keys would otherwise insert/beep
      if (GetPropW(h, kCellProp) && w == L'\t') return 0;  // Tab is navigation, never a character
      if (multi && ctrl && (w == 0x02 || w == 0x09 || w == 0x15 || w == 0x0B || w == 0x18 || w == 0x1D || w == 0x19 || w == 0x1A)) return 0;  // Ctrl+B/I/U/K/Shift+X/]
      if (w == VK_ESCAPE || (w == VK_RETURN && !multi) || (ctrl && (w == 0x0A || w == VK_RETURN)))
        return 0;
      break;
  }
  return CallWindowProcW(g_oldEditProc, h, m, w, l);
}

POINT handlePt(const RECT& r, int i) {
  int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  switch (i) {
    case 0: return {r.left, r.top};
    case 1: return {cx, r.top};
    case 2: return {r.right, r.top};
    case 3: return {r.right, cy};
    case 4: return {r.right, r.bottom};
    case 5: return {cx, r.bottom};
    case 6: return {r.left, r.bottom};
    default: return {r.left, cy};
  }
}

const wchar_t* typeName(ObjType t) {
  switch (t) {
    case ObjType::Text: return L"Text";
    case ObjType::Button: return L"Button";
    case ObjType::Rect: return L"Rectangle";
    case ObjType::Image: return L"Image";
    case ObjType::Folder: return L"Folder listing";
    case ObjType::Table: return L"Table";
    case ObjType::Audio: return L"Audio";
  }
  return L"Object";
}

const double kZoomLadder[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};


// ---- rich text: styled runs + web links --------------------------------------------
// Plain text still goes through DrawText. Text that has formatting runs or contains a link
// is laid out by hand so every piece knows its font, colour and link (for drawing and clicks).
constexpr COLORREF kLinkColor = RGB(0x0B, 0x57, 0xD0);

struct Span {           // a styled stretch of the text, in UTF-16 indices
  size_t s, e;
  unsigned flags;
  int color, size, link;  // link: index into RichModel::links, or -1
};

struct RichModel {
  std::wstring w;               // the text (tabs flattened to spaces)
  std::vector<Span> spans;      // sorted, non-overlapping; gaps are default style
  std::vector<std::wstring> links;
};

struct Piece {
  size_t s, e;
  int x, y, w, h, base;  // base = font ascent
  unsigned flags;
  int color, size, link;
  bool space;
};

}  // namespace

// The stored runs plus a display-only styling of [[wiki links]]: a found page gets a link, a missing one
// is shown in muted red and does nothing. Nothing here is written back to the document.
std::string Canvas::withWikiRuns(const Object& o) const {
  if (!doc_) return o.prop("runs");
  return wikiOverlay(o.prop("text"), o.prop("runs"),
                     [&](const std::string& t) { return (long long)doc_->pageByTitle(t); });
}

namespace {

RichModel buildRich(const std::string& utf8, const std::string& runsEnc) {
  RichModel m;
  m.w = widen(utf8);
  for (wchar_t& ch : m.w) if (ch == L'\t') ch = L' ';
  size_t n = m.w.size();

  // byte offset -> UTF-16 index
  std::vector<size_t> b2w(utf8.size() + 1);
  size_t wi = 0;
  for (size_t i = 0; i < utf8.size(); ++i) {
    b2w[i] = wi;
    unsigned char c = (unsigned char)utf8[i];
    if ((c & 0xC0) != 0x80) wi += c >= 0xF0 ? 2 : 1;
  }
  b2w[utf8.size()] = wi;

  std::vector<Span> ex;
  Runs runs = runsEnc.empty() ? Runs{} : decodeRuns(runsEnc, utf8.size());
  for (const Run& r : runs) {
    Span sp{std::min(b2w[r.start], n), std::min(b2w[r.end()], n), r.flags, r.color, r.size, -1};
    if (!r.url.empty()) {
      sp.link = (int)m.links.size();
      m.links.push_back(widen(r.url));
    }
    if (sp.e > sp.s) ex.push_back(sp);
  }
  auto urls = findUrls(m.w);
  if (ex.empty() && urls.empty()) return m;

  std::vector<size_t> pts = {0, n};
  for (const Span& sp : ex) { pts.push_back(sp.s); pts.push_back(sp.e); }
  for (auto& u : urls) { pts.push_back(u.first); pts.push_back(u.second); }
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

  std::map<size_t, int> autoLink;  // url start -> link index
  size_t ei = 0, ui = 0;
  for (size_t k = 0; k + 1 < pts.size(); ++k) {
    size_t p = pts[k], q = pts[k + 1];
    while (ei < ex.size() && ex[ei].e <= p) ++ei;
    while (ui < urls.size() && urls[ui].second <= p) ++ui;
    const Span* e = (ei < ex.size() && ex[ei].s <= p) ? &ex[ei] : nullptr;
    bool inUrl = ui < urls.size() && urls[ui].first <= p;
    Span sp{p, q, e ? e->flags : 0u, e ? e->color : -1, e ? e->size : 0, e ? e->link : -1};
    if (sp.link < 0 && inUrl) {
      auto it = autoLink.find(urls[ui].first);
      if (it == autoLink.end()) {
        it = autoLink.emplace(urls[ui].first, (int)m.links.size()).first;
        m.links.push_back(urlTarget(m.w.substr(urls[ui].first, urls[ui].second - urls[ui].first)));
      }
      sp.link = it->second;
    }
    if (!sp.flags && sp.color < 0 && !sp.size && sp.link < 0) continue;
    if (!m.spans.empty() && m.spans.back().e == sp.s && m.spans.back().flags == sp.flags &&
        m.spans.back().color == sp.color && m.spans.back().size == sp.size && m.spans.back().link == sp.link)
      m.spans.back().e = sp.e;
    else
      m.spans.push_back(sp);
  }
  return m;
}

// Greedy word-wrap of mixed-style text. Pieces come back in page-pixel coordinates
// relative to the top-left of the text box (add the box origin when drawing).
void layoutRich(HDC dc, const RichModel& m, int width, int align, const FontOf& fontOf,
                std::vector<Piece>& out, int& totalH) {
  const std::wstring& w = m.w;
  width = std::max(1, width);
  struct FM { int h, asc; };
  std::map<HFONT, FM> fmCache;
  auto fm = [&](HFONT f) {
    auto it = fmCache.find(f);
    if (it != fmCache.end()) return it->second;
    SelectObject(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    FM r{std::max<int>(1, tm.tmHeight), (int)tm.tmAscent};
    fmCache[f] = r;
    return r;
  };
  auto ext = [&](HFONT f, size_t a, size_t b) {
    SelectObject(dc, f);
    SIZE sz{0, 0};
    if (b > a) GetTextExtentPoint32W(dc, w.c_str() + a, (int)(b - a), &sz);
    return (int)sz.cx;
  };
  const FM baseFM = fm(fontOf(0, 0));

  std::vector<Piece> line;
  int x = 0, y = 0, lineH = 0, lineAsc = 0;
  bool softWrapped = false;
  auto finish = [&]() {
    int used = 0;
    for (const Piece& p : line) if (!p.space) used = std::max(used, p.x + p.w);  // trailing spaces don't count
    int shift = align == 1 ? (width - used) / 2 : align == 2 ? (width - used) : 0;
    if (shift < 0) shift = 0;
    int h = line.empty() ? baseFM.h : lineH, asc = line.empty() ? baseFM.asc : lineAsc;
    for (Piece p : line) {
      p.x += shift;
      p.y = y + asc - p.base;
      out.push_back(p);
    }
    y += h;
    line.clear();
    x = 0;
    lineH = lineAsc = 0;
  };
  auto place = [&](size_t a, size_t b, int pw, const FM& f, const Span* sp, bool space) {
    Piece p{a, b, x, 0, pw, f.h, f.asc, sp ? sp->flags : 0u, sp ? sp->color : -1, sp ? sp->size : 0,
            sp ? sp->link : -1, space};
    line.push_back(p);
    x += pw;
    lineH = std::max(lineH, f.h);
    lineAsc = std::max(lineAsc, f.asc);
  };

  size_t i = 0, si = 0, n = w.size();
  while (i < n) {
    while (si < m.spans.size() && m.spans[si].e <= i) ++si;
    const Span* sp = (si < m.spans.size() && m.spans[si].s <= i) ? &m.spans[si] : nullptr;
    size_t limit = sp ? sp->e : (si < m.spans.size() ? m.spans[si].s : n);
    if (w[i] == L'\n') {
      finish();
      softWrapped = false;
      ++i;
      continue;
    }
    bool space = w[i] == L' ';
    size_t j = i;
    while (j < limit && w[j] != L'\n' && (w[j] == L' ') == space) ++j;
    HFONT f = fontOf(sp ? sp->flags : 0u, sp ? sp->size : 0);
    FM fmx = fm(f);
    int aw = ext(f, i, j);
    if (space) {
      if (!(softWrapped && x == 0)) place(i, j, aw, fmx, sp, true);
    } else {
      if (x > 0 && x + aw > width) {
        finish();
        softWrapped = true;
      }
      if (aw > width) {  // one word wider than the box: break it by characters
        size_t pos = i;
        while (pos < j) {
          int fit = 0;
          SIZE sz{0, 0};
          SelectObject(dc, f);
          GetTextExtentExPointW(dc, w.c_str() + pos, (int)(j - pos), width, &fit, nullptr, &sz);
          size_t e = pos + (size_t)std::max(1, fit);
          if (e < j && e - pos > 1 && w[e - 1] >= 0xD800 && w[e - 1] <= 0xDBFF) --e;  // keep surrogate pairs whole
          place(pos, e, ext(f, pos, e), fmx, sp, false);
          pos = e;
          if (pos < j) {
            finish();
            softWrapped = true;
          }
        }
      } else {
        place(i, j, aw, fmx, sp, false);
      }
      softWrapped = false;
    }
    i = j;
  }
  finish();
  totalH = y;
}

}  // namespace

// ------------------------------------------------------------ window class

bool Canvas::registerClass(HINSTANCE hi) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = &Canvas::wndProc;
  wc.hInstance = hi;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  return RegisterClassExW(&wc) != 0;
}

HWND Canvas::create(HWND parent, int id, HINSTANCE hi) {
  hi_ = hi;
  hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kClass, L"",
                          WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0,
                          100, 100, parent, (HMENU)(INT_PTR)id, hi, this);
  dpi_ = dpiFor(hwnd_);
  DragAcceptFiles(hwnd_, TRUE);
  all_.push_back(this);
  return hwnd_;
}

Canvas::~Canvas() {
  all_.erase(std::remove(all_.begin(), all_.end(), this), all_.end());
  if (all_.empty()) audioStop();
  if (all_.empty() && !audioDir_.empty()) {  // the temporary copies of embedded clips
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(audioDir_), ec);
  }
  clearFonts();
  clearImages();
}

LRESULT CALLBACK Canvas::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Canvas* c = (Canvas*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) {
    c = (Canvas*)((CREATESTRUCTW*)l)->lpCreateParams;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
    if (c) c->hwnd_ = h;
  }
  return c ? c->proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------ public state

void Canvas::setDocument(Document* d) {
  endEdit(false, false);
  clearImages();
  cancelDrag();
  doc_ = d;
  pageId_ = 0;
  sel_.clear();
  scrollX_ = scrollY_ = 0;
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::setPage(int64_t id) {
  endEdit(true, false);
  cancelDrag();
  bool same = (id == pageId_);
  pageId_ = id;
  sel_.clear();
  if (!same) {
    scrollX_ = scrollY_ = 0;
    audioStop();  // sound belongs to the page it was started on
    audioAutoplay();
  }
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::docChanged() {
  if (audioObj_ && !(doc_ && doc_->object(audioObj_))) audioStop();  // the playing object was deleted
  if (editWnd_ && !(doc_ && doc_->object(editId_))) endEdit(false, false);
  pruneSelection();
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::onDpiChanged() {
  dpi_ = dpiFor(hwnd_);
  clearFonts();
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::setEditMode(bool on) {
  if (edit_ == on) return;
  endEdit(true, false);
  cancelDrag();
  edit_ = on;
  if (!on) sel_.clear();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::setSnap(bool on) {
  snap_ = on;
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::setZoom(double z) {
  z = std::clamp(z, 0.25, 4.0);
  if (std::fabs(z - zoom_) < 1e-6) return;
  double ratio = z / zoom_;
  endEdit(true, false);
  zoom_ = z;
  clearFonts();
  scrollX_ = (int)std::lround(scrollX_ * ratio);
  scrollY_ = (int)std::lround(scrollY_ * ratio);
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::zoomStep(int dir) {
  const int n = (int)(sizeof kZoomLadder / sizeof kZoomLadder[0]);
  if (dir > 0) {
    for (int i = 0; i < n; ++i)
      if (kZoomLadder[i] > zoom_ + 1e-6) return setZoom(kZoomLadder[i]);
  } else {
    for (int i = n - 1; i >= 0; --i)
      if (kZoomLadder[i] < zoom_ - 1e-6) return setZoom(kZoomLadder[i]);
  }
}

void Canvas::notify() {
  if (onStateChanged) onStateChanged();
}

bool Canvas::selectionHasText() const {
  if (!doc_) return false;
  for (int64_t id : sel_)
    if (const Object* o = doc_->object(id))
      if (o->type == ObjType::Text) return true;
  return false;
}

bool Canvas::selectionHasSizable() const {
  if (!doc_) return false;
  for (int64_t id : sel_)
    if (const Object* o = doc_->object(id))
      if (o->type == ObjType::Text || o->type == ObjType::Button) return true;
  return false;
}

std::wstring Canvas::selectionSummary() const {
  if (sel_.empty()) return L"";
  if (sel_.size() > 1) return std::to_wstring(sel_.size()) + L" objects selected";
  const Object* o = doc_ ? doc_->object(sel_[0]) : nullptr;
  if (!o) return L"";
  std::wstring s = typeName(o->type);
  s += L"   x " + std::to_wstring(o->x) + L"  y " + std::to_wstring(o->y) + L"   " +
       std::to_wstring(o->w) + L" × " + std::to_wstring(o->h);
  if (o->type == ObjType::Image) s += L"   " + widen(o->prop("name"));
  if (o->type == ObjType::Folder) s += L"   " + widen(o->prop("path", "."));
  if (o->type == ObjType::Button) {
    int64_t t = o->propInt("target", 0);
    const Page* tp = t > 0 ? doc_->page(t) : nullptr;
    s += tp ? L"   → " + widen(tp->title) : (t > 0 ? L"   → (missing page)" : L"   (no target)");
  }
  return s;
}

// ---------------------------------------------------------------- geometry

POINT Canvas::origin() const {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  const Page* p = curPage();
  int margin = MulDiv(28, dpi_, 96);
  int pw = p ? (int)std::lround(p->w * scale()) : 0;
  POINT o;
  o.x = (pw + 2 * margin <= rc.right) ? (rc.right - pw) / 2 : margin - scrollX_;
  o.y = margin - scrollY_;
  return o;
}

RECT Canvas::objRectPx(const Object& o) const {
  POINT org = origin();
  double s = scale();
  RECT r;
  r.left = org.x + (int)std::lround(o.x * s);
  r.top = org.y + (int)std::lround(o.y * s);
  r.right = org.x + (int)std::lround((o.x + o.w) * s);
  r.bottom = org.y + (int)std::lround((o.y + o.h) * s);
  return r;
}

POINT Canvas::toPagePt(int px, int py) const {
  POINT org = origin();
  double s = scale();
  return {(int)std::floor((px - org.x) / s), (int)std::floor((py - org.y) / s)};
}

int Canvas::snapv(int v) const {
  return snap_ ? (int)std::floor(v / (double)kGrid + 0.5) * kGrid : v;
}

const Object& Canvas::eff(const Object& o) const {
  auto it = dragNow_.find(o.id);
  return it == dragNow_.end() ? o : it->second;
}

// ---------------------------------------------------------------- painting

HFONT Canvas::fontFor(int pt, bool bold) { return fontStyled(pt, bold ? (unsigned)kBold : 0u); }

HFONT Canvas::fontStyled(int pt, unsigned flags) {
  flags &= kBold | kItalic | kMono;
  auto key = std::make_pair(pt, (int)flags);
  auto it = fonts_.find(key);
  if (it != fonts_.end()) return it->second;
  int px = std::max(4, (int)std::lround(pt * 4.0 / 3.0 * scale()));
  bool mono = (flags & kMono) != 0;
  HFONT f = CreateFontW(-px, 0, 0, 0, (flags & kBold) ? FW_BOLD : FW_NORMAL, (flags & kItalic) ? TRUE : FALSE,
                        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, mono ? (FIXED_PITCH | FF_MODERN) : (DEFAULT_PITCH | FF_SWISS),
                        mono ? L"Consolas" : L"Segoe UI");
  fonts_[key] = f;
  return f;
}

void Canvas::clearFonts() {
  for (auto& kv : fonts_) DeleteObject(kv.second);
  fonts_.clear();
}

void Canvas::drawObject(HDC dc, const Object& o, bool pressed) {
  RECT r = objRectPx(o);
  int pad = std::max(2, (int)std::lround(4 * scale()));
  SaveDC(dc);
  IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
  switch (o.type) {
    case ObjType::Rect: {
      RestoreDC(dc, -1);  // shapes draw their own border; don't clip it
      SaveDC(dc);
      int fill = o.propInt("fill", -1), line = o.propInt("line", 0x555555);
      int lw = o.propInt("lw", 1), rad = o.propInt("round", 0);
      int pw = std::max(1, (int)std::lround(lw * scale()));
      HBRUSH br = fill >= 0 ? CreateSolidBrush(toColorRef(fill)) : (HBRUSH)GetStockObject(NULL_BRUSH);
      HPEN pen = (line >= 0 && lw > 0) ? CreatePen(PS_INSIDEFRAME, pw, toColorRef(line))
                                       : (HPEN)GetStockObject(NULL_PEN);
      SelectObject(dc, br);
      SelectObject(dc, pen);
      if (rad > 0) {
        int d = std::max(2, (int)std::lround(2 * rad * scale()));
        RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, d, d);
      } else {
        Rectangle(dc, r.left, r.top, r.right + 1, r.bottom + 1);
      }
      if (fill >= 0) DeleteObject(br);
      if (line >= 0 && lw > 0) DeleteObject(pen);
      break;
    }
    case ObjType::Text: {
      int fill = o.propInt("fill", -1);
      if (fill >= 0) {
        HBRUSH b = CreateSolidBrush(toColorRef(fill));
        FillRect(dc, &r, b);
        DeleteObject(b);
      } else if (edit_) {
        HPEN pen = CreatePen(PS_DOT, 1, RGB(170, 170, 170));  // ghost outline so empty text is findable
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        DeleteObject(pen);
      }
      SelectObject(dc, fontFor(o.propInt("size", 14), o.propInt("bold", 0) != 0));
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, toColorRef(o.propInt("color", 0x222222)));
      RECT tr = r;
      InflateRect(&tr, -pad, -pad);
      int al = o.propInt("align", 0);
      UINT fl = DT_WORDBREAK | DT_NOPREFIX | DT_TOP | (al == 1 ? DT_CENTER : al == 2 ? DT_RIGHT : DT_LEFT);
      RichModel rm = buildRich(o.prop("text"), withWikiRuns(o));
      if (rm.spans.empty()) {
        std::wstring w = widen(o.prop("text"));
        DrawTextW(dc, w.c_str(), (int)w.size(), &tr, fl);
      } else {
        std::vector<Piece> pieces;
        int totalH = 0;
        layoutRich(dc, rm, tr.right - tr.left, al, richFont(o), pieces, totalH);
        FontOf fo = richFont(o);
        COLORREF normal = toColorRef(o.propInt("color", 0x222222));
        for (const Piece& pc : pieces) {
          int py = tr.top + pc.y, px = tr.left + pc.x;
          if (py > tr.bottom) break;
          bool link = pc.link >= 0;
          SelectObject(dc, fo(pc.flags, pc.size));
          COLORREF col = link ? kLinkColor : (pc.color >= 0 ? toColorRef(pc.color) : normal);
          SetTextColor(dc, col);
          if (!pc.space) TextOutW(dc, px, py, rm.w.c_str() + pc.s, (int)(pc.e - pc.s));
          if (link || (pc.flags & kUnderline) || (pc.flags & kStrike)) {
            HPEN up = CreatePen(PS_SOLID, 1, col);
            HGDIOBJ op = SelectObject(dc, up);
            if (link || (pc.flags & kUnderline)) {
              int uy = py + pc.base + std::max(1, pc.h / 14);
              MoveToEx(dc, px, uy, nullptr);
              LineTo(dc, px + pc.w, uy);
            }
            if (pc.flags & kStrike) {
              int sy = py + pc.h * 11 / 20;
              MoveToEx(dc, px, sy, nullptr);
              LineTo(dc, px + pc.w, sy);
            }
            SelectObject(dc, op);
            DeleteObject(up);
          }
        }
      }
      break;
    }
    case ObjType::Image:
      drawImage(dc, o, r);
      break;
    case ObjType::Folder:
      drawFolder(dc, o, r);
      break;
    case ObjType::Table:
      drawTable(dc, o, r);
      break;
    case ObjType::Audio:
      drawAudio(dc, o, r);
      break;
    case ObjType::Button: {
      int fill = o.propInt("fill", -1);
      HBRUSH b = fill >= 0 ? CreateSolidBrush(toColorRef(fill)) : GetSysColorBrush(COLOR_BTNFACE);
      FillRect(dc, &r, b);
      if (fill >= 0) DeleteObject(b);
      DrawEdge(dc, &r, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
      SelectObject(dc, fontFor(o.propInt("size", 13), o.propInt("bold", 0) != 0));
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, toColorRef(o.propInt("color", 0x000000)));
      RECT tr = r;
      InflateRect(&tr, -pad, -pad);
      if (pressed) OffsetRect(&tr, 1, 1);
      std::wstring w = widen(o.prop("label"));
      DrawTextW(dc, w.c_str(), (int)w.size(), &tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      break;
    }
  }
  RestoreDC(dc, -1);
}

void Canvas::drawSelection(HDC dc) {
  if (!edit_ || !doc_) return;
  HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
  for (int64_t id : sel_) {
    const Object* o = doc_->object(id);
    if (!o) continue;
    RECT r = objRectPx(eff(*o));
    Rectangle(dc, r.left, r.top, r.right + 1, r.bottom + 1);
  }
  if (sel_.size() == 1 && drag_ != Drag::Marquee) {
    if (const Object* o = doc_->object(sel_[0])) {
      RECT r = objRectPx(eff(*o));
      int hs = MulDiv(4, dpi_, 96);
      SelectObject(dc, GetStockObject(WHITE_BRUSH));
      for (int i = 0; i < 8; ++i) {
        POINT p = handlePt(r, i);
        Rectangle(dc, p.x - hs, p.y - hs, p.x + hs + 1, p.y + hs + 1);
      }
    }
  }
  if (drag_ == Drag::Marquee) {
    HPEN dot = CreatePen(PS_DOT, 1, kAccent);
    SelectObject(dc, dot);
    Rectangle(dc, marquee_.left, marquee_.top, marquee_.right, marquee_.bottom);
    SelectObject(dc, pen);
    DeleteObject(dot);
  }
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBr);
  DeleteObject(pen);
}

void Canvas::paint(HDC target) {
  ++paintCount_;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int cw = rc.right, ch = rc.bottom;
  if (cw <= 0 || ch <= 0) return;
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, cw, ch);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);

  HBRUSH ws = CreateSolidBrush(edit_ ? RGB(104, 112, 122) : RGB(84, 90, 98));
  FillRect(dc, &rc, ws);
  DeleteObject(ws);

  const Page* p = curPage();
  if (!p) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 220));
    DrawTextW(dc, L"No page selected", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else {
    POINT org = origin();
    double s = scale();
    RECT pr = {org.x, org.y, org.x + (int)std::lround(p->w * s), org.y + (int)std::lround(p->h * s)};
    int sh = MulDiv(4, dpi_, 96);
    RECT shadow = pr;
    OffsetRect(&shadow, sh, sh);
    HBRUSH sb = CreateSolidBrush(RGB(52, 58, 66));
    FillRect(dc, &shadow, sb);
    DeleteObject(sb);
    HBRUSH pb = CreateSolidBrush(toColorRef(p->bg));
    FillRect(dc, &pr, pb);
    DeleteObject(pb);

    SaveDC(dc);
    IntersectClipRect(dc, pr.left, pr.top, pr.right, pr.bottom);

    if (edit_ && snap_) {  // faint grid dots
      double step = 2 * kGrid * s;
      if (step >= 6) {
        int br = (GetRValue(toColorRef(p->bg)) + GetGValue(toColorRef(p->bg)) + GetBValue(toColorRef(p->bg))) / 3;
        COLORREF dot = br > 128 ? RGB(205, 208, 212) : RGB(90, 94, 100);
        for (double gy = 0; gy <= p->h; gy += 2 * kGrid)
          for (double gx = 0; gx <= p->w; gx += 2 * kGrid)
            SetPixelV(dc, org.x + (int)std::lround(gx * s), org.y + (int)std::lround(gy * s), dot);
      }
    }
    for (const Object& o : p->objects) {
      bool pressed = (drag_ == Drag::Press && pressed_ == o.id && pressInside_);
      drawObject(dc, eff(o), pressed);
    }
    RestoreDC(dc, -1);

    drawSelection(dc);
  }
  BitBlt(target, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

// ------------------------------------------------------------- hit testing

const Object* Canvas::objectAt(int px, int py) const {
  const Page* p = curPage();
  if (!p) return nullptr;
  for (auto it = p->objects.rbegin(); it != p->objects.rend(); ++it) {
    RECT r = objRectPx(eff(*it));
    InflateRect(&r, 2, 2);
    POINT pt = {px, py};
    if (PtInRect(&r, pt)) return &*it;
  }
  return nullptr;
}

int Canvas::handleAt(int px, int py) const {
  if (!edit_ || sel_.size() != 1 || !doc_) return -1;
  const Object* o = doc_->object(sel_[0]);
  if (!o) return -1;
  RECT r = objRectPx(eff(*o));
  int hs = MulDiv(4, dpi_, 96) + 3;
  for (int i = 0; i < 8; ++i) {
    POINT p = handlePt(r, i);
    if (std::abs(px - p.x) <= hs && std::abs(py - p.y) <= hs) return i;
  }
  return -1;
}

bool Canvas::isSelected(int64_t id) const {
  return std::find(sel_.begin(), sel_.end(), id) != sel_.end();
}

void Canvas::pruneSelection() {
  const Page* p = curPage();
  sel_.erase(std::remove_if(sel_.begin(), sel_.end(),
                            [&](int64_t id) {
                              if (!p) return true;
                              for (const Object& o : p->objects)
                                if (o.id == id) return false;
                              return true;
                            }),
             sel_.end());
}

// --------------------------------------------------------------- scrolling

void Canvas::updateScroll() {
  if (!hwnd_ || inScroll_) return;
  inScroll_ = true;
  const Page* p = curPage();
  int margin = MulDiv(28, dpi_, 96);
  int contentW = p ? (int)std::lround(p->w * scale()) + 2 * margin : 0;
  int contentH = p ? (int)std::lround(p->h * scale()) + 2 * margin : 0;
  for (int pass = 0; pass < 2; ++pass) {  // a scrollbar appearing changes the client size
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SCROLLINFO si{};
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = std::max(0, contentW - 1);
    si.nPage = (UINT)rc.right;
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
    si.nMax = std::max(0, contentH - 1);
    si.nPage = (UINT)rc.bottom;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
  }
  RECT rc;
  GetClientRect(hwnd_, &rc);
  scrollX_ = std::clamp(scrollX_, 0, std::max(0, contentW - (int)rc.right));
  scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentH - (int)rc.bottom));
  SetScrollPos(hwnd_, SB_HORZ, scrollX_, TRUE);
  SetScrollPos(hwnd_, SB_VERT, scrollY_, TRUE);
  inScroll_ = false;
}

void Canvas::setScroll(int bar, int pos) {
  (bar == SB_HORZ ? scrollX_ : scrollY_) = pos;
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::scrollBy(int dx, int dy) {
  if (dx) setScroll(SB_HORZ, scrollX_ + dx);
  if (dy) setScroll(SB_VERT, scrollY_ + dy);
}

// ------------------------------------------------------------ mouse input

void Canvas::cancelDrag() {
  bool had = drag_ != Drag::None;
  drag_ = Drag::None;
  dragNow_.clear();
  pressed_ = 0;
  pressInside_ = false;
  if (had) {
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

FontOf Canvas::richFont(const Object& o) {
  int base = o.propInt("size", 14);
  unsigned objBold = o.propInt("bold", 0) != 0 ? (unsigned)kBold : 0u;
  return [this, base, objBold](unsigned flags, int pct) {
    int pt = std::max(4, (int)std::lround(base * (pct ? pct : 100) / 100.0));
    return fontStyled(pt, flags | objBold);
  };
}

std::wstring Canvas::urlAt(const Object& o, int x, int y) {
  if (o.type != ObjType::Text) return L"";
  RichModel rm = buildRich(o.prop("text"), withWikiRuns(o));
  if (rm.links.empty()) return L"";
  RECT r = objRectPx(o);
  int pad = std::max(2, (int)std::lround(4 * scale()));
  InflateRect(&r, -pad, -pad);
  HDC dc = GetDC(hwnd_);
  std::vector<Piece> pieces;
  int totalH = 0;
  layoutRich(dc, rm, r.right - r.left, o.propInt("align", 0), richFont(o), pieces, totalH);
  ReleaseDC(hwnd_, dc);
  for (const Piece& pc : pieces) {
    if (pc.link < 0) continue;
    int px = r.left + pc.x, py = r.top + pc.y;
    if (x >= px && x < px + pc.w && y >= py && y < py + pc.h) return rm.links[(size_t)pc.link];
  }
  return L"";
}

void Canvas::onLButtonDown(int x, int y, WPARAM keys) {
  SetFocus(hwnd_);
  endEdit(true, false);
  if (!curPage()) return;
  bool extend = (keys & (MK_SHIFT | MK_CONTROL)) != 0;

  if (!edit_) {  // Run mode: only buttons react
    const Object* o = objectAt(x, y);
    if (o && o->type == ObjType::Text) {
      std::wstring u = urlAt(*o, x, y);  // copy before the callback: it may reload the page
      if (!u.empty() && onOpenLink) onOpenLink(u);
      return;
    }
    if (o && o->type == ObjType::Audio) {
      if (audioObj_ == o->id) audioStop();
      else if (!audioStart(*o))
        MessageBoxW(GetParent(hwnd_), L"This clip can't be played on this computer.", L"Cairn", MB_OK | MB_ICONINFORMATION);
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    if (o && o->type == ObjType::Folder) {
      int row = folderRowAt(*o, x, y);
      const FolderCache& fc = folderItems(*o);
      int idx = row + (folderScroll_.count(o->id) ? folderScroll_[o->id] : 0);
      if (row >= 0 && idx < (int)fc.items.size() && onOpenLink) {
        std::wstring full = fc.path + L"\\" + fc.items[(size_t)idx].name;  // copy: the callback may reload
        onOpenLink(full);
      }
      return;
    }
    if (o && o->type == ObjType::Button) {
      pressed_ = o->id;
      pressInside_ = true;
      drag_ = Drag::Press;
      SetCapture(hwnd_);
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return;
  }

  int hnd = handleAt(x, y);
  if (hnd >= 0) {
    dragStart_ = {x, y};
    dragHandle_ = hnd;
    dragOrig_ = {*doc_->object(sel_[0])};
    drag_ = Drag::Resize;
    SetCapture(hwnd_);
    return;
  }

  if (const Object* o = objectAt(x, y)) {
    if (extend) {
      auto it = std::find(sel_.begin(), sel_.end(), o->id);
      if (it == sel_.end()) sel_.push_back(o->id);
      else sel_.erase(it);
      InvalidateRect(hwnd_, nullptr, FALSE);
      notify();
      return;
    }
    if (!isSelected(o->id)) sel_ = {o->id};
    dragStart_ = {x, y};
    dragOrig_.clear();
    for (int64_t id : sel_)
      if (const Object* so = doc_->object(id)) dragOrig_.push_back(*so);
    drag_ = Drag::Pending;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    notify();
    return;
  }

  marqueeBase_ = extend ? sel_ : std::vector<int64_t>{};
  if (!extend) sel_.clear();
  dragStart_ = {x, y};
  marquee_ = {x, y, x, y};
  drag_ = Drag::Marquee;
  SetCapture(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::onMouseMove(int x, int y, WPARAM) {
  if (drag_ == Drag::None) return;
  double s = scale();
  int ddx = (int)std::lround((x - dragStart_.x) / s);
  int ddy = (int)std::lround((y - dragStart_.y) / s);
  bool snapNow = snap_ && !(GetKeyState(VK_MENU) & 0x8000);  // Alt = freehand

  if (drag_ == Drag::Pending) {
    int thr = MulDiv(3, dpi_, 96);
    if (std::abs(x - dragStart_.x) < thr && std::abs(y - dragStart_.y) < thr) return;
    drag_ = Drag::Move;
  }

  if (drag_ == Drag::Move) {
    if (snapNow && !dragOrig_.empty()) {
      auto g = [](int v) { return (int)std::floor(v / (double)kGrid + 0.5) * kGrid; };
      ddx = g(dragOrig_[0].x + ddx) - dragOrig_[0].x;
      ddy = g(dragOrig_[0].y + ddy) - dragOrig_[0].y;
    }
    dragNow_.clear();
    for (const Object& o : dragOrig_) {
      Object n = o;
      n.x += ddx;
      n.y += ddy;
      dragNow_[n.id] = n;
    }
  } else if (drag_ == Drag::Resize) {
    const Object& o = dragOrig_[0];
    int l = o.x, t = o.y, r = o.x + o.w, b = o.y + o.h;
    int h = dragHandle_;
    bool mL = (h == 0 || h == 6 || h == 7), mT = (h == 0 || h == 1 || h == 2);
    bool mR = (h == 2 || h == 3 || h == 4), mB = (h == 4 || h == 5 || h == 6);
    auto g = [&](int v) { return snapNow ? (int)std::floor(v / (double)kGrid + 0.5) * kGrid : v; };
    if (mL) l = g(l + ddx);
    if (mT) t = g(t + ddy);
    if (mR) r = g(r + ddx);
    if (mB) b = g(b + ddy);
    if ((h == 0 || h == 2 || h == 4 || h == 6) && (GetKeyState(VK_SHIFT) & 0x8000) && o.w > 0 && o.h > 0) {
      int nw = std::max(kMinSize, r - l);  // Shift on a corner keeps the original proportions
      int nh = std::max(kMinSize, (int)std::lround(nw * (double)o.h / o.w));
      if (mL) l = r - nw; else r = l + nw;
      if (mT) t = b - nh; else b = t + nh;
    }
    if (r - l < kMinSize) { if (mL) l = r - kMinSize; else r = l + kMinSize; }
    if (b - t < kMinSize) { if (mT) t = b - kMinSize; else b = t + kMinSize; }
    Object n = o;
    n.x = l; n.y = t; n.w = r - l; n.h = b - t;
    dragNow_.clear();
    dragNow_[n.id] = n;
  } else if (drag_ == Drag::Marquee) {
    marquee_ = {std::min<LONG>(dragStart_.x, x), std::min<LONG>(dragStart_.y, y),
                std::max<LONG>(dragStart_.x, x), std::max<LONG>(dragStart_.y, y)};
    sel_ = marqueeBase_;
    if (const Page* p = curPage()) {
      for (const Object& o : p->objects) {
        RECT r = objRectPx(o), hit;
        if (IntersectRect(&hit, &r, &marquee_) && !isSelected(o.id)) sel_.push_back(o.id);
      }
    }
    notify();
  } else if (drag_ == Drag::Press) {
    const Object* o = doc_ ? doc_->object(pressed_) : nullptr;
    bool inside = false;
    if (o) {
      RECT r = objRectPx(*o);
      POINT pt = {x, y};
      inside = PtInRect(&r, pt) != 0;
    }
    if (inside != pressInside_) pressInside_ = inside;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::onLButtonUp(int, int) {
  Drag d = drag_;
  drag_ = Drag::None;  // before ReleaseCapture, so WM_CAPTURECHANGED doesn't treat it as a cancel
  if (GetCapture() == hwnd_) ReleaseCapture();

  if ((d == Drag::Move || d == Drag::Resize) && !dragNow_.empty()) {
    std::vector<Object> v;
    for (auto& kv : dragNow_) v.push_back(kv.second);
    dragNow_.clear();
    doc_->updateObjects(v, d == Drag::Move ? "Move" : "Resize");
  } else if (d == Drag::Press) {
    int64_t id = pressed_;
    bool inside = pressInside_;
    pressed_ = 0;
    pressInside_ = false;
    if (inside) {
      if (const Object* o = doc_->object(id)) {
        int64_t target = o->propInt("target", 0);
        std::wstring open = widen(o->prop("open"));  // copy first: navigating can invalidate `o`
        if (!open.empty() && onOpenLink) onOpenLink(open);
        if (target > 0 && doc_->page(target) && onNavigate) onNavigate(target);
      }
    }
  }
  dragNow_.clear();
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::onDoubleClick(int x, int y) {
  if (!edit_) {
    onLButtonDown(x, y, 0);  // a quick second click on a Run-mode button is just another click
    return;
  }
  const Object* o = objectAt(x, y);
  if (!o) return;
  sel_ = {o->id};
  notify();
  if (o->type == ObjType::Text) beginTextEdit();
  else if (o->type == ObjType::Table) {
    Table t = parseTable(o->prop("data"));
    size_t r, c;
    if (tableCellAt(objRectPx(*o), t.rows.size(), t.cols(), x, y, r, c)) beginCellEdit(*o, r, c);
    else editProperties();
  } else editProperties();
}

bool Canvas::onKeyDown(WPARAM vk) {
  if (!doc_) return false;
  bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  int step = shift ? 10 : 1;
  if (vk == VK_ESCAPE) {
    if (drag_ != Drag::None) cancelDrag();
    else if (!sel_.empty()) { sel_.clear(); InvalidateRect(hwnd_, nullptr, FALSE); notify(); }
    return true;
  }
  if (!edit_) return false;
  switch (vk) {
    case VK_DELETE: deleteSelection(); return true;
    case VK_LEFT: nudge(-step, 0); return true;
    case VK_RIGHT: nudge(step, 0); return true;
    case VK_UP: nudge(0, -step); return true;
    case VK_DOWN: nudge(0, step); return true;
    case VK_F2:
    case VK_RETURN: beginTextEdit(); return true;
  }
  return false;
}

// ----------------------------------------------------------------- editing

bool Canvas::beginCellEdit(const Object& o, size_t row, size_t col) {
  Table t = parseTable(o.prop("data"));
  if (row >= t.rows.size() || col >= t.cols()) return false;
  endEdit(true, false);
  RECT cell = tableCellRect(objRectPx(o), t.rows.size(), t.cols(), row, col);
  int minW = MulDiv(70, dpi_, 96);
  if (cell.right - cell.left < minW) cell.right = cell.left + minW;
  std::wstring text = widen(t.rows[row][col]);
  editWnd_ = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             cell.left, cell.top, cell.right - cell.left, cell.bottom - cell.top, hwnd_,
                             (HMENU)(INT_PTR)1, hi_, nullptr);
  if (!editWnd_) return false;
  editRich_ = false;
  g_editRich = false;
  SetPropW(editWnd_, kCellProp, (HANDLE)1);
  editIsText_ = false;
  editId_ = o.id;
  editRow_ = (int)row;
  editCol_ = (int)col;
  SendMessageW(editWnd_, EM_LIMITTEXT, kTableMaxCell, 0);
  SendMessageW(editWnd_, WM_SETFONT, (WPARAM)fontFor(o.propInt("size", 12), o.propInt("header", 1) != 0 && row == 0), TRUE);
  g_oldEditProc = (WNDPROC)SetWindowLongPtrW(editWnd_, GWLP_WNDPROC, (LONG_PTR)editProc);
  SetFocus(editWnd_);
  SendMessageW(editWnd_, EM_SETSEL, 0, -1);
  return true;
}

// Tab: commit this cell and open the next one (Shift+Tab: the previous). Tab in the very last cell
// adds a row, like a spreadsheet.
void Canvas::cellNavigate(int dir) {
  if (!editWnd_ || editRow_ < 0 || !doc_) return;
  int64_t id = editId_;
  size_t row = (size_t)editRow_, col = (size_t)editCol_;
  endEdit(true, false);
  const Object* o = doc_->object(id);
  if (!o || o->type != ObjType::Table) return;
  Table t = parseTable(o->prop("data"));
  size_t rows = t.rows.size(), cols = t.cols();
  if (!rows || !cols) return;
  size_t idx = row * cols + col;
  if (dir > 0) {
    if (idx + 1 >= rows * cols) {
      if (!tableInsertRow(t, rows)) return;
      Object a = *o;
      a.setProp("data", formatTable(t));
      a.h = std::min(a.h + std::max(20, o->h / (int)rows), 4000);
      doc_->updateObject(a, "Table Row");
      o = doc_->object(id);
      if (!o) return;
    }
    ++idx;
  } else {
    if (idx == 0) return;
    --idx;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
  beginCellEdit(*o, idx / cols, idx % cols);
}

bool Canvas::contextOnTable() const {
  if (!doc_) return false;
  const Object* o = const_cast<Canvas*>(this)->objectAt(ctx_.x, ctx_.y);
  if (!o || o->type != ObjType::Table) return false;
  Table t = parseTable(o->prop("data"));
  size_t r, c;
  return tableCellAt(objRectPx(*o), t.rows.size(), t.cols(), ctx_.x, ctx_.y, r, c);
}

void Canvas::tableOp(int op) {
  if (!doc_) return;
  endEdit(true, false);
  const Object* o = objectAt(ctx_.x, ctx_.y);
  if (!o || o->type != ObjType::Table) return;
  Table t = parseTable(o->prop("data"));
  size_t rows = t.rows.size(), cols = t.cols(), r = 0, c = 0;
  if (!tableCellAt(objRectPx(*o), rows, cols, ctx_.x, ctx_.y, r, c)) return;
  int rowH = std::max(20, o->h / (int)std::max<size_t>(1, rows));
  Object a = *o;
  bool ok = false;
  const char* label = "Table Row";
  switch (op) {
    case 0: ok = tableInsertRow(t, r); a.h += rowH; break;
    case 1: ok = tableInsertRow(t, r + 1); a.h += rowH; break;
    case 2: ok = tableDeleteRow(t, r); a.h = std::max(24, a.h - rowH); break;
    case 3: ok = tableInsertCol(t, c); label = "Table Column"; break;
    case 4: ok = tableInsertCol(t, c + 1); label = "Table Column"; break;
    case 5: ok = tableDeleteCol(t, c); label = "Table Column"; break;
  }
  if (!ok) return;
  a.h = std::min(a.h, 4000);
  a.setProp("data", formatTable(t));
  doc_->updateObject(a, label);
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::beginTextEdit() {
  if (!edit_ || sel_.size() != 1 || !doc_) return;
  const Object* o = doc_->object(sel_[0]);
  if (!o) return;
  if (o->type != ObjType::Text && o->type != ObjType::Button) return editProperties();  // rect, image: nothing to type into
  endEdit(true, false);

  RECT r = objRectPx(*o);
  bool multi = o->type == ObjType::Text;
  int w = std::max<int>(r.right - r.left, MulDiv(90, dpi_, 96));
  int h = std::max<int>(r.bottom - r.top, MulDiv(multi ? 32 : 26, dpi_, 96));
  DWORD st = WS_CHILD | WS_VISIBLE | WS_BORDER;
  st |= multi ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : (ES_AUTOHSCROLL | ES_CENTER);
  std::wstring text = toEditText(o->prop(multi ? "text" : "label"));
  static HMODULE richLib = multi ? LoadLibraryW(L"Msftedit.dll") : nullptr;
  editRich_ = false;
  if (multi && richLib)
    editWnd_ = CreateWindowExW(0, L"RICHEDIT50W", text.c_str(), st, r.left, r.top, w, h, hwnd_,
                               (HMENU)(INT_PTR)1, hi_, nullptr);
  if (editWnd_) editRich_ = true;
  if (!editWnd_)
    editWnd_ = CreateWindowExW(0, L"EDIT", text.c_str(), st, r.left, r.top, w, h, hwnd_,
                               (HMENU)(INT_PTR)1, hi_, nullptr);
  g_editRich = editRich_;
  if (editRich_) {
    SendMessageW(editWnd_, EM_SETEVENTMASK, 0, ENM_CHANGE);  // EN_CHANGE keeps runs in step
    SendMessageW(editWnd_, EM_SETUNDOLIMIT, 0, 0);           // we keep the history ourselves
  }
  editUndo_.clear();
  editRedo_.clear();
  pendActive_ = false;
  restoring_ = false;
  lastTyping_ = false;
  lastEditTick_ = 0;
  editId_ = o->id;
  SendMessageW(editWnd_, EM_LIMITTEXT, 1000000, 0);
  editIsText_ = multi;
  editSynced_ = multi ? o->prop("text") : std::string();
  editRuns_ = multi ? decodeRuns(o->prop("runs"), editSynced_.size()) : Runs{};
  SendMessageW(editWnd_, WM_SETFONT,
               (WPARAM)fontFor(o->propInt("size", multi ? 14 : 13), o->propInt("bold", 0) != 0), TRUE);
  g_oldEditProc = (WNDPROC)SetWindowLongPtrW(editWnd_, GWLP_WNDPROC, (LONG_PTR)editProc);
  applyEditStyling();
  editCur_ = {editSynced_, encodeRuns(editRuns_), editSynced_.size()};
  SetFocus(editWnd_);
  SendMessageW(editWnd_, EM_SETSEL, 0, -1);
}

void Canvas::endEdit(bool commit, bool refocus) {
  if (!editWnd_) return;
  HWND e = editWnd_;
  int64_t id = editId_;
  editWnd_ = nullptr;  // first, so the WM_KILLFOCUS that DestroyWindow causes finds nothing to do
  editId_ = 0;
  std::wstring w = readEditText(e, editRich_);
  DestroyWindow(e);
  std::string newText = fromEditText(w);
  if (editRow_ >= 0) {  // a table cell
    size_t row = (size_t)editRow_, col = (size_t)editCol_;
    editRow_ = editCol_ = -1;
    if (commit && doc_)
      if (const Object* o = doc_->object(id)) {
        Table t = parseTable(o->prop("data"));
        if (tableSetCell(t, row, col, newText)) {
          Object a = *o;
          a.setProp("data", formatTable(t));
          if (!(a.props == o->props)) doc_->updateObject(a, "Edit Cell");
        }
      }
    if (refocus) SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  if (editIsText_) {
    shiftRunsForEdit(editRuns_, editSynced_, newText);  // no-op if EN_CHANGE already kept it in step
    editSynced_ = newText;
  }
  if (commit && doc_) {
    if (const Object* o = doc_->object(id)) {
      Object a = *o;
      a.setProp(o->type == ObjType::Text ? "text" : "label", newText);
      if (o->type == ObjType::Text) {
        std::string enc = encodeRuns(editRuns_);
        if (enc.empty()) a.props.erase("runs"); else a.setProp("runs", enc);
        // A text box grows to show what was typed or pasted (it never shrinks on its own).
        int need = measureRichHeight(newText, enc, o->propInt("size", 14), o->w);
        if (need > a.h) a.h = std::min(need, 4000);
      }
      if (!(a.props == o->props) || a.h != o->h) doc_->updateObject(a, "Edit Text");
    }
  }
  editRuns_.clear();
  editSynced_.clear();
  editUndo_.clear();
  editRedo_.clear();
  editIsText_ = false;
  editRich_ = false;
  g_editRich = false;
  if (refocus) SetFocus(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ------------------------------------------------------------------ rich text

std::wstring Canvas::editText() const { return readEditText(editWnd_, editRich_); }

// The editor's selection as UTF-8 byte offsets into the text it holds (a <= b).
bool Canvas::editSelBytes(size_t& a, size_t& b) {
  if (!editWnd_) return false;
  DWORD s0 = 0, e0 = 0;
  std::wstring all = editText();
  if (editRich_) {  // character positions count "\r\n" as one; map them onto the CRLF text
    CHARRANGE cr{0, 0};
    SendMessageW(editWnd_, EM_EXGETSEL, 0, (LPARAM)&cr);
    auto toCrlf = [&](LONG cp) {
      size_t i = 0;
      LONG seen = 0;
      while (i < all.size() && seen < cp) {
        i += (all[i] == L'\r' && i + 1 < all.size() && all[i + 1] == L'\n') ? 2 : 1;
        ++seen;
      }
      return (DWORD)i;
    };
    s0 = toCrlf(cr.cpMin);
    e0 = toCrlf(cr.cpMax);
  } else {
    SendMessageW(editWnd_, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
  }
  if (s0 > e0) std::swap(s0, e0);
  a = fromEditText(all.substr(0, std::min<size_t>(s0, all.size()))).size();
  b = fromEditText(all.substr(0, std::min<size_t>(e0, all.size()))).size();
  return true;
}

// Notes the editor's current text/styling as a step in its own undo history. Typing within a
// second of the last typing is one step; formatting and pastes are steps of their own.
void Canvas::recordEdit(bool typing) {
  if (!editWnd_ || !editRich_ || !editIsText_ || restoring_) return;
  size_t a = 0, b = 0;
  editSelBytes(a, b);
  EditSnap now{editSynced_, encodeRuns(editRuns_), b};
  if (now.text == editCur_.text && now.runs == editCur_.runs) {
    editCur_.caret = b;
    return;
  }
  unsigned long t = GetTickCount();
  bool merge = typing && lastTyping_ && !editUndo_.empty() && t - lastEditTick_ < 1000;
  if (!merge) {
    editUndo_.push_back(editCur_);
    if (editUndo_.size() > 300) editUndo_.erase(editUndo_.begin());
  }
  editRedo_.clear();
  editCur_ = now;
  lastEditTick_ = t;
  lastTyping_ = typing;
}

void Canvas::editUndoRedo(bool redo) {
  if (!editWnd_ || !editRich_ || !editIsText_) return;
  auto& from = redo ? editRedo_ : editUndo_;
  auto& to = redo ? editUndo_ : editRedo_;
  if (from.empty()) return;
  size_t a = 0, b = 0;
  editSelBytes(a, b);
  editCur_.caret = b;
  to.push_back(editCur_);
  EditSnap snap = from.back();
  from.pop_back();
  restoring_ = true;
  pendActive_ = false;
  editSynced_ = snap.text;
  editRuns_ = decodeRuns(snap.runs, snap.text.size());
  SetWindowTextW(editWnd_, toEditText(snap.text).c_str());
  applyEditStyling();
  CHARRANGE cr{cpOfByte(snap.text, snap.caret), cpOfByte(snap.text, snap.caret)};
  SendMessageW(editWnd_, EM_EXSETSEL, 0, (LPARAM)&cr);
  editCur_ = snap;
  lastTyping_ = false;
  restoring_ = false;
}

// Paints editRuns_ onto the live editor: base style for everything, then each run on top. The
// runs stay the source of truth; the control only shows them.
void Canvas::applyEditStyling() {
  if (!editWnd_ || !editRich_ || !editIsText_ || styling_ || !doc_) return;
  const Object* o = doc_->object(editId_);
  if (!o || editSynced_.size() > 20000) return;
  styling_ = true;
  HFONT base = fontFor(o->propInt("size", 14), o->propInt("bold", 0) != 0);
  LOGFONTW lf{};
  GetObjectW(base, sizeof lf, &lf);
  HDC dc = GetDC(editWnd_);
  int dpiY = std::max(1, GetDeviceCaps(dc, LOGPIXELSY));
  ReleaseDC(editWnd_, dc);
  int basePx = std::abs(lf.lfHeight);
  auto twips = [&](int pct) { return (LONG)((long long)basePx * (pct > 0 ? pct : 100) * 1440 / (100LL * dpiY)); };

  CHARRANGE keep{0, 0};
  SendMessageW(editWnd_, EM_EXGETSEL, 0, (LPARAM)&keep);
  DWORD oldMask = (DWORD)SendMessageW(editWnd_, EM_SETEVENTMASK, 0, 0);
  SendMessageW(editWnd_, WM_SETREDRAW, FALSE, 0);

  auto setRange = [&](LONG a, LONG b, const CHARFORMAT2W& cf) {
    CHARRANGE cr{a, b};
    SendMessageW(editWnd_, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(editWnd_, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
  };
  CHARFORMAT2W bf{};
  bf.cbSize = sizeof bf;
  bf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_COLOR | CFM_SIZE | CFM_FACE;
  bf.dwEffects = o->propInt("bold", 0) ? CFE_BOLD : 0;
  bf.yHeight = twips(100);
  bf.crTextColor = toColorRef(o->propInt("color", 0x222222));
  lstrcpynW(bf.szFaceName, lf.lfFaceName, LF_FACESIZE);
  setRange(0, -1, bf);

  for (const Run& r : editRuns_) {
    if (r.len == 0) continue;
    CHARFORMAT2W cf = bf;
    cf.dwEffects = (o->propInt("bold", 0) || (r.flags & kBold) ? CFE_BOLD : 0) | ((r.flags & kItalic) ? CFE_ITALIC : 0) |
                   ((r.flags & kUnderline) || !r.url.empty() ? CFE_UNDERLINE : 0) | ((r.flags & kStrike) ? CFE_STRIKEOUT : 0);
    if (r.color >= 0) cf.crTextColor = toColorRef(r.color);
    else if (!r.url.empty()) cf.crTextColor = RGB(0x0B, 0x57, 0xD0);
    if (r.size > 0) cf.yHeight = twips(r.size);
    if (r.flags & kMono) lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    setRange(cpOfByte(editSynced_, r.start), cpOfByte(editSynced_, r.end()), cf);
  }

  SendMessageW(editWnd_, EM_EXSETSEL, 0, (LPARAM)&keep);
  SendMessageW(editWnd_, EM_SETEVENTMASK, 0, (LPARAM)oldMask);
  SendMessageW(editWnd_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(editWnd_, nullptr, TRUE);
  styling_ = false;
}

void Canvas::syncEditRuns() {
  if (!editWnd_ || !editIsText_) return;
  std::string now = fromEditText(editText());
  if (now == editSynced_) return;
  std::string before = editSynced_;
  shiftRunsForEdit(editRuns_, editSynced_, now);
  if (pendActive_) applyTypingStyle(editRuns_, before, now, pendCaret_, pendFlags_);  // refuses if the caret moved
  pendActive_ = false;  // the pending style is spent by the next edit, applied or not
  editSynced_ = now;
}

unsigned Canvas::editStyleFlags() {
  if (!editWnd_ || !editRich_ || !editIsText_) return 0;
  size_t a = 0, b = 0;
  editSelBytes(a, b);
  if (pendActive_ && a == b && a == pendCaret_) return pendFlags_;
  if (a == b) return styleBefore(editRuns_, a);
  unsigned f = 0;
  for (unsigned bit : {(unsigned)kBold, (unsigned)kItalic, (unsigned)kUnderline, (unsigned)kStrike, (unsigned)kMono})
    if (rangeAllHave(editRuns_, a, b, bit)) f |= bit;
  return f;
}

bool Canvas::askFormat(int cmd, const Runs& runs, size_t a, size_t b, FormatArg& arg) {
  static const unsigned flagOf[] = {0, kBold, kItalic, kUnderline, kStrike};
  if (cmd >= FmtBold && cmd <= FmtStrike) {
    arg.set = !rangeAllHave(runs, a, b, flagOf[cmd]);
    return true;
  }
  if (cmd == FmtCode) {
    arg.set = !rangeAllHave(runs, a, b, kMono);
    return true;
  }
  if (cmd == FmtClear) return true;
  const Run* first = nullptr;  // current value shown in the dialog: that of the first styled run
  for (const Run& r : runs)
    if (r.end() > a && r.start < b) { first = &r; break; }
  HWND owner = GetParent(hwnd_);
  if (cmd == FmtLink) {
    PropForm f(owner, L"Link");
    int ia = f.addText(L"Link address", widen(first ? first->url : ""));
    if (!f.run()) return false;
    std::string typed = narrow(f.text(ia));
    arg.url = sanitizeLinkTarget(typed);
    if (arg.url.empty() && !typed.empty()) {
      MessageBoxW(owner, L"That doesn't look like a web address Cairn will open.\n\nUse http://, https://, mailto: or something like example.com.",
                  L"Cairn", MB_OK | MB_ICONINFORMATION);
      return false;
    }
    return true;
  }
  PropForm f(owner, L"Text color");
  int ic = f.addColor(L"Color", first && first->color >= 0 ? first->color : -1, true);
  if (!f.run()) return false;
  arg.color = f.color(ic);
  return true;
}

void Canvas::applyFormatArg(Runs& runs, size_t textLen, size_t a, size_t b, int cmd, const FormatArg& arg) {
  static const unsigned flagOf[] = {0, kBold, kItalic, kUnderline, kStrike};
  auto setFlag = [&](unsigned fl) {
    restyleRange(runs, a, b, textLen, [&](Run& r) { r.flags = arg.set ? (r.flags | fl) : (r.flags & ~fl); });
  };
  switch (cmd) {
    case FmtBold: case FmtItalic: case FmtUnderline: case FmtStrike: setFlag(flagOf[cmd]); break;
    case FmtCode: setFlag(kMono); break;
    case FmtLink: restyleRange(runs, a, b, textLen, [&](Run& r) { r.url = arg.url; }); break;
    case FmtColor: restyleRange(runs, a, b, textLen, [&](Run& r) { r.color = arg.color; }); break;
    case FmtClear: restyleRange(runs, a, b, textLen, [&](Run& r) { r.flags = 0; r.color = -1; r.size = 0; r.url.clear(); }); break;
  }
}

void Canvas::changeTextSize(int dir) {
  if (!doc_ || !curPage() || dir == 0) return;
  dir = dir > 0 ? 1 : -1;
  if (editWnd_) {  // the selected characters of the box being edited
    if (!editIsText_) return;
    syncEditRuns();
    size_t a = 0, b = 0;
    editSelBytes(a, b);
    if (a >= b) return;
    restyleRange(editRuns_, a, b, editSynced_.size(), [&](Run& r) {
      int next = stepSizePercent(r.size, dir);
      r.size = next == 100 ? 0 : next;
    });
    applyEditStyling();
    recordEdit(false);
    SetFocus(editWnd_);
    return;
  }
  if (!edit_) return;
  std::vector<Object> changed;
  for (int64_t id : sel_) {
    const Object* o = doc_->object(id);
    if (!o || (o->type != ObjType::Text && o->type != ObjType::Button)) continue;
    Object n = *o;
    int cur = o->propInt("size", o->type == ObjType::Text ? 14 : 13);
    n.setPropInt("size", stepFontPt(cur, dir));
    if (o->type == ObjType::Text) {  // a bigger font needs a taller box (it never shrinks on its own)
      std::string rn = o->prop("runs");
      int need = measureRichHeight(o->prop("text"), rn, n.propInt("size", 14), o->w);
      if (dir > 0 && need > n.h) n.h = std::min(need, 4000);
    }
    if (!(n.props == o->props) || n.h != o->h) changed.push_back(n);
  }
  if (!changed.empty()) doc_->updateObjects(changed, dir > 0 ? "Bigger Text" : "Smaller Text");
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::applyFormat(int cmd) {
  if (!doc_ || !curPage() || cmd < FmtBold || cmd > FmtCode) return;
  if (editWnd_) {  // formatting the selected characters of the box being edited
    if (!editIsText_) return;
    syncEditRuns();
    size_t a = 0, b = 0;
    editSelBytes(a, b);
    if (a >= b) {  // nothing selected: toggle the style of the text about to be typed
      static const unsigned flagOf[] = {0, kBold, kItalic, kUnderline, kStrike};
      unsigned bit = cmd >= FmtBold && cmd <= FmtStrike ? flagOf[cmd] : cmd == FmtCode ? (unsigned)kMono : 0u;
      if (!bit) return;
      if (!pendActive_ || pendCaret_ != a) {
        pendFlags_ = styleBefore(editRuns_, a);
        pendCaret_ = a;
        pendActive_ = true;
      }
      pendFlags_ ^= bit;
      SetFocus(editWnd_);
      return;
    }
    FormatArg arg;
    inFormat_ = true;  // dialogs steal focus; that must not end the edit
    bool ok = askFormat(cmd, editRuns_, a, b, arg);
    inFormat_ = false;
    if (ok) applyFormatArg(editRuns_, editSynced_.size(), a, b, cmd, arg);
    applyEditStyling();
    recordEdit(false);
    SetFocus(editWnd_);
    return;
  }
  if (!edit_) return;
  std::vector<Object> changed;
  FormatArg arg;
  bool asked = false;
  for (int64_t id : sel_) {
    const Object* o = doc_->object(id);
    if (!o || o->type != ObjType::Text) continue;
    std::string text = o->prop("text");
    Runs runs = decodeRuns(o->prop("runs"), text.size());
    if (text.empty()) continue;
    if (!asked) {
      if (!askFormat(cmd, runs, 0, text.size(), arg)) return;
      asked = true;
    }
    applyFormatArg(runs, text.size(), 0, text.size(), cmd, arg);
    Object n = *o;
    std::string enc = encodeRuns(runs);
    if (enc.empty()) n.props.erase("runs"); else n.setProp("runs", enc);
    if (!(n.props == o->props)) changed.push_back(n);
  }
  if (!changed.empty()) doc_->updateObjects(changed, "Format Text");
  InvalidateRect(hwnd_, nullptr, FALSE);
}

namespace {
bool readHtmlClip(HWND owner, bool alreadyOpen, std::string& frag, std::string& src) {
  UINT fmt = RegisterClipboardFormatW(L"HTML Format");
  bool opened = alreadyOpen || OpenClipboard(owner);
  if (!opened) return false;
  std::string raw;
  bool ok = false;
  if (HANDLE h = GetClipboardData(fmt)) {
    if (const char* p = (const char*)GlobalLock(h)) {
      size_t n = GlobalSize(h);
      raw.assign(p, strnlen(p, n));
      GlobalUnlock(h);
      ok = !raw.empty();
    }
  }
  if (!alreadyOpen) CloseClipboard();
  if (ok) frag = htmlFragmentFromClipboard(raw, &src);
  return ok;
}
}  // namespace

void Canvas::pasteRichIntoEditor() {
  if (!editWnd_ || !editIsText_) return;
  std::string frag, src;
  RichText rt;
  if (readHtmlClip(hwnd_, false, frag, src)) rt = htmlToRich(frag, src);
  if (rt.text.empty()) {  // nothing usable in the HTML: let the edit box paste plain text itself
    SendMessageW(editWnd_, WM_PASTE, 0, 0);
    return;
  }
  syncEditRuns();
  size_t a = 0, b0 = 0;
  editSelBytes(a, b0);
  SendMessageW(editWnd_, EM_REPLACESEL, TRUE, (LPARAM)toEditText(rt.text).c_str());  // EN_CHANGE shifts the runs
  syncEditRuns();
  overlayRuns(editRuns_, rt.runs, a, rt.text.size(), editSynced_.size());
  applyEditStyling();
  recordEdit(false);
}

int Canvas::measureRichHeight(const std::string& utf8, const std::string& runsEnc, int pt, int widthUnits) {
  RichModel rm = buildRich(utf8, runsEnc);
  HDC dc = GetDC(hwnd_);
  FontOf fo = [this, pt](unsigned flags, int pct) {
    return fontStyled(std::max(4, (int)std::lround(pt * (pct ? pct : 100) / 100.0)), flags);
  };
  std::vector<Piece> pieces;
  int totalH = 0;
  layoutRich(dc, rm, std::max(10, (int)std::lround((widthUnits - 8) * scale())), 0, fo, pieces, totalH);
  ReleaseDC(hwnd_, dc);
  return std::clamp((int)std::ceil(totalH / scale()) + 10, 28, 4000);
}

// ---------------------------------------------------------------- commands

void Canvas::addObject(ObjType t, int cx, int cy) {
  const Page* p = curPage();
  if (!p) return;
  if (!edit_) setEditMode(true);
  endEdit(true, false);
  Object o;
  o.type = t;
  if (t == ObjType::Text) {
    o.w = 240; o.h = 40;
    o.setProp("text", "Text");
    o.setPropInt("size", 14);
    o.setPropInt("color", 0x222222);
  } else if (t == ObjType::Table) {
    o.w = 360; o.h = 100;
    o.setProp("data", "| Column 1 | Column 2 | Column 3 |\n| a | b | c |\n| d | e | f |\n");
    o.setPropInt("header", 1);
    o.setPropInt("size", 12);
  } else if (t == ObjType::Folder) {
    o.w = 280; o.h = 220;
    o.setProp("path", ".");
    o.setPropInt("size", 12);
  } else if (t == ObjType::Button) {
    o.w = 160; o.h = 40;
    o.setProp("label", "Button");
    o.setPropInt("size", 13);
  } else {
    o.w = 240; o.h = 140;
    o.setPropInt("fill", 0xDDE6F0);
    o.setPropInt("line", 0x7A93AD);
    o.setPropInt("lw", 1);
    o.setPropInt("round", 6);
  }
  placeNew(o.w, o.h, cx, cy, o.x, o.y);  // at the given client point, or staggered in view
  int64_t id = doc_->addObject(pageId_, o, "Add Object");
  sel_ = {id};
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
  if (t == ObjType::Folder || t == ObjType::Table) editProperties();
  else if (t != ObjType::Rect) beginTextEdit();
}

void Canvas::deleteSelection() {
  if (!edit_ || sel_.empty()) return;
  std::vector<int64_t> ids = sel_;
  sel_.clear();
  doc_->deleteObjects(pageId_, ids, "Delete");
}

void Canvas::duplicateSelection() {
  const Page* p = curPage();
  if (!edit_ || !p || sel_.empty()) return;
  std::vector<Object> copies;
  for (const Object& o : p->objects)
    if (isSelected(o.id)) {
      Object c = o;
      c.x += 16;
      c.y += 16;
      copies.push_back(c);
    }
  auto ids = doc_->addObjects(pageId_, copies, "Duplicate");
  sel_ = ids;
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::selectAll() {
  const Page* p = curPage();
  if (!edit_ || !p) return;
  sel_.clear();
  for (const Object& o : p->objects) sel_.push_back(o.id);
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

void Canvas::nudge(int dx, int dy) {
  if (!edit_ || sel_.empty()) return;
  std::vector<Object> v;
  for (int64_t id : sel_)
    if (const Object* o = doc_->object(id)) {
      Object n = *o;
      n.x += dx;
      n.y += dy;
      v.push_back(n);
    }
  doc_->updateObjects(v, "Nudge");
}

void Canvas::bringToFront() {
  if (edit_ && !sel_.empty()) doc_->reorder(pageId_, sel_, true);
}
void Canvas::sendToBack() {
  if (edit_ && !sel_.empty()) doc_->reorder(pageId_, sel_, false);
}

void Canvas::editPageProperties() {
  const Page* p = curPage();
  if (!p) return;
  PropForm f(GetParent(hwnd_), L"Page Properties");
  int iw = f.addNumber(L"Width", p->w, 200, 8000);
  int ih = f.addNumber(L"Height", p->h, 200, 8000);
  int ibg = f.addColor(L"Background", p->bg, false);
  int itags = f.addText(L"Tags (comma separated)", widen(p->tags));
  if (f.run()) {
    std::string tags = narrow(f.text(itags));
    doc_->setPageProps(pageId_, f.number(iw), f.number(ih), f.color(ibg), &tags);
  }
}

void Canvas::editProperties() {
  if (!edit_ || !doc_) return;
  if (sel_.empty()) return editPageProperties();
  if (sel_.size() != 1) return;
  const Object* o = doc_->object(sel_[0]);
  if (!o) return;
  endEdit(true, false);
  Object a = *o;

  PropForm f(GetParent(hwnd_), std::wstring(typeName(a.type)) + L" Properties");
  int ix = f.addNumber(L"X", a.x, -8000, 8000), iy = f.addNumber(L"Y", a.y, -8000, 8000);
  int iw = f.addNumber(L"Width", a.w, kMinSize, 8000), ih = f.addNumber(L"Height", a.h, kMinSize, 8000);

  int itext = -1, ialign = -1, isize = -1, ibold = -1, icolor = -1, ifill = -1, itarget = -1;
  int iline = -1, ilw = -1, iround = -1, iopen = -1, ifit = -1, irestore = -1;
  std::vector<int64_t> pageIds;

  if (a.type == ObjType::Text) {
    itext = f.addText(L"Text", toEditText(a.prop("text")), true);
    isize = f.addNumber(L"Font size (pt)", a.propInt("size", 14), 6, 200);
    ibold = f.addCheck(L"Bold", a.propInt("bold", 0) != 0);
    ialign = f.addCombo(L"Align", {L"Left", L"Center", L"Right"}, std::clamp(a.propInt("align", 0), 0, 2));
    icolor = f.addColor(L"Text color", a.propInt("color", 0x222222), false);
    ifill = f.addColor(L"Fill", a.propInt("fill", -1), true);
  } else if (a.type == ObjType::Button) {
    itext = f.addText(L"Label", widen(a.prop("label")));
    std::vector<std::wstring> items = {L"(none)"};
    int sel = 0;
    int64_t cur = a.propInt("target", 0);
    for (const Page& pg : doc_->pages()) {
      pageIds.push_back(pg.id);
      items.push_back(widen(pg.title));
      if (pg.id == cur) sel = (int)pageIds.size();
    }
    itarget = f.addCombo(L"Go to page", items, sel);
    iopen = f.addText(L"Open file or URL", widen(a.prop("open")));
    isize = f.addNumber(L"Font size (pt)", a.propInt("size", 13), 6, 200);
    ibold = f.addCheck(L"Bold", a.propInt("bold", 0) != 0);
    icolor = f.addColor(L"Text color", a.propInt("color", 0x000000), false);
    ifill = f.addColor(L"Fill", a.propInt("fill", -1), true);
  } else if (a.type == ObjType::Audio) {
    itext = f.addText(L"Name", widen(a.prop("name")));
    ifit = f.addCheck(L"Loop", a.propInt("loop", 0) != 0);
    irestore = f.addCheck(L"Play when the page opens (Run mode)", a.propInt("auto", 0) != 0);
  } else if (a.type == ObjType::Table) {
    itext = f.addText(L"Cells: one row per line, cells between | bars (a literal bar is \\|)",
                      toEditText(a.prop("data")), true);
    isize = f.addNumber(L"Font size (pt)", a.propInt("size", 12), 6, 200);
    ibold = f.addCheck(L"First row is a header", a.propInt("header", 1) != 0);
  } else if (a.type == ObjType::Folder) {
    itext = f.addText(L"Folder (absolute, or relative to this .cairn file)", widen(a.prop("path", ".")));
    isize = f.addNumber(L"Font size (pt)", a.propInt("size", 12), 6, 200);
    ifill = f.addColor(L"Fill", a.propInt("fill", 0xFFFFFF), true);
  } else if (a.type == ObjType::Image) {
    itext = f.addText(L"Name", widen(a.prop("name")));
    ifit = f.addCombo(L"Scaling", {L"Stretch to the box", L"Keep proportions"}, a.propInt("fit", 1) ? 1 : 0);
    irestore = f.addCheck(L"Restore original proportions (keeps width)", false);
  } else {
    ifill = f.addColor(L"Fill", a.propInt("fill", -1), true);
    iline = f.addColor(L"Line color", a.propInt("line", 0x555555), true);
    ilw = f.addNumber(L"Line width", a.propInt("lw", 1), 0, 50);
    iround = f.addNumber(L"Corner radius", a.propInt("round", 0), 0, 200);
  }
  if (!f.run()) return;

  a.x = f.number(ix); a.y = f.number(iy); a.w = f.number(iw); a.h = f.number(ih);
  if (a.type == ObjType::Text) {
    a.setProp("text", fromEditText(f.text(itext)));
    a.setProp("align", std::to_string(f.combo(ialign)));
  } else if (a.type == ObjType::Button) {
    a.setProp("label", narrow(f.text(itext)));
    int ti = f.combo(itarget);
    a.setPropInt("target", ti > 0 ? (long)pageIds[(size_t)ti - 1] : 0);
    std::string open = narrow(f.text(iopen));
    while (!open.empty() && (open.back() == ' ' || open.front() == ' ')) {
      if (open.back() == ' ') open.pop_back(); else open.erase(open.begin());
    }
    if (open.empty()) a.props.erase("open"); else a.setProp("open", open);
  } else if (a.type == ObjType::Audio) {
    a.setProp("name", narrow(f.text(itext)));
    a.setPropInt("loop", f.check(ifit) ? 1 : 0);
    a.setPropInt("auto", f.check(irestore) ? 1 : 0);
  } else if (a.type == ObjType::Table) {
    a.setProp("data", formatTable(parseTable(fromEditText(f.text(itext)))));
    a.setPropInt("header", f.check(ibold) ? 1 : 0);
  } else if (a.type == ObjType::Folder) {
    std::string pth = narrow(f.text(itext));
    while (!pth.empty() && pth.back() == ' ') pth.pop_back();
    a.setProp("path", pth.empty() ? "." : pth);
    folderCache_.erase(a.id);
    folderScroll_.erase(a.id);
  } else if (a.type == ObjType::Image) {
    a.setProp("name", narrow(f.text(itext)));
    a.setPropInt("fit", f.combo(ifit));
    int iw = a.propInt("iw", 0), ih = a.propInt("ih", 0);
    if (f.check(irestore) && iw > 0 && ih > 0) a.h = std::max(kMinSize, (int)std::lround(a.w * (double)ih / iw));
  }
  if (isize >= 0) a.setPropInt("size", f.number(isize));
  if (ibold >= 0) a.setPropInt("bold", f.check(ibold) ? 1 : 0);
  if (icolor >= 0) a.setPropInt("color", f.color(icolor));
  if (ifill >= 0) a.setPropInt("fill", f.color(ifill));
  if (iline >= 0) a.setPropInt("line", f.color(iline));
  if (ilw >= 0) a.setPropInt("lw", f.number(ilw));
  if (iround >= 0) a.setPropInt("round", f.number(iround));
  doc_->updateObject(a, "Properties");
}

// ---------------------------------------------------------------- messages

// Anything a handler throws (a full disk during a drag-commit, an allocation failure)
// must not escape a window procedure: that would terminate the process and lose the
// session. Report it, resync with the document, and carry on.
LRESULT Canvas::proc(UINT m, WPARAM w, LPARAM l) {
  try {
    return procInner(m, w, l);
  } catch (const std::exception& e) {
    cancelDrag();
    MessageBoxW(GetParent(hwnd_), widen(std::string("That didn't work: ") + e.what()).c_str(), L"Cairn",
                MB_OK | MB_ICONWARNING);
    docChanged();
    return 0;
  }
}

LRESULT Canvas::procInner(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_DROPFILES: {
      HDROP hd = (HDROP)w;
      POINT pt{0, 0};
      DragQueryPoint(hd, &pt);
      UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> files;
      for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(hd, i, nullptr, 0);
        std::wstring f((size_t)len + 1, L'\0');
        DragQueryFileW(hd, i, &f[0], len + 1);
        f.resize(len);
        files.push_back(f);
      }
      DragFinish(hd);
      addFilesAt(files, pt.x, pt.y);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      paint(dc);
      if (marked_) {  // split view: the pane that commands go to
        RECT cr;
        GetClientRect(hwnd_, &cr);
        HBRUSH mb = CreateSolidBrush(RGB(0x2F, 0x6F, 0xDE));
        RECT top{cr.left, cr.top, cr.right, cr.top + 3};
        FillRect(dc, &top, mb);
        DeleteObject(mb);
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_SIZE:
      updateScroll();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
      int bar = (m == WM_HSCROLL) ? SB_HORZ : SB_VERT;
      SCROLLINFO si{};
      si.cbSize = sizeof si;
      si.fMask = SIF_ALL;
      GetScrollInfo(hwnd_, bar, &si);
      int pos = si.nPos, line = MulDiv(40, dpi_, 96);
      switch (LOWORD(w)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
      }
      setScroll(bar, pos);
      return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      int d = GET_WHEEL_DELTA_WPARAM(w);
      if (m == WM_MOUSEWHEEL && (LOWORD(w) & MK_CONTROL)) {
        zoomStep(d > 0 ? 1 : -1);
      } else if (m == WM_MOUSEWHEEL && doc_ && [&] {
                   POINT mp{(short)LOWORD(l), (short)HIWORD(l)};
                   ScreenToClient(hwnd_, &mp);
                   const Object* fo = objectAt(mp.x, mp.y);
                   if (!fo || fo->type != ObjType::Folder) return false;
                   int total = (int)folderItems(*fo).items.size(), &sc = folderScroll_[fo->id];
                   sc = std::clamp(sc + (d > 0 ? -3 : 3), 0, std::max(0, total - 1));
                   InvalidateRect(hwnd_, nullptr, FALSE);
                   return true;
                 }()) {
        // scrolled a folder listing
      } else {
        int px = -d * MulDiv(60, dpi_, 96) / 120;
        bool horiz = (m == WM_MOUSEHWHEEL) || (LOWORD(w) & MK_SHIFT);
        if (m == WM_MOUSEHWHEEL) px = -px;
        horiz ? scrollBy(px, 0) : scrollBy(0, px);
      }
      return 0;
    }
    case WM_TIMER:
      if (w == 1) animTick();
      return 0;
    case WM_RBUTTONDOWN: {
      if (!doc_ || !curPage()) break;
      SetFocus(hwnd_);
      int x = (short)LOWORD(l), y = (short)HIWORD(l);
      ctx_ = {x, y};
      ctxLink_.clear();
      const Object* o = objectAt(x, y);
      if (o) ctxLink_ = urlAt(*o, x, y);
      if (edit_) {  // right-clicking an unselected object selects it; empty space clears
        if (o && !isSelected(o->id)) sel_ = {o->id};
        else if (!o) sel_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notify();
      }
      break;  // DefWindowProc turns the release into WM_CONTEXTMENU
    }
    case WM_CONTEXTMENU: {
      if (!doc_ || !curPage()) return 0;
      POINT scr = {(short)LOWORD(l), (short)HIWORD(l)};
      if (l == -1) {  // keyboard (Menu key / Shift+F10): anchor on the selection or the view centre
        RECT rc;
        GetClientRect(hwnd_, &rc);
        POINT c = {(rc.right) / 2, (rc.bottom) / 2};
        if (!sel_.empty())
          if (const Object* o = doc_->object(sel_[0])) {
            RECT r = objRectPx(*o);
            c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
          }
        ctx_ = c;
        ctxLink_.clear();
        scr = c;
        ClientToScreen(hwnd_, &scr);
      }
      if (onContextMenu) onContextMenu(scr);
      return 0;
    }
    case WM_SETFOCUS:
      if (onActivate) onActivate();
      break;
    case WM_LBUTTONDOWN: if (onActivate) onActivate(); onLButtonDown((short)LOWORD(l), (short)HIWORD(l), w); return 0;
    case WM_LBUTTONDBLCLK: onDoubleClick((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_LBUTTONUP: onLButtonUp((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: onMouseMove((short)LOWORD(l), (short)HIWORD(l), w); return 0;
    case WM_CAPTURECHANGED:
      if (drag_ != Drag::None && (HWND)l != hwnd_) cancelDrag();
      return 0;
    case WM_SETCURSOR: {
      if (LOWORD(l) == HTCLIENT && doc_) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        LPCWSTR cur = IDC_ARROW;
        if (edit_) {
          int h = handleAt(pt.x, pt.y);
          if (h == 0 || h == 4) cur = IDC_SIZENWSE;
          else if (h == 2 || h == 6) cur = IDC_SIZENESW;
          else if (h == 1 || h == 5) cur = IDC_SIZENS;
          else if (h == 3 || h == 7) cur = IDC_SIZEWE;
          else if (objectAt(pt.x, pt.y)) cur = IDC_SIZEALL;
        } else if (const Object* o = objectAt(pt.x, pt.y)) {
          if (o->type == ObjType::Button || o->type == ObjType::Audio || (o->type == ObjType::Text && !urlAt(*o, pt.x, pt.y).empty()) ||
              (o->type == ObjType::Folder && folderRowAt(*o, pt.x, pt.y) >= 0))
            cur = IDC_HAND;
        }
        SetCursor(LoadCursor(nullptr, cur));
        return TRUE;
      }
      break;
    }
    case WM_KEYDOWN:
      if (onKeyDown(w)) return 0;
      break;
    case WM_COMMAND:
      if (LOWORD(w) == 1 && HIWORD(w) == EN_CHANGE && editWnd_ && editIsText_ && !restoring_ && !styling_) {
        syncEditRuns();
        applyEditStyling();
        recordEdit(true);
      }
      return 0;
    case WM_EDIT_FORMAT:
      applyFormat((int)w);
      return 0;
    case WM_EDIT_UNDO:
      editUndoRedo(w != 0);
      return 0;
    case WM_EDIT_SIZE:
      changeTextSize((int)w);
      return 0;
    case WM_EDIT_PASTE:
      pasteRichIntoEditor();
      return 0;
    case WM_EDIT_COMMIT:
      if (inFormat_) return 0;  // a dialog took focus from the editor; the edit continues afterwards
      if (l && (HWND)l != editWnd_) return 0;  // a late message from an editor that is already gone
      endEdit(true, w != 0);
      return 0;
    case WM_EDIT_CANCEL:
      endEdit(false, true);
      return 0;
    case WM_EDIT_CELLNAV:
      cellNavigate((int)w);
      return 0;
    case MM_MCINOTIFY:
      audioNotify(w);
      return 0;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}


// ------------------------------------------------------------------- media

void Canvas::clearImages() {
  for (auto& kv : images_) {
    if (!kv.second.frames.empty()) {
      for (HBITMAP f : kv.second.frames) DeleteObject(f);  // src is one of these
    } else if (kv.second.src) {
      DeleteObject(kv.second.src);
    }
    if (kv.second.scaled) DeleteObject(kv.second.scaled);
  }
  images_.clear();
}

Canvas::ImgEntry* Canvas::imageFor(int64_t blobId) {
  auto it = images_.find(blobId);
  if (it != images_.end()) return it->second.src ? &it->second : nullptr;
  ImgEntry e;
  std::string bytes;
  Decoded d;
  Animation an;
  if (doc_ && doc_->blob(blobId, bytes)) {
    if (decodeAnimation(bytes, an)) {
      e.frames = an.frames;
      e.delays = an.delaysMs;
      e.src = e.frames[0];
      e.w = an.w;
      e.h = an.h;
      e.next = GetTickCount() + (DWORD)e.delays[0];
      if (!animTimer_) animTimer_ = SetTimer(hwnd_, 1, 20, nullptr) != 0;
    } else if (decodeImage(bytes, d)) {
      e.src = d.bmp;
      e.w = d.w;
      e.h = d.h;
    }
  }
  ImgEntry& slot = (images_[blobId] = e);  // a failed decode is cached too, so we don't retry every paint
  return slot.src ? &slot : nullptr;
}

std::wstring Canvas::folderPath(const Object& o) const {
  std::wstring p = widen(o.prop("path", "."));
  bool drive = p.size() >= 2 && p[1] == L':';
  bool unc = p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\';
  if (!drive && !unc && doc_) {  // relative to the .cairn file, so a library can move as a unit
    std::wstring d = widen(doc_->path());
    size_t sl = d.find_last_of(L"\\/");
    if (sl != std::wstring::npos) p = d.substr(0, sl + 1) + p;
  }
  while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  return p;
}

// Reads the folder at most every two seconds while the page is on screen. Capped at 2000 entries.
const Canvas::FolderCache& Canvas::folderItems(const Object& o) {
  FolderCache& c = folderCache_[o.id];
  std::wstring path = folderPath(o);
  DWORD now = GetTickCount();
  if (c.path == path && c.tick && now - c.tick < 2000) return c;
  c.path = path;
  c.tick = now ? now : 1;
  c.items.clear();
  c.ok = false;
  std::error_code ec;
  std::filesystem::directory_iterator it(std::filesystem::path(path), std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return c;
  c.ok = true;
  for (; !ec && it != std::filesystem::directory_iterator() && c.items.size() < 2000; it.increment(ec)) {
    std::error_code e2;
    FolderEntry fe;
    fe.name = it->path().filename().wstring();
    fe.dir = it->is_directory(e2);
    if (!fe.name.empty() && fe.name[0] != L'.') c.items.push_back(std::move(fe));
  }
  std::sort(c.items.begin(), c.items.end(), [](const FolderEntry& a, const FolderEntry& b) {
    if (a.dir != b.dir) return a.dir;
    return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return c;
}

int Canvas::folderRowAt(const Object& o, int px, int py) const {
  RECT r = objRectPx(eff(o));
  int rowH = std::max(8, (int)std::lround(o.propInt("size", 12) * dpi_ / 72.0 * 1.5 * scale()));
  int top = r.top + rowH + 4;  // header row
  if (px < r.left || px >= r.right || py < top || py >= r.bottom) return -1;
  return (py - top) / rowH;
}


void Canvas::drawTable(HDC dc, const Object& o, const RECT& r) {
  Table t = parseTable(o.prop("data"));
  bool header = o.propInt("header", 1) != 0;
  HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
  FillRect(dc, &r, white);
  DeleteObject(white);
  size_t rows = t.rows.size(), cols = t.cols();
  if (!rows || !cols) {
    FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    return;
  }
  int pt = o.propInt("size", 12);
  int pad = MulDiv(4, dpi_, 96);
  int cw = std::max(1, (int)((r.right - r.left) / cols));
  int rh = std::max(1, (int)((r.bottom - r.top) / rows));
  SetBkMode(dc, TRANSPARENT);
  HPEN grid = CreatePen(PS_SOLID, 1, RGB(138, 138, 138));
  HGDIOBJ oldPen = SelectObject(dc, grid);
  for (size_t ri = 0; ri < rows; ++ri) {
    SelectObject(dc, fontFor(pt, header && ri == 0));
    for (size_t ci = 0; ci < cols; ++ci) {
      RECT cell = tableCellRect(r, rows, cols, ri, ci);
      int left = cell.left, right = cell.right, top = cell.top, bottom = cell.bottom;
      if (header && ri == 0) {
        HBRUSH hb = CreateSolidBrush(RGB(232, 236, 242));
        FillRect(dc, &cell, hb);
        DeleteObject(hb);
      }
      SetTextColor(dc, RGB(30, 30, 30));
      RECT tr{left + pad, top + 1, right - pad, bottom - 1};
      std::wstring w = widen(cellInline(t.rows[ri][ci]));
      DrawTextW(dc, w.c_str(), (int)w.size(), &tr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
  }
  // grid lines
  for (size_t ri = 0; ri <= rows; ++ri) {
    int y = ri == rows ? r.bottom - 1 : r.top + (int)ri * rh;
    MoveToEx(dc, r.left, y, nullptr);
    LineTo(dc, r.right, y);
  }
  for (size_t ci = 0; ci <= cols; ++ci) {
    int x = ci == cols ? r.right - 1 : r.left + (int)ci * cw;
    MoveToEx(dc, x, r.top, nullptr);
    LineTo(dc, x, r.bottom);
  }
  SelectObject(dc, oldPen);
  DeleteObject(grid);
}

void Canvas::drawFolder(HDC dc, const Object& o, const RECT& r) {
  int fill = o.propInt("fill", 0xFFFFFF);
  HBRUSH b = CreateSolidBrush(toColorRef(fill));
  FillRect(dc, &r, b);
  DeleteObject(b);
  FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
  const FolderCache& fc = folderItems(o);
  int pt = o.propInt("size", 12);
  HGDIOBJ oldFont = SelectObject(dc, fontFor(pt, false));
  SetBkMode(dc, TRANSPARENT);
  int rowH = std::max(8, (int)std::lround(pt * dpi_ / 72.0 * 1.5 * scale()));
  int pad = MulDiv(4, dpi_, 96);
  RECT hdr{r.left, r.top, r.right, r.top + rowH + 4};
  HBRUSH hb = CreateSolidBrush(RGB(232, 236, 242));
  FillRect(dc, &hdr, hb);
  DeleteObject(hb);
  std::wstring title = fc.path;
  size_t sl = title.find_last_of(L"\\/");
  if (sl != std::wstring::npos && sl + 1 < title.size()) title = title.substr(sl + 1);
  SetTextColor(dc, RGB(30, 30, 30));
  RECT tr{r.left + pad, hdr.top, r.right - pad, hdr.bottom};
  std::wstring head = fc.ok ? title + L"\\" : L"(can't read that folder)";
  DrawTextW(dc, head.c_str(), (int)head.size(), &tr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  int first = folderScroll_.count(o.id) ? folderScroll_[o.id] : 0;
  int y = hdr.bottom;
  for (size_t i = (size_t)first; i < fc.items.size() && y + rowH <= r.bottom; ++i, y += rowH) {
    const FolderEntry& e = fc.items[i];
    SetTextColor(dc, e.dir ? RGB(20, 60, 140) : RGB(40, 40, 40));
    RECT er{r.left + pad, y, r.right - pad, y + rowH};
    std::wstring label = e.dir ? e.name + L"\\" : e.name;
    DrawTextW(dc, label.c_str(), (int)label.size(), &er, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  }
  if (fc.ok && fc.items.empty()) {
    SetTextColor(dc, RGB(130, 130, 130));
    RECT er{r.left + pad, hdr.bottom, r.right - pad, hdr.bottom + rowH};
    DrawTextW(dc, L"(empty)", -1, &er, DT_VCENTER | DT_SINGLELINE);
  }
  SelectObject(dc, oldFont);
}

void Canvas::drawImage(HDC dc, const Object& o, const RECT& r) {
  ImgEntry* im = imageFor(o.propInt("blob", 0));
  int bw = r.right - r.left, bh = r.bottom - r.top;
  if (!im) {
    HBRUSH b = CreateSolidBrush(RGB(236, 236, 236));
    FillRect(dc, &r, b);
    DeleteObject(b);
    SelectObject(dc, fontFor(11, false));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(120, 120, 120));
    RECT tr = r;
    DrawTextW(dc, L"Image can't be shown", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  im->drawnPaint = paintCount_;
  RECT dst = r;
  if (o.propInt("fit", 1) == 1 && bw > 0 && bh > 0) {  // keep proportions, centered in the box
    double sc = std::min((double)bw / im->w, (double)bh / im->h);
    int dw = std::max(1, (int)std::lround(im->w * sc)), dh = std::max(1, (int)std::lround(im->h * sc));
    dst.left = r.left + (bw - dw) / 2;
    dst.top = r.top + (bh - dh) / 2;
    dst.right = dst.left + dw;
    dst.bottom = dst.top + dh;
  }
  int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
  if (dw <= 0 || dh <= 0) return;

  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  HDC mem = CreateCompatibleDC(dc);

  // Resample once per size (WIC scaler: correct on premultiplied alpha), then composite 1:1.
  // Skip the cache for huge destinations (high zoom) rather than allocate hundreds of megabytes.
  bool cacheable = dw <= 8192 && dh <= 8192 && (long long)dw * dh <= 40LL * 1000 * 1000;
  if (cacheable && (!im->scaled || im->sw != dw || im->sh != dh || im->sframe != im->cur)) {
    HBITMAP nb = nullptr;
    if (scaleImage(im->src, im->w, im->h, dw, dh, nb)) {
      if (im->scaled) DeleteObject(im->scaled);
      im->scaled = nb;
      im->sw = dw;
      im->sh = dh;
      im->sframe = im->cur;
    }
  }
  if (cacheable && im->scaled && im->sw == dw && im->sh == dh && im->sframe == im->cur) {
    HGDIOBJ old = SelectObject(mem, im->scaled);
    AlphaBlend(dc, dst.left, dst.top, dw, dh, mem, 0, 0, dw, dh, bf);
    SelectObject(mem, old);
  } else {
    HGDIOBJ old = SelectObject(mem, im->src);
    AlphaBlend(dc, dst.left, dst.top, dw, dh, mem, 0, 0, im->w, im->h, bf);
    SelectObject(mem, old);
  }
  DeleteDC(mem);
}

void Canvas::animTick() {
  DWORD now = GetTickCount();
  bool changed = false;
  for (auto& kv : images_) {
    ImgEntry& e = kv.second;
    size_t n = e.frames.size();
    if (n < 2 || e.drawnPaint != paintCount_) continue;  // only animate what is on screen
    size_t guard = 0;
    while ((int)(now - e.next) >= 0 && guard++ < n) {
      e.cur = (int)((e.cur + 1) % n);
      e.next += (DWORD)e.delays[(size_t)e.cur];
      changed = true;
    }
    if ((int)(now - e.next) >= 0) e.next = now + (DWORD)e.delays[(size_t)e.cur];  // fell far behind: resync
    e.src = e.frames[(size_t)e.cur];
  }
  if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Canvas::placeNew(int w, int h, int px, int py, int& x, int& y) {
  const Page* p = curPage();
  if (px >= 0 && py >= 0) {
    POINT pp = toPagePt(px, py);
    x = pp.x;
    y = pp.y;
  } else {
    int margin = MulDiv(28, dpi_, 96);
    int vx = std::max(0, (int)((scrollX_ - margin) / scale()));
    int vy = std::max(0, (int)((scrollY_ - margin) / scale()));
    int stagger = (int)(p->objects.size() % 8) * 24;
    x = vx + 40 + stagger;
    y = vy + 40 + stagger;
  }
  x = std::clamp(snapv(x), 0, std::max(0, p->w - w));
  y = std::clamp(snapv(y), 0, std::max(0, p->h - h));
}

int Canvas::measureTextHeight(const std::string& utf8, int pt, int widthUnits) {
  HDC dc = GetDC(hwnd_);
  HGDIOBJ old = SelectObject(dc, fontFor(pt, false));
  std::wstring w = widen(utf8);
  RECT rc = {0, 0, std::max(10, (int)std::lround((widthUnits - 8) * scale())), 0};
  DrawTextW(dc, w.c_str(), (int)w.size(), &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(hwnd_, dc);
  return std::clamp((int)std::ceil(rc.bottom / scale()) + 10, 28, 4000);
}

int64_t Canvas::addTextObject(const std::string& utf8, int px, int py, const std::string& runsEnc) {
  Object o;
  o.type = ObjType::Text;
  o.w = 480;
  o.h = runsEnc.empty() ? measureTextHeight(utf8, 14, o.w) : measureRichHeight(utf8, runsEnc, 14, o.w);
  o.setProp("text", utf8);
  if (!runsEnc.empty()) o.setProp("runs", runsEnc);
  o.setPropInt("size", 14);
  o.setPropInt("color", 0x222222);
  placeNew(o.w, o.h, px, py, o.x, o.y);
  return doc_->addObject(pageId_, o, "Add Text");
}

namespace {
bool isAudioExtension(const std::wstring& p) {
  size_t dot = p.find_last_of(L'.');
  if (dot == std::wstring::npos) return false;
  std::wstring e = p.substr(dot + 1);
  for (wchar_t& c : e) c = (wchar_t)std::towlower(c);
  return e == L"wav" || e == L"mp3" || e == L"m4a" || e == L"wma" || e == L"mid" || e == L"midi";
}
}  // namespace

bool Canvas::addImageBytes(const std::string& bytes, const std::string& name, int px, int py) {
  if (!curPage() || !doc_) return false;
  Decoded d;
  if (!decodeImage(bytes, d)) return false;  // not an image we can show: store nothing
  if (!edit_) setEditMode(true);
  endEdit(true, false);

  double natural = std::min({1.0, 520.0 / d.w, 400.0 / d.h});  // natural size, capped for the page
  Object o;
  o.type = ObjType::Image;
  o.w = std::max(16, (int)std::lround(d.w * natural));
  o.h = std::max(16, (int)std::lround(d.h * natural));
  placeNew(o.w, o.h, px, py, o.x, o.y);
  int64_t blob = doc_->addBlob(bytes, sniffMime(bytes), name);
  o.setPropInt("blob", (long)blob);
  o.setProp("name", name);
  o.setPropInt("iw", d.w);
  o.setPropInt("ih", d.h);
  o.setPropInt("fit", 1);

  // Reuse the bitmap we just decoded instead of decoding it again on the first paint.
  if (images_.count(blob) || sniffMime(bytes) == "image/gif") {  // GIFs decode on first paint, to animate
    DeleteObject(d.bmp);
  } else {
    ImgEntry e;
    e.src = d.bmp;
    e.w = d.w;
    e.h = d.h;
    images_[blob] = e;
  }
  int64_t id = doc_->addObject(pageId_, o, "Add Image");
  sel_ = {id};
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
  return true;
}

void Canvas::addFilesAt(const std::vector<std::wstring>& files, int px, int py) {
  if (!curPage() || !doc_ || files.empty()) return;
  if (!edit_) setEditMode(true);
  endEdit(true, false);
  int step = MulDiv(24, dpi_, 96);
  std::vector<int64_t> made;
  std::wstring failed;
  int i = 0;
  for (const std::wstring& f : files) {
    int ox = px >= 0 ? px + i * step : -1, oy = py >= 0 ? py + i * step : -1;
    ++i;
    DWORD attr = GetFileAttributesW(f.c_str());
    bool isDir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    std::string bytes;
    if (!isDir && isImageExtension(f)) {
      if (readWholeFile(f, bytes, 256u * 1024 * 1024) && addImageBytes(bytes, narrow(fileNameOf(f)), ox, oy)) {
        made.push_back(sel_[0]);
      } else {
        failed += L"\n" + fileNameOf(f);
      }
      continue;
    }
    if (!isDir && isAudioExtension(f)) {
      if (readWholeFile(f, bytes, 128u * 1024 * 1024) && addAudioBytes(bytes, narrow(fileNameOf(f)), ox, oy)) {
        made.push_back(sel_[0]);
      } else {
        failed += L"\n" + fileNameOf(f);
      }
      continue;
    }
    if (!isDir && isTextExtension(f) && readWholeFile(f, bytes, 512 * 1024)) {
      made.push_back(addTextObject(textFromBytes(bytes), ox, oy));
      continue;
    }
    // Anything else becomes a link button that opens the file where it lives.
    Object b;
    b.type = ObjType::Button;
    b.w = 240;
    b.h = 40;
    b.setProp("label", narrow(fileNameOf(f)));
    b.setProp("open", narrow(f));
    b.setPropInt("size", 13);
    placeNew(b.w, b.h, ox, oy, b.x, b.y);
    made.push_back(doc_->addObject(pageId_, b, "Add File Link"));
  }
  sel_ = made;
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
  if (!failed.empty())
    MessageBoxW(GetParent(hwnd_), (L"These files couldn't be read, so they were skipped:" + failed).c_str(),
                L"Cairn", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- audio
// Clips live in the file as blobs. To play one it is written to a per-run temp folder and opened through
// MCI, which uses whatever codecs Windows has for that file type. One clip plays at a time.

namespace {
const wchar_t* kAlias = L"cairnaudio";
}

bool Canvas::audioStart(const Object& o) {
  if (!doc_) return false;
  std::string data;
  AudioKind ak;
  if (!doc_->blob(o.propInt("blob", 0), data) || !sniffAudio(data, ak)) return false;
  audioStop();
  if (audioDir_.empty()) {
    wchar_t tmp[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH) return false;
    audioDir_ = std::wstring(tmp, n) + L"cairn-audio-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(audioDir_.c_str(), nullptr);
  }
  std::wstring path = audioDir_ + L"\\b" + std::to_wstring(o.propInt("blob", 0)) + L"." + widen(ak.ext);
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(f);
    if (!ok || wrote != data.size()) {
      DeleteFileW(path.c_str());
      return false;
    }
  }
  std::wstring open = L"open \"" + path + L"\" alias " + kAlias;
  if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0) return false;
  std::wstring play = std::wstring(L"play ") + kAlias + L" notify";
  if (mciSendStringW(play.c_str(), nullptr, 0, hwnd_) != 0) {
    mciSendStringW((std::wstring(L"close ") + kAlias).c_str(), nullptr, 0, nullptr);
    return false;
  }
  audioObj_ = o.id;
  audioLoop_ = o.propInt("loop", 0) != 0;
  audioTick_ = GetTickCount();
  return true;
}

void Canvas::audioStop() {
  if (!audioObj_) return;
  audioObj_ = 0;  // first: the "aborted" notification this causes must find nothing to do
  mciSendStringW((std::wstring(L"stop ") + kAlias).c_str(), nullptr, 0, nullptr);
  mciSendStringW((std::wstring(L"close ") + kAlias).c_str(), nullptr, 0, nullptr);
  for (Canvas* c : all_) if (c->hwnd_) InvalidateRect(c->hwnd_, nullptr, FALSE);
}

void Canvas::audioNotify(WPARAM w) {
  if (!audioObj_) return;
  if (w == MCI_NOTIFY_SUCCESSFUL && audioLoop_ && GetTickCount() - audioTick_ > 300) {
    audioTick_ = GetTickCount();
    mciSendStringW((std::wstring(L"seek ") + kAlias + L" to start").c_str(), nullptr, 0, nullptr);
    if (mciSendStringW((std::wstring(L"play ") + kAlias + L" notify").c_str(), nullptr, 0, hwnd_) == 0) return;
  }
  if (w == MCI_NOTIFY_SUPERSEDED) return;
  audioStop();  // finished, failed, or aborted
}

void Canvas::audioAutoplay() {
  if (edit_ || !doc_) return;
  const Page* p = curPage();
  if (!p) return;
  for (const Object& o : p->objects)
    if (o.type == ObjType::Audio && o.propInt("auto", 0)) {
      audioStart(o);  // a clip that cannot play is just silent here; clicking it explains
      return;
    }
}

void Canvas::drawAudio(HDC dc, const Object& o, const RECT& r) {
  bool playing = audioObj_ == o.id;
  HBRUSH bg = CreateSolidBrush(playing ? RGB(226, 238, 255) : RGB(240, 242, 246));
  FillRect(dc, &r, bg);
  DeleteObject(bg);
  FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
  int h = r.bottom - r.top, pad = MulDiv(8, dpi_, 96);
  int d = std::max(12, h - 2 * pad);
  RECT ic{r.left + pad, r.top + (h - d) / 2, r.left + pad + d, r.top + (h - d) / 2 + d};
  HBRUSH accent = CreateSolidBrush(RGB(0x2F, 0x6F, 0xDE));
  HGDIOBJ ob = SelectObject(dc, accent), op = SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, ic.left, ic.top, ic.right, ic.bottom);
  HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
  SelectObject(dc, white);
  int cx = (ic.left + ic.right) / 2, cy = (ic.top + ic.bottom) / 2, q = d / 4;
  if (playing) {
    Rectangle(dc, cx - q, cy - q, cx + q + 1, cy + q + 1);  // stop square
  } else {
    POINT tri[3] = {{cx - q / 2, cy - q}, {cx - q / 2, cy + q}, {cx + q, cy}};
    Polygon(dc, tri, 3);  // play triangle
  }
  SelectObject(dc, op);
  SelectObject(dc, ob);
  DeleteObject(accent);
  DeleteObject(white);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(30, 30, 30));
  HGDIOBJ of = SelectObject(dc, fontFor(13, false));
  std::wstring label = widen(o.prop("name", "audio"));
  if (o.propInt("loop", 0)) label += L"  (loop)";
  RECT tr{ic.right + pad, r.top, r.right - pad, r.bottom};
  DrawTextW(dc, label.c_str(), (int)label.size(), &tr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(dc, of);
}

bool Canvas::addAudioBytes(const std::string& bytes, const std::string& name, int px, int py) {
  AudioKind ak;
  if (!curPage() || !doc_ || !sniffAudio(bytes, ak)) return false;
  if (!edit_) setEditMode(true);
  endEdit(true, false);
  Object o;
  o.type = ObjType::Audio;
  o.w = 280;
  o.h = 48;
  placeNew(o.w, o.h, px, py, o.x, o.y);
  o.setProp("name", name);
  o.setProp("ext", ak.ext);
  o.setPropInt("blob", (long)doc_->addBlob(bytes, ak.mime, name));
  int64_t id = doc_->addObject(pageId_, o, "Add Audio");
  sel_ = {id};
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
  return true;
}

// --------------------------------------------------------------- clipboard

namespace {

UINT fmtObjects() {
  static UINT f = RegisterClipboardFormatW(L"CairnObjects");
  return f;
}
UINT fmtPng() {
  static UINT f = RegisterClipboardFormatW(L"PNG");
  return f;
}

bool putClip(UINT fmt, const void* data, size_t bytes) {
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!g) return false;
  void* p = GlobalLock(g);
  if (!p) {
    GlobalFree(g);
    return false;
  }
  std::memcpy(p, data, bytes);
  GlobalUnlock(g);
  if (!SetClipboardData(fmt, g)) {
    GlobalFree(g);
    return false;
  }
  return true;
}

bool getClip(UINT fmt, std::string& out) {
  if (!IsClipboardFormatAvailable(fmt)) return false;
  HANDLE h = GetClipboardData(fmt);
  if (!h) return false;
  size_t n = GlobalSize(h);
  const void* p = GlobalLock(h);
  if (!p) return false;
  out.assign((const char*)p, n);
  GlobalUnlock(h);
  return true;
}

// CF_DIB is a bitmap without a file header; wrap it so WIC can decode it as a .bmp.
std::string dibToBmp(const std::string& dib) {
  if (dib.size() < sizeof(BITMAPINFOHEADER)) return {};
  const BITMAPINFOHEADER* bi = (const BITMAPINFOHEADER*)dib.data();
  if (bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biSize > dib.size()) return {};
  DWORD colors = bi->biClrUsed;
  if (colors == 0 && bi->biBitCount <= 8) colors = 1u << bi->biBitCount;
  DWORD masks = (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
  size_t off = 14 + (size_t)bi->biSize + masks + (size_t)colors * 4;
  if (off > dib.size() + 14) return {};
  std::string bmp(14, '\0');
  bmp[0] = 'B';
  bmp[1] = 'M';
  DWORD total = (DWORD)(dib.size() + 14), offd = (DWORD)off;
  std::memcpy(&bmp[2], &total, 4);
  std::memcpy(&bmp[10], &offd, 4);
  return bmp + dib;
}

}  // namespace

void Canvas::copySelection(bool cut) {
  const Page* p = curPage();
  if (!edit_ || !p || sel_.empty() || !doc_) return;
  endEdit(true, false);
  std::vector<int64_t> ids;  // page order, so pasted objects keep their stacking
  std::wstring text;
  std::string html;  // styled text objects, for other apps
  bool anyStyled = false;
  const Object* single = nullptr;
  for (const Object& o : p->objects) {
    if (!isSelected(o.id)) continue;
    ids.push_back(o.id);
    if (o.type == ObjType::Text && !o.prop("text").empty()) {
      std::string t = o.prop("text"), rn = o.prop("runs");
      anyStyled = anyStyled || !rn.empty();
      html += (html.empty() ? "" : "<br><br>") + richToHtml(t, decodeRuns(rn, t.size()));
    }
    single = &o;
    std::string t = o.type == ObjType::Text ? o.prop("text")
                  : o.type == ObjType::Button ? o.prop("label")
                  : o.type == ObjType::Image ? o.prop("name") : "";
    if (!t.empty()) text += (text.empty() ? L"" : L"\r\n") + toEditText(t);
  }
  if (ids.empty()) return;
  std::string payload = doc_->exportObjects(ids);

  if (!OpenClipboard(hwnd_)) return;
  EmptyClipboard();
  bool ok = putClip(fmtObjects(), payload.data(), payload.size());
  if (!text.empty()) putClip(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  if (anyStyled) {
    std::string clip = wrapClipboardHtml(html);
    putClip(RegisterClipboardFormatW(L"HTML Format"), clip.c_str(), clip.size() + 1);
  }
  if (ids.size() == 1 && single && single->type == ObjType::Image) {  // also let other apps paste the picture
    std::string data, mime;
    if (doc_->blob(single->propInt("blob", 0), data, &mime) && mime == "image/png")
      putClip(fmtPng(), data.data(), data.size());
  }
  CloseClipboard();
  if (!ok) {
    MessageBoxW(GetParent(hwnd_), L"There wasn't enough memory to put that on the clipboard.", L"Cairn",
                MB_OK | MB_ICONWARNING);
    return;
  }
  clipSeq_ = GetClipboardSequenceNumber();
  pasteCount_ = 0;
  cutPending_ = cut;
  if (cut) deleteSelection();
}

void Canvas::paste(bool plainOnly, int px, int py) {
  if (!curPage() || !doc_) return;
  if (!edit_) setEditMode(true);
  endEdit(true, false);

  std::string objs, png, dib;
  std::vector<std::wstring> files;
  std::wstring text;
  RichText rich;
  if (!OpenClipboard(hwnd_)) return;
  getClip(fmtObjects(), objs);
  if (objs.empty()) {
    if (!getClip(fmtPng(), png)) getClip(CF_DIB, dib);
    if (png.empty() && dib.empty()) {
      if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (HDROP hd = (HDROP)GetClipboardData(CF_HDROP)) {
          UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
          for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring f((size_t)len + 1, L'\0');
            DragQueryFileW(hd, i, &f[0], len + 1);
            f.resize(len);
            files.push_back(f);
          }
        }
      }
      if (files.empty() && !plainOnly) {  // text copied from a web page keeps its formatting
        std::string frag, src;
        if (readHtmlClip(hwnd_, true, frag, src)) rich = htmlToRich(frag, src);
      }
      if (files.empty() && rich.text.empty() && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
          if (const wchar_t* t = (const wchar_t*)GlobalLock(h)) {
            text = t;
            GlobalUnlock(h);
          }
        }
      }
    }
  }
  DWORD seq = GetClipboardSequenceNumber();
  CloseClipboard();

  if (!objs.empty()) {
    if (seq != clipSeq_) {  // someone else (or another Cairn) filled the clipboard
      pasteCount_ = 0;
      cutPending_ = false;
    }
    int off = 24 * (pasteCount_ + (cutPending_ ? 0 : 1));
    ++pasteCount_;
    auto ids = doc_->importObjects(pageId_, objs, off, off, "Paste");
    if (ids.empty()) {
      MessageBoxW(GetParent(hwnd_), L"The clipboard holds Cairn objects, but they're damaged and can't be pasted.",
                  L"Cairn", MB_OK | MB_ICONWARNING);
      return;
    }
    sel_ = ids;
  } else if (!png.empty() || !dib.empty()) {
    std::string bytes = !png.empty() ? png : dibToBmp(dib);
    if (bytes.empty() || !addImageBytes(bytes, "Pasted image", px, py)) {
      MessageBoxW(GetParent(hwnd_), L"That picture is in a format Cairn can't read.", L"Cairn",
                  MB_OK | MB_ICONINFORMATION);
      return;
    }
    return;  // addImageBytes already selected and repainted
  } else if (!files.empty()) {
    addFilesAt(files, px, py);
    return;
  } else if (!rich.text.empty()) {
    sel_ = {addTextObject(rich.text, px, py, encodeRuns(rich.runs))};
  } else if (!text.empty()) {
    int64_t id = addTextObject(fromEditText(text), px, py);
    sel_ = {id};
  } else {
    return;  // nothing we understand
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
  notify();
}

}  // namespace cairn

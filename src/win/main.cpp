// Cairn: application shell (window, menus, navigator, status bar, settings).
#include "util.h"  // must come first: it pulls in windows.h, which the headers below rely on

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "document.h"
#include "export.h"
#include "markdown.h"
#include "propform.h"
#include "rich.h"

using namespace cairn;

namespace {

constexpr const wchar_t* kAppClass = L"CairnMain";
constexpr const wchar_t* kAppName = L"Cairn";
constexpr const char* kVersion = "0.18.0";

constexpr int kTagBase = 5000;  // popup items for "filter by tag": kTagBase = show all, +1.. = tags

enum Ids {
  IDC_SEARCH = 201, IDC_TOOLBAR, IDC_PAGES, IDC_BACKLINKS, IDC_CANVAS, IDC_STATUS, IDC_LINKLABEL,

  IDM_FILE_NEW = 100, IDM_FILE_OPEN, IDM_FILE_SAVECOPY, IDM_FILE_EXPORT, IDM_FILE_EXPORTMD, IDM_FILE_IMPORTMD, IDM_FILE_EXIT,
  IDM_EDIT_UNDO, IDM_EDIT_REDO, IDM_EDIT_ADDTEXT, IDM_EDIT_ADDBUTTON, IDM_EDIT_ADDRECT, IDM_EDIT_ADDFOLDER, IDM_EDIT_ADDTABLE, IDM_EDIT_ADDAUDIO, IDM_TBL_ROW_ABOVE, IDM_TBL_ROW_BELOW, IDM_TBL_ROW_DEL, IDM_TBL_COL_LEFT, IDM_TBL_COL_RIGHT, IDM_TBL_COL_DEL,
  IDM_EDIT_EDITTEXT, IDM_EDIT_REPLACE, IDM_EDIT_PROPS, IDM_EDIT_DUPLICATE, IDM_EDIT_DELETE, IDM_EDIT_SELECTALL,
  IDM_EDIT_FRONT, IDM_EDIT_BACK,
  IDM_PAGE_NEW, IDM_PAGE_RENAME, IDM_PAGE_DELETE, IDM_PAGE_PROPS, IDM_PAGE_BACK, IDM_PAGE_FORWARD,
  IDM_PAGE_FIND, IDM_PAGE_MOVEUP, IDM_PAGE_MOVEDOWN,
  IDM_FMT_BOLD, IDM_FMT_ITALIC, IDM_FMT_UNDERLINE, IDM_FMT_STRIKE, IDM_FMT_LINK, IDM_FMT_COLOR,
  IDM_FMT_CODE, IDM_FMT_CLEAR, IDM_EDIT_PASTETEXT,
  IDM_VIEW_EDITMODE, IDM_VIEW_SNAP, IDM_VIEW_ZOOMIN, IDM_VIEW_ZOOMOUT, IDM_VIEW_ZOOM100,
  IDM_EDIT_CUT, IDM_EDIT_COPY, IDM_EDIT_PASTE, IDM_EDIT_ADDIMAGE, IDM_VIEW_PALETTE,
  IDM_CTX_OPENLINK, IDM_CTX_COPYLINK, IDM_CTX_ADDTEXT, IDM_CTX_ADDBUTTON, IDM_CTX_ADDRECT, IDM_CTX_ADDIMAGE,
  IDM_CTX_PASTE, IDM_CTX_PASTETEXT, IDM_PAGE_TAGS,
  IDM_FMT_BIGGER, IDM_FMT_SMALLER, IDM_VIEW_FKEYS,
  IDM_HELP_ABOUT,
};

struct App {
  HINSTANCE hi = nullptr;
  HWND hwnd = nullptr, search = nullptr, pageList = nullptr, linkLabel = nullptr;
  HWND backList = nullptr, status = nullptr, toolbar = nullptr;
  HIMAGELIST tbIcons = nullptr;
  int tbH = 0;
  HWND fkeys = nullptr;  // the F1-F10 bar above the status bar
  bool fkShow = true;
  int fkH = 0;
  HMENU menu = nullptr;
  HACCEL accel = nullptr;
  HFONT font = nullptr;
  int dpi = 96;

  Canvas canvas;
  std::unique_ptr<Document> doc;
  std::vector<int64_t> listIds, backIds;
  std::vector<int64_t> hist;
  int hpos = -1;
  bool inRefresh = false;

  int splitX = 250;  // left pane width at 96 dpi
  bool dragSplit = false;

  std::wstring iniPath;
  bool startEdit = false;
  bool startSnap = true;
};

App g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

void showError(const std::wstring& msg) {
  MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

std::wstring baseName(const std::wstring& path) {
  size_t i = path.find_last_of(L"\\/");
  return i == std::wstring::npos ? path : path.substr(i + 1);
}

std::wstring knownFolder(int csidl) {
  wchar_t buf[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, 0, buf))) return buf;
  return L"";
}

// ------------------------------------------------------------------ settings

void setupIniPath() {
  std::wstring dir = knownFolder(CSIDL_APPDATA);
  if (dir.empty()) return;
  dir += L"\\Cairn";
  CreateDirectoryW(dir.c_str(), nullptr);
  g.iniPath = dir + L"\\cairn.ini";
}

std::wstring readIni(const wchar_t* key, const wchar_t* def = L"") {
  if (g.iniPath.empty()) return def;
  wchar_t buf[2048] = {};
  GetPrivateProfileStringW(L"app", key, def, buf, 2047, g.iniPath.c_str());
  return buf;
}
int readIniInt(const wchar_t* key, int def) {
  return g.iniPath.empty() ? def : (int)GetPrivateProfileIntW(L"app", key, def, g.iniPath.c_str());
}
void writeIni(const wchar_t* key, const std::wstring& v) {
  if (!g.iniPath.empty()) WritePrivateProfileStringW(L"app", key, v.c_str(), g.iniPath.c_str());
}
void writeIniInt(const wchar_t* key, int v) { writeIni(key, std::to_wstring(v)); }

void saveSettings() {
  if (!g.hwnd) return;
  WINDOWPLACEMENT wp{};
  wp.length = sizeof wp;
  GetWindowPlacement(g.hwnd, &wp);
  writeIniInt(L"x", wp.rcNormalPosition.left);
  writeIniInt(L"y", wp.rcNormalPosition.top);
  writeIniInt(L"w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
  writeIniInt(L"h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
  writeIniInt(L"max", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
  writeIniInt(L"split", g.splitX);
  writeIniInt(L"snap", g.canvas.snap() ? 1 : 0);
  writeIniInt(L"edit", g.canvas.editMode() ? 1 : 0);
}

// ------------------------------------------------------------------- fonts

void rebuildFont() {
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof ncm;
  HFONT nf = nullptr;
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)g.dpi))
    nf = CreateFontIndirectW(&ncm.lfMessageFont);
  if (!nf) nf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  HFONT old = g.font;
  g.font = nf;
  for (HWND h : {g.search, g.pageList, g.linkLabel, g.backList, g.status, g.toolbar})
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)nf, TRUE);
  PropForm::setFont(nf);
  if (old && old != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(old);
}

// ------------------------------------------------------------- list refresh

void updateTitle() {
  std::wstring t;
  if (g.doc) {
    if (const Page* p = g.doc->page(g.canvas.pageId())) t += widen(p->title) + L" — ";
    t += baseName(widen(g.doc->path())) + L" — ";
  }
  t += kAppName;
  SetWindowTextW(g.hwnd, t.c_str());
}

void syncToolbar();
void createToolbar();
void canvasContextMenu(POINT screen);
const wchar_t* tooltipFor(int id);

void updateStatus() {
  if (!g.status) return;
  const wchar_t* mode = g.canvas.editMode() ? L"  EDIT MODE" : L"  RUN MODE";
  SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)mode);
  std::wstring sel = g.canvas.selectionSummary();
  if (sel.empty() && !g.canvas.editMode()) sel = L"Ctrl+E to edit this page";
  SendMessageW(g.status, SB_SETTEXTW, 1, (LPARAM)(L"  " + sel).c_str());
  SendMessageW(g.status, SB_SETTEXTW, 2, (LPARAM)(g.canvas.snap() ? L"  Snap on" : L"  Snap off"));
  std::wstring pg;
  if (g.doc)
    if (const Page* p = g.doc->page(g.canvas.pageId()))
      pg = std::to_wstring(p->w) + L" × " + std::to_wstring(p->h) + L"   ";
  pg += std::to_wstring((int)std::lround(g.canvas.zoom() * 100)) + L"%";
  SendMessageW(g.status, SB_SETTEXTW, 3, (LPARAM)(L"  " + pg).c_str());
  syncToolbar();
}

int currentPageIndex();
void cmdMovePage(int delta);

void refreshPageList() {
  if (!g.doc) return;
  std::wstring q;
  int n = GetWindowTextLengthW(g.search);
  q.resize((size_t)n + 1);
  GetWindowTextW(g.search, &q[0], n + 1);
  q.resize((size_t)n);

  g.listIds = g.doc->search(narrow(q));
  g.inRefresh = true;
  int top = (int)SendMessageW(g.pageList, LB_GETTOPINDEX, 0, 0);
  SendMessageW(g.pageList, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g.pageList, LB_RESETCONTENT, 0, 0);
  int sel = -1;
  for (size_t i = 0; i < g.listIds.size(); ++i) {
    const Page* p = g.doc->page(g.listIds[i]);
    std::wstring label = widen(p ? p->title : "?");
    if (p && !p->tags.empty())
      for (const std::string& t : cairn::splitTags(p->tags)) label += L"  #" + widen(t);
    SendMessageW(g.pageList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
    if (g.listIds[i] == g.canvas.pageId()) sel = (int)i;
  }
  SendMessageW(g.pageList, LB_SETCURSEL, (WPARAM)sel, 0);
  SendMessageW(g.pageList, LB_SETTOPINDEX, (WPARAM)std::max(0, top), 0);
  SendMessageW(g.pageList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g.pageList, nullptr, TRUE);
  g.inRefresh = false;
}

void refreshBacklinks() {
  if (!g.doc) return;
  auto bl = g.doc->backlinks(g.canvas.pageId());
  g.backIds.clear();
  g.inRefresh = true;
  SendMessageW(g.backList, LB_RESETCONTENT, 0, 0);
  for (auto& b : bl) {
    g.backIds.push_back(b.first);
    SendMessageW(g.backList, LB_ADDSTRING, 0, (LPARAM)widen(b.second).c_str());
  }
  g.inRefresh = false;
}

void navigate(int64_t id, bool push) {
  if (!g.doc || !g.doc->page(id)) return;
  if (push && !(g.hpos >= 0 && g.hist[(size_t)g.hpos] == id)) {
    g.hist.resize((size_t)g.hpos + 1);
    g.hist.push_back(id);
    g.hpos = (int)g.hist.size() - 1;
  }
  g.canvas.setPage(id);
  refreshPageList();
  refreshBacklinks();
  updateTitle();
  updateStatus();
}

void historyStep(int dir) {
  int i = g.hpos + dir;
  while (i >= 0 && i < (int)g.hist.size() && !g.doc->page(g.hist[(size_t)i])) i += dir;  // skip deleted pages
  if (i < 0 || i >= (int)g.hist.size()) return;
  g.hpos = i;
  navigate(g.hist[(size_t)i], false);
}

void onDocChanged() {
  if (!g.doc) return;
  if (!g.doc->page(g.canvas.pageId()) && !g.doc->pages().empty()) {
    navigate(g.doc->pages().front().id, false);  // current page vanished (delete or undo)
  } else {
    refreshPageList();
    refreshBacklinks();
  }
  g.canvas.docChanged();
  updateTitle();
  updateStatus();
}

// -------------------------------------------------------------------- layout

int leftWidth(const RECT& rc) {
  return std::clamp(S(g.splitX), S(120), std::max(S(120), (int)rc.right - S(240)));
}

void layout() {
  if (!g.hwnd || !g.status) return;
  RECT rc;
  GetClientRect(g.hwnd, &rc);

  SendMessageW(g.status, WM_SIZE, 0, 0);
  RECT sr;
  GetWindowRect(g.status, &sr);
  int statusH = sr.bottom - sr.top;
  int w3 = S(190), w2 = S(90), w0 = S(120);
  int rightEdge = rc.right;
  int parts[4] = {w0, std::max(w0 + S(100), rightEdge - w3 - w2), rightEdge - w3, -1};
  SendMessageW(g.status, SB_SETPARTS, 4, (LPARAM)parts);

  g.fkH = g.fkShow ? S(24) : 0;
  if (g.fkeys) {
    MoveWindow(g.fkeys, 0, rc.bottom - statusH - g.fkH, rc.right, g.fkH, TRUE);
    ShowWindow(g.fkeys, g.fkShow ? SW_SHOW : SW_HIDE);
  }
  int bottom = rc.bottom - statusH - g.fkH;
  int leftW = leftWidth(rc), splitW = S(5);
  int pad = S(4);

  if (g.toolbar) {  // buttons wrap onto more rows when the window is narrow
    MoveWindow(g.toolbar, 0, 0, rc.right, S(300), TRUE);
    int n = (int)SendMessageW(g.toolbar, TB_BUTTONCOUNT, 0, 0);
    RECT last{};
    if (n > 0 && SendMessageW(g.toolbar, TB_GETITEMRECT, (WPARAM)(n - 1), (LPARAM)&last))
      g.tbH = last.bottom + S(3);
    MoveWindow(g.toolbar, 0, 0, rc.right, g.tbH, TRUE);
  }
  int top = g.tbH;

  int y = top + pad;
  MoveWindow(g.search, pad, y, leftW - 2 * pad, S(24), TRUE);
  y += S(24) + pad;
  int labelH = S(18);
  int backH = std::clamp((bottom - y) / 4, S(60), S(200));
  int pagesBottom = bottom - backH - labelH - 2 * pad;
  MoveWindow(g.pageList, pad, y, leftW - 2 * pad, std::max(S(40), pagesBottom - y), TRUE);
  MoveWindow(g.linkLabel, pad + S(2), pagesBottom + pad, leftW - 2 * pad, labelH, TRUE);
  int by = pagesBottom + pad + labelH;
  MoveWindow(g.backList, pad, by, leftW - 2 * pad, std::max(S(30), bottom - by - pad), TRUE);

  int cx = leftW + splitW;
  MoveWindow(g.canvas.hwnd(), cx, top, std::max(10, (int)rc.right - cx), std::max(10, bottom - top), TRUE);
}

// ---------------------------------------------------------------- documents

std::wstring uniquePath(const std::wstring& dir, const std::wstring& stem) {
  std::wstring p = dir + L"\\" + stem + L".cairn";
  for (int i = 2; std::filesystem::exists(std::filesystem::path(p)) && i < 1000; ++i)
    p = dir + L"\\" + stem + L" " + std::to_wstring(i) + L".cairn";
  return p;
}

void setDoc(std::unique_ptr<Document> d) {
  g.canvas.setDocument(nullptr);  // never leave the canvas pointing at a dying document
  g.doc = std::move(d);
  g.hist.clear();
  g.hpos = -1;
  SetWindowTextW(g.search, L"");
  if (!g.doc) return;
  g.doc->onChange = [] { onDocChanged(); };
  g.canvas.setDocument(g.doc.get());
  writeIni(L"last", widen(g.doc->path()));
  if (!g.doc->pages().empty()) navigate(g.doc->pages().front().id, true);
  else {
    refreshPageList();
    refreshBacklinks();
    updateTitle();
    updateStatus();
  }
}

bool openDoc(const std::wstring& path, bool create, bool quiet) {
  try {
    setDoc(create ? Document::create(narrow(path), true) : Document::open(narrow(path)));
    return true;
  } catch (const DbError& e) {
    if (!quiet) showError(widen(e.what()));
    return false;
  }
}

bool pickFile(bool save, std::wstring& out) {
  wchar_t buf[1024] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = g.hwnd;
  ofn.lpstrFilter = L"Cairn files (*.cairn)\0*.cairn\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 1023;
  ofn.lpstrDefExt = L"cairn";
  std::wstring dir = knownFolder(CSIDL_PERSONAL) + L"\\Cairn";
  ofn.lpstrInitialDir = dir.c_str();
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT * 0 : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
  out = buf;
  return true;
}

void cmdNew() {
  std::wstring p;
  if (!pickFile(true, p)) return;
  if (std::filesystem::exists(std::filesystem::path(p))) {
    showError(L"That file already exists. Use File > Open to open it, or pick a new name.");
    return;
  }
  openDoc(p, true, false);
}

void cmdOpen() {
  std::wstring p;
  if (pickFile(false, p)) openDoc(p, false, false);
}

void cmdSaveCopy() {
  if (!g.doc) return;
  std::wstring p;
  if (!pickFile(true, p)) return;
  if (std::filesystem::exists(std::filesystem::path(p))) {
    showError(L"That file already exists. Pick a new name; Cairn never overwrites on its own.");
    return;
  }
  g.doc->saveCopy(narrow(p));
}

bool pickFolder(std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Choose where to put the exported website");
  bool ok = false;
  if (SUCCEEDED(dlg->Show(g.hwnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it))) {
      PWSTR name = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
        out = name;
        CoTaskMemFree(name);
        ok = true;
      }
      it->Release();
    }
  }
  dlg->Release();
  return ok;
}

// Writes the document as a folder of static HTML pages into a NEW subfolder of the chosen
// folder (never into an existing one), then offers to open it.
void cmdExportSite() {
  if (!g.doc) return;
  std::wstring parent;
  if (!pickFolder(parent)) return;
  namespace fs = std::filesystem;
  std::wstring stem = fs::path(widen(g.doc->path())).stem().wstring();
  if (stem.empty()) stem = L"Cairn";
  fs::path dir;
  try {
    for (int i = 1; i < 1000; ++i) {
      dir = fs::path(parent) / (stem + L" (website)" + (i > 1 ? L" " + std::to_wstring(i) : L""));
      if (!fs::exists(dir)) break;
    }
    auto files = exportSite(*g.doc, narrow(stem));
    fs::create_directories(dir / L"images");
    for (const ExportFile& f : files) {
      std::ofstream out(dir / fs::path(widen(f.path)), std::ios::binary);
      if (!out) throw std::runtime_error("couldn't write " + f.path);
      out.write(f.data.data(), (std::streamsize)f.data.size());
      if (!out) throw std::runtime_error("couldn't write " + f.path);
    }
    std::wstring msg = L"Exported " + std::to_wstring(g.doc->pages().size()) + L" page(s) to:\n" + dir.wstring() +
                       L"\n\nButtons that open local files or folders are not included. Open it now?";
    if (MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES)
      ShellExecuteW(g.hwnd, L"open", (dir / L"index.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } catch (const std::exception& e) {
    showError(widen(std::string("The export didn't finish: ") + e.what()));
  }
}

bool pickMarkdown(bool save, std::wstring& out) {
  wchar_t buf[1024] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = g.hwnd;
  ofn.lpstrFilter = L"Markdown (*.md)\0*.md;*.markdown;*.txt\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 1023;
  ofn.lpstrDefExt = L"md";
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? 0 : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
  out = buf;
  return true;
}

// Text-only Markdown: one "# heading" per page. Never overwrites an existing file.
void cmdExportMarkdown() {
  if (!g.doc) return;
  std::wstring p;
  if (!pickMarkdown(true, p)) return;
  namespace fs = std::filesystem;
  if (fs::exists(fs::path(p))) {
    showError(L"That file already exists. Pick a new name; Cairn never overwrites on its own.");
    return;
  }
  std::string md = exportMarkdown(*g.doc);
  std::ofstream out(fs::path(p), std::ios::binary);
  out.write(md.data(), (std::streamsize)md.size());
  if (!out) {
    showError(L"Couldn't write that file.");
    return;
  }
  MessageBoxW(g.hwnd, L"Exported the text of every page.\n\nImages and rectangles are not included.", kAppName,
              MB_OK | MB_ICONINFORMATION);
}

// Each "# heading" becomes a page; everything lands as one undo step per page.
void cmdImportMarkdown() {
  if (!g.doc) return;
  std::wstring p;
  if (!pickMarkdown(false, p)) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  auto sz = fs::file_size(fs::path(p), ec);
  if (ec || sz > (2u << 20)) {
    showError(L"That file is too large to import (the limit is 2 MB).");
    return;
  }
  std::ifstream in(fs::path(p), std::ios::binary);
  std::string md((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (md.compare(0, 3, "\xEF\xBB\xBF") == 0) md.erase(0, 3);
  MdImportResult r = importMarkdown(*g.doc, md, narrow(fs::path(p).stem().wstring()));
  if (!r.pages) {
    showError(L"Nothing to import: the file has no text.");
    return;
  }
  navigate(r.firstPage, true);
}

// Find & replace over the text of text objects and button labels. Always shows what it is about
// to do (a dry run) and asks before changing anything; the whole replace is one undo step.
void cmdReplace() {
  if (!g.doc) return;
  static std::wstring lastFind, lastRepl;
  static bool lastCase = false;
  static int lastScope = 1;
  PropForm f(g.hwnd, L"Find and Replace");
  int ifind = f.addText(L"Find", lastFind);
  int irepl = f.addText(L"Replace with", lastRepl);
  int iscope = f.addCombo(L"Where", {L"This page", L"All pages"}, lastScope);
  int icase = f.addCheck(L"Match case", lastCase);
  if (!f.run()) return;
  lastFind = f.text(ifind);
  lastRepl = f.text(irepl);
  lastScope = f.combo(iscope);
  lastCase = f.check(icase);
  if (lastFind.empty()) return;
  int64_t only = lastScope == 0 ? g.canvas.pageId() : 0;
  std::string fd = narrow(lastFind), rp = narrow(lastRepl);
  auto dry = g.doc->replaceAll(fd, rp, lastCase, only, true);
  if (dry.matches == 0) {
    MessageBoxW(g.hwnd, L"No matches found.", kAppName, MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring msg = L"Replace " + std::to_wstring(dry.matches) + (dry.matches == 1 ? L" match" : L" matches") +
                     L" in " + std::to_wstring(dry.objects) + (dry.objects == 1 ? L" object" : L" objects") +
                     L" on " + std::to_wstring(dry.pages) + (dry.pages == 1 ? L" page" : L" pages") +
                     L"?\n\nThis is one undo step (Ctrl+Z).";
  if (MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  g.doc->replaceAll(fd, rp, lastCase, only, false);
}

// -------------------------------------------------------------- page commands

bool promptText(const wchar_t* title, const wchar_t* label, std::wstring& v) {
  PropForm f(g.hwnd, title);
  int i = f.addText(label, v);
  if (!f.run()) return false;
  v = f.text(i);
  return true;
}

void cmdNewPage() {
  if (!g.doc) return;
  std::wstring t = L"New page";
  if (!promptText(L"New Page", L"Title", t)) return;
  int64_t id = g.doc->addPage(narrow(t));
  navigate(id, true);
}

void cmdRenamePage() {
  const Page* p = g.doc ? g.doc->page(g.canvas.pageId()) : nullptr;
  if (!p) return;
  std::wstring t = widen(p->title);
  if (promptText(L"Rename Page", L"Title", t)) g.doc->renamePage(p->id, narrow(t));
}

void cmdDeletePage() {
  const Page* p = g.doc ? g.doc->page(g.canvas.pageId()) : nullptr;
  if (!p) return;
  if (g.doc->pages().size() <= 1) {
    showError(L"A Cairn needs at least one page.");
    return;
  }
  if (!p->objects.empty()) {
    std::wstring m = L"Delete “" + widen(p->title) + L"” and the " +
                     std::to_wstring(p->objects.size()) + L" object(s) on it?\n\nYou can undo this with Ctrl+Z.";
    if (MessageBoxW(g.hwnd, m.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  }
  g.doc->deletePage(p->id);
}

void cmdAbout() {
  std::wstring m = std::wstring(kAppName) + L" " + widen(kVersion) +
                   L"\nA local-first multimedia knowledge environment.\n\nSQLite " +
                   widen(sqlite3_libversion()) +
                   L"\nEverything lives in one .cairn file. There is no Save step: every change is "
                   L"written immediately.";
  MessageBoxW(g.hwnd, m.c_str(), L"About Cairn", MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------------- commands

// ------------------------------------------------------------- links & media

// Opens a web address, an email link, or a file/folder. A document is data that may
// have come from someone else, so a button must never be able to launch a program:
// executable and script types are refused (the user can reveal them in Explorer).
bool isLaunchableType(std::wstring path) {
  while (!path.empty() && (path.back() == L' ' || path.back() == L'.')) path.pop_back();  // Windows ignores these
  size_t d = path.find_last_of(L'.'), sl = path.find_last_of(L"\\/");
  if (d == std::wstring::npos || (sl != std::wstring::npos && d < sl)) return false;
  std::wstring e = path.substr(d + 1);
  for (wchar_t& c : e) c = (wchar_t)std::towlower(c);
  static const wchar_t* bad[] = {
      L"exe", L"com", L"bat", L"cmd", L"scr", L"pif", L"msi", L"msp", L"lnk", L"url", L"js", L"jse",
      L"vbs", L"vbe", L"wsf", L"wsh", L"ps1", L"psm1", L"hta", L"jar", L"reg", L"cpl", L"dll", L"appx",
      L"msix", L"msixbundle", L"gadget", L"inf", L"application", L"chm", L"scf", L"diagcab", L"xll",
      L"settingcontent-ms", L"library-ms", L"searchconnector-ms", L"sys", L"vb", L"ws"};
  for (const wchar_t* b : bad)
    if (e == b) return true;
  return false;
}

void openLink(const std::wstring& raw) {
  std::wstring t = raw;
  while (!t.empty() && iswspace(t.back())) t.pop_back();
  while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
  if (t.empty()) return;
  std::wstring low = t;
  for (wchar_t& c : low) c = (wchar_t)std::towlower(c);
  auto starts = [&](const wchar_t* p) { return low.compare(0, wcslen(p), p) == 0; };

  if (starts(L"cairn-new:")) {  // a red [[wiki link]]: make the page (one undo step) and go there
    std::string title = narrow(t.substr(10));
    if (!g.doc || title.empty()) return;
    int64_t id = g.doc->pageByTitle(title);
    if (!id) id = g.doc->addPage(title);
    refreshPageList();
    navigate(id, true);
    return;
  }
  if (starts(L"cairn-page:")) {  // a [[wiki link]] resolved by the canvas
    long long id = wcstoll(t.c_str() + 11, nullptr, 10);
    if (g.doc && id > 0 && g.doc->page(id)) navigate(id, true);
    return;
  }

  if (starts(L"http://") || starts(L"https://") || starts(L"mailto:")) {
    ShellExecuteW(g.hwnd, L"open", t.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  bool drive = t.size() >= 2 && t[1] == L':';
  bool unc = t.size() >= 2 && t[0] == L'\\' && t[1] == L'\\';
  if (!drive && !unc && t.find(L':') != std::wstring::npos) {  // some other scheme: javascript:, ms-msdt:, file:...
    showError(L"Cairn only opens web addresses (http and https), email links and files:\n\n" + t);
    return;
  }
  std::wstring path = t;
  if (!drive && !unc && g.doc) {  // relative paths are relative to the .cairn file, so a library can move as a unit
    std::wstring doc = widen(g.doc->path());
    size_t sl = doc.find_last_of(L"\\/");
    if (sl != std::wstring::npos) path = doc.substr(0, sl + 1) + t;
  }
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    showError(L"Can't find:\n\n" + path);
    return;
  }
  if (isLaunchableType(path)) {
    std::wstring m = L"Cairn doesn't run programs from a document.\n\n" + path +
                     L"\n\nShow it in its folder instead?";
    if (MessageBoxW(g.hwnd, m.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
      std::wstring arg = L"/select,\"" + path + L"\"";
      ShellExecuteW(g.hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
    return;
  }
  ShellExecuteW(g.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void cmdAddImage(int px = -1, int py = -1) {
  if (!g.doc) return;
  static wchar_t buf[32768];
  buf[0] = 0;
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = g.hwnd;
  ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico;*.webp;*.heic\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 32767;
  ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return;
  std::vector<std::wstring> files;
  std::wstring first = buf;
  const wchar_t* next = buf + first.size() + 1;
  if (*next == 0) {
    files.push_back(first);  // exactly one file: the buffer holds its full path
  } else {                   // several: directory, then bare file names
    while (*next) {
      std::wstring name = next;
      files.push_back(first + L"\\" + name);
      next += name.size() + 1;
    }
  }
  g.canvas.addFilesAt(files, px, py);
}

void cmdAddAudio() {
  if (!g.doc) return;
  wchar_t buf[1024] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = g.hwnd;
  ofn.lpstrFilter = L"Audio (*.wav;*.mp3;*.m4a;*.wma;*.mid)\0*.wav;*.mp3;*.m4a;*.wma;*.mid;*.midi\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 1023;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return;
  g.canvas.addFilesAt({std::wstring(buf)}, -1, -1);
}

// ------------------------------------------------------------ function-key bar
// The F1-F10 strip file managers have along the bottom: each cell is also a button.

struct FKey { int id; const wchar_t* label; };
const FKey kFKeys[10] = {
    {IDM_HELP_ABOUT, L"Help"},     {IDM_EDIT_EDITTEXT, L"Edit Text"}, {IDM_VIEW_EDITMODE, L""},
    {IDM_EDIT_PROPS, L"Properties"}, {IDM_EDIT_DUPLICATE, L"Duplicate"}, {IDM_PAGE_RENAME, L"Rename Page"},
    {IDM_PAGE_NEW, L"New Page"},   {IDM_EDIT_DELETE, L"Delete"},       {IDM_VIEW_PALETTE, L"Palette"},
    {IDM_PAGE_FIND, L"Find"},
};
int gFkHover = -1, gFkDown = -1;

bool fkEnabled(int i) {
  bool has = g.doc != nullptr, edit = g.canvas.editMode();
  size_t nsel = g.canvas.selectionCount();
  switch (i) {
    case 0: return true;
    case 1: return has && edit && nsel == 1;
    case 3: return has && edit;
    case 4: case 7: return has && edit && nsel > 0;
    default: return has;
  }
}

int fkCellAt(int x, int width) { return width > 0 && x >= 0 ? std::min(9, x * 10 / width) : -1; }

LRESULT CALLBACK fkProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
      HGDIOBJ of = SelectObject(dc, g.font);
      SetBkMode(dc, TRANSPARENT);
      for (int i = 0; i < 10; ++i) {
        RECT c{rc.right * i / 10, 0, rc.right * (i + 1) / 10, rc.bottom};
        bool on = fkEnabled(i);
        if (on && (i == gFkDown || i == gFkHover)) {
          HBRUSH hb = CreateSolidBrush(i == gFkDown ? RGB(0xC4, 0xD8, 0xF2) : RGB(0xE2, 0xEC, 0xF8));
          RECT in = c;
          InflateRect(&in, -1, -2);
          FillRect(dc, &in, hb);
          DeleteObject(hb);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC8, 0xC8, 0xC8));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, c.right - 1, c.top + 3, nullptr);
        LineTo(dc, c.right - 1, c.bottom - 3);
        SelectObject(dc, op);
        DeleteObject(pen);
        std::wstring key = L"F" + std::to_wstring(i + 1);
        std::wstring label = kFKeys[i].label;
        if (i == 2) label = g.canvas.editMode() ? L"Run" : L"Edit";
        RECT kr{c.left + S(5), 0, c.left + S(5) + S(22), rc.bottom};
        SetTextColor(dc, on ? RGB(0x2F, 0x6F, 0xDE) : GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, key.c_str(), -1, &kr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT lr{c.left + S(5) + S(22), 0, c.right - S(3), rc.bottom};
        SetTextColor(dc, on ? GetSysColor(COLOR_BTNTEXT) : GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      }
      SelectObject(dc, of);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      RECT rc;
      GetClientRect(h, &rc);
      int c = fkCellAt((short)LOWORD(l), rc.right);
      if (c != gFkHover) {
        gFkHover = c;
        InvalidateRect(h, nullptr, FALSE);
      }
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      gFkHover = -1;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      RECT rc;
      GetClientRect(h, &rc);
      int c = fkCellAt((short)LOWORD(l), rc.right);
      if (c >= 0 && fkEnabled(c)) {
        gFkDown = c;
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      int was = gFkDown;
      gFkDown = -1;
      if (GetCapture() == h) ReleaseCapture();
      InvalidateRect(h, nullptr, FALSE);
      RECT rc;
      GetClientRect(h, &rc);
      if (was >= 0 && fkCellAt((short)LOWORD(l), rc.right) == was && (short)HIWORD(l) >= 0 && (short)HIWORD(l) < rc.bottom)
        PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(kFKeys[was].id, 0), 0);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (gFkDown >= 0) {
        gFkDown = -1;
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

void createFKeys() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = fkProc;
  wc.hInstance = g.hi;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"CairnFKeys";
  RegisterClassExW(&wc);
  g.fkShow = readIniInt(L"fkeys", 1) != 0;
  g.fkeys = CreateWindowExW(0, L"CairnFKeys", L"", WS_CHILD | (g.fkShow ? WS_VISIBLE : 0), 0, 0, 10, 10, g.hwnd,
                            nullptr, g.hi, nullptr);
}

// ------------------------------------------------------------ command palette
// Ctrl+P: type a few letters, press Enter. Lists every command and every page.

struct PalEntry {
  std::wstring label;
  int cmd = 0;
  int64_t page = 0;
};
struct Palette {
  HWND hwnd = nullptr, edit = nullptr, list = nullptr;
  std::vector<PalEntry> all, shown;
} pal;

const struct { int id; const wchar_t* name; const wchar_t* keys; } kPalCommands[] = {
    {IDM_EDIT_ADDTEXT, L"Add Text", L"Ctrl+Shift+T"},
    {IDM_EDIT_ADDBUTTON, L"Add Button", L"Ctrl+Shift+B"},
    {IDM_EDIT_ADDRECT, L"Add Rectangle", L"Ctrl+Shift+R"},
    {IDM_EDIT_ADDFOLDER, L"Add Folder Listing", L""},
    {IDM_EDIT_ADDTABLE, L"Add Table", L""},
    {IDM_EDIT_ADDAUDIO, L"Add Audio...", L""},
    {IDM_EDIT_ADDIMAGE, L"Add Image", L"Ctrl+Shift+I"},
    {IDM_PAGE_NEW, L"New Page", L"Ctrl+Shift+N"},
    {IDM_PAGE_RENAME, L"Rename Page", L""},
    {IDM_PAGE_DELETE, L"Delete Page", L""},
    {IDM_PAGE_PROPS, L"Page Properties (size, background)", L""},
    {IDM_PAGE_TAGS, L"Filter Pages by Tag...", L""},
    {IDM_PAGE_MOVEUP, L"Move Page Up", L"Alt+Up"},
    {IDM_PAGE_MOVEDOWN, L"Move Page Down", L"Alt+Down"},
    {IDM_PAGE_BACK, L"Go Back", L"Alt+Left"},
    {IDM_PAGE_FORWARD, L"Go Forward", L"Alt+Right"},
    {IDM_PAGE_FIND, L"Find Page", L"Ctrl+F"},
    {IDM_VIEW_EDITMODE, L"Toggle Edit / Run Mode", L"Ctrl+E"},
    {IDM_VIEW_SNAP, L"Toggle Snap to Grid", L""},
    {IDM_VIEW_ZOOMIN, L"Zoom In", L"Ctrl++"},
    {IDM_VIEW_ZOOMOUT, L"Zoom Out", L"Ctrl+-"},
    {IDM_VIEW_ZOOM100, L"Actual Size (100%)", L"Ctrl+0"},
    {IDM_EDIT_UNDO, L"Undo", L"Ctrl+Z"},
    {IDM_EDIT_REDO, L"Redo", L"Ctrl+Y"},
    {IDM_EDIT_CUT, L"Cut", L"Ctrl+X"},
    {IDM_EDIT_COPY, L"Copy", L"Ctrl+C"},
    {IDM_EDIT_PASTE, L"Paste", L"Ctrl+V"},
    {IDM_EDIT_PASTETEXT, L"Paste as Plain Text", L"Ctrl+Shift+V"},
    {IDM_FMT_BOLD, L"Format: Bold", L"Ctrl+B"},
    {IDM_FMT_ITALIC, L"Format: Italic", L"Ctrl+I"},
    {IDM_FMT_UNDERLINE, L"Format: Underline", L"Ctrl+U"},
    {IDM_FMT_STRIKE, L"Format: Strikethrough", L"Ctrl+Shift+X"},
    {IDM_FMT_CODE, L"Format: Code (monospace)", L""},
    {IDM_FMT_LINK, L"Format: Link...", L"Ctrl+K"},
    {IDM_FMT_COLOR, L"Format: Text Color...", L""},
    {IDM_FMT_CLEAR, L"Format: Clear Formatting", L""},
    {IDM_FMT_BIGGER, L"Format: Bigger Text", L"Ctrl+]"},
    {IDM_FMT_SMALLER, L"Format: Smaller Text", L"Ctrl+["},
    {IDM_VIEW_FKEYS, L"Toggle Function Key Bar", L""},
    {IDM_EDIT_DUPLICATE, L"Duplicate Selection", L"Ctrl+D"},
    {IDM_EDIT_DELETE, L"Delete Selection", L"Del"},
    {IDM_EDIT_SELECTALL, L"Select All", L"Ctrl+A"},
    {IDM_EDIT_REPLACE, L"Find and Replace...", L"Ctrl+H"},
    {IDM_EDIT_PROPS, L"Properties of Selection", L"Alt+Enter"},
    {IDM_EDIT_FRONT, L"Bring to Front", L""},
    {IDM_EDIT_BACK, L"Send to Back", L""},
    {IDM_FILE_NEW, L"New Cairn File", L"Ctrl+N"},
    {IDM_FILE_OPEN, L"Open Cairn File", L"Ctrl+O"},
    {IDM_FILE_SAVECOPY, L"Save a Copy As", L""},
    {IDM_FILE_EXPORT, L"Export as Website...", L""},
    {IDM_FILE_EXPORTMD, L"Export as Markdown...", L""},
    {IDM_FILE_IMPORTMD, L"Import Markdown...", L""},
    {IDM_HELP_ABOUT, L"About Cairn", L""},
};

std::wstring lowerW(std::wstring s) {
  for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
  return s;
}

void palFilter() {
  int n = GetWindowTextLengthW(pal.edit);
  std::wstring q((size_t)n + 1, L'\0');
  GetWindowTextW(pal.edit, &q[0], n + 1);
  q.resize((size_t)n);
  std::vector<std::wstring> words;
  {
    std::wstring cur;
    for (wchar_t c : lowerW(q)) {
      if (iswspace(c)) { if (!cur.empty()) words.push_back(cur), cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
  }
  pal.shown.clear();
  std::vector<PalEntry> rest;  // matches that don't start with the first word rank after those that do
  for (const PalEntry& e : pal.all) {
    std::wstring l = lowerW(e.label);
    bool ok = true;
    for (const auto& w : words)
      if (l.find(w) == std::wstring::npos) { ok = false; break; }
    if (!ok) continue;
    bool starts = words.empty() || l.compare(0, words[0].size(), words[0]) == 0 ||
                  (e.page > 0 && l.compare(12, words[0].size(), words[0]) == 0);  // after "Go to page: "
    (starts ? pal.shown : rest).push_back(e);
  }
  pal.shown.insert(pal.shown.end(), rest.begin(), rest.end());
  SendMessageW(pal.list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(pal.list, LB_RESETCONTENT, 0, 0);
  for (const PalEntry& e : pal.shown) SendMessageW(pal.list, LB_ADDSTRING, 0, (LPARAM)e.label.c_str());
  if (!pal.shown.empty()) SendMessageW(pal.list, LB_SETCURSEL, 0, 0);
  SendMessageW(pal.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(pal.list, nullptr, TRUE);
}

void palClose() {
  if (pal.hwnd) {
    HWND h = pal.hwnd;
    pal.hwnd = nullptr;  // first: DestroyWindow triggers WM_ACTIVATE, which must not recurse
    DestroyWindow(h);
  }
}

void palRun() {
  int i = (int)SendMessageW(pal.list, LB_GETCURSEL, 0, 0);
  if (i < 0 || i >= (int)pal.shown.size()) return;
  PalEntry e = pal.shown[(size_t)i];
  palClose();
  SetFocus(g.canvas.hwnd());
  if (e.page > 0) navigate(e.page, true);
  else PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(e.cmd, 0), 0);
}

LRESULT CALLBACK palEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (m == WM_KEYDOWN) {
    int cnt = (int)SendMessageW(pal.list, LB_GETCOUNT, 0, 0);
    int cur = (int)SendMessageW(pal.list, LB_GETCURSEL, 0, 0);
    auto move = [&](int to) { if (cnt > 0) SendMessageW(pal.list, LB_SETCURSEL, (WPARAM)std::clamp(to, 0, cnt - 1), 0); };
    switch (w) {
      case VK_DOWN: move(cur + 1); return 0;
      case VK_UP: move(cur - 1); return 0;
      case VK_NEXT: move(cur + 8); return 0;
      case VK_PRIOR: move(cur - 8); return 0;
      case VK_RETURN: palRun(); return 0;
      case VK_ESCAPE: palClose(); SetFocus(g.canvas.hwnd()); return 0;
    }
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
  return DefSubclassProc(h, m, w, l);
}

POINT palPos{0, 0};  // where the palette was last left (screen coordinates)
bool palHasPos = false;
int palStrip() { return S(18); }  // the grab strip along the top

LRESULT CALLBACK palProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_NCHITTEST: {  // the strip drags the whole window
      POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
      ScreenToClient(h, &pt);
      if (pt.y >= 0 && pt.y < palStrip()) return HTCAPTION;
      break;
    }
    case WM_EXITSIZEMOVE: {
      RECT r;
      GetWindowRect(h, &r);
      palPos = {r.left, r.top};
      palHasPos = true;
      if (pal.edit) SetFocus(pal.edit);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      RECT strip{0, 0, rc.right, palStrip()};
      FillRect(dc, &strip, GetSysColorBrush(COLOR_BTNFACE));
      HGDIOBJ of = SelectObject(dc, g.font);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      RECT tr{S(8), 0, rc.right - S(8), palStrip()};
      DrawTextW(dc, L"Command Palette  \u00B7  drag here to move", -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(dc, of);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_COMMAND:
      if ((HWND)l == pal.edit && HIWORD(w) == EN_CHANGE) palFilter();
      else if ((HWND)l == pal.list && HIWORD(w) == LBN_DBLCLK) palRun();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE && pal.hwnd == h) palClose();  // clicking away dismisses it
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

void showPalette() {
  if (!g.doc) return;
  palClose();
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = palProc;
    wc.hInstance = g.hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CairnPalette";
    RegisterClassExW(&wc);
    registered = true;
  }
  pal.all.clear();
  for (const auto& c : kPalCommands) {
    std::wstring label = c.name;
    if (*c.keys) label += L"   (" + std::wstring(c.keys) + L")";
    pal.all.push_back({label, c.id, 0});
  }
  for (const Page& p : g.doc->pages()) pal.all.push_back({L"Go to page: " + widen(p.title), 0, p.id});

  RECT rc;
  GetWindowRect(g.hwnd, &rc);
  int strip = palStrip();
  int w = S(540), h = S(360) + strip, pad = S(6), eh = S(28);
  int x = rc.left + ((rc.right - rc.left) - w) / 2, y = rc.top + S(90);
  if (palHasPos) {  // reopen where it was left, but never off every screen
    RECT want{palPos.x, palPos.y, palPos.x + w, palPos.y + h};
    HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(mon, &mi)) {
      x = std::clamp((int)palPos.x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
      y = std::clamp((int)palPos.y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h));
    }
  }
  pal.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"CairnPalette", L"", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, x, y,
                             w, h, g.hwnd, nullptr, g.hi, nullptr);
  pal.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, pad, strip + pad, w - 2 * pad - 2,
                             eh, pal.hwnd, nullptr, g.hi, nullptr);
  SendMessageW(pal.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type a command or a page name");
  pal.list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             pad, strip + pad + eh + pad, w - 2 * pad - 2, h - strip - eh - 3 * pad - 2, pal.hwnd, nullptr, g.hi,
                             nullptr);
  SendMessageW(pal.edit, WM_SETFONT, (WPARAM)g.font, TRUE);
  SendMessageW(pal.list, WM_SETFONT, (WPARAM)g.font, TRUE);
  SetWindowSubclass(pal.edit, palEditProc, 2, 0);
  palFilter();
  ShowWindow(pal.hwnd, SW_SHOW);
  SetForegroundWindow(pal.hwnd);
  SetFocus(pal.edit);
}

std::vector<std::string> gTagNames;  // what the tag popup's items stand for
void cmdTagMenu();
void onCommand(int id, int code, HWND ctl) {
  try {
    if (id >= kTagBase && id <= kTagBase + 200) {  // an item of the tag popup
      if (id == kTagBase) SetWindowTextW(g.search, L"");
      else if ((size_t)(id - kTagBase - 1) < gTagNames.size())
        SetWindowTextW(g.search, (L"#" + widen(gTagNames[(size_t)(id - kTagBase - 1)])).c_str());
      return;
    }
    switch (id) {
      case IDM_FILE_NEW: cmdNew(); break;
      case IDM_FILE_OPEN: cmdOpen(); break;
      case IDM_FILE_SAVECOPY: cmdSaveCopy(); break;
      case IDM_FILE_EXPORT: cmdExportSite(); break;
      case IDM_FILE_EXPORTMD: cmdExportMarkdown(); break;
      case IDM_FILE_IMPORTMD: cmdImportMarkdown(); break;
      case IDM_FILE_EXIT: SendMessageW(g.hwnd, WM_CLOSE, 0, 0); break;

      case IDM_EDIT_UNDO: if (g.doc) g.doc->undo(); break;
      case IDM_EDIT_REDO: if (g.doc) g.doc->redo(); break;
      case IDM_EDIT_ADDTEXT: g.canvas.addObject(ObjType::Text); break;
      case IDM_EDIT_ADDBUTTON: g.canvas.addObject(ObjType::Button); break;
      case IDM_EDIT_ADDRECT: g.canvas.addObject(ObjType::Rect); break;
      case IDM_EDIT_ADDFOLDER: g.canvas.addObject(ObjType::Folder); break;
      case IDM_EDIT_ADDTABLE: g.canvas.addObject(ObjType::Table); break;
      case IDM_EDIT_ADDAUDIO: cmdAddAudio(); break;
      case IDM_TBL_ROW_ABOVE: g.canvas.tableOp(0); break;
      case IDM_TBL_ROW_BELOW: g.canvas.tableOp(1); break;
      case IDM_TBL_ROW_DEL: g.canvas.tableOp(2); break;
      case IDM_TBL_COL_LEFT: g.canvas.tableOp(3); break;
      case IDM_TBL_COL_RIGHT: g.canvas.tableOp(4); break;
      case IDM_TBL_COL_DEL: g.canvas.tableOp(5); break;
      case IDM_EDIT_ADDIMAGE: cmdAddImage(); break;
      case IDM_EDIT_CUT: g.canvas.copySelection(true); break;
      case IDM_EDIT_COPY: g.canvas.copySelection(false); break;
      case IDM_EDIT_PASTE: g.canvas.paste(); break;
      case IDM_EDIT_PASTETEXT: g.canvas.paste(true); break;
      case IDM_CTX_PASTE: case IDM_CTX_PASTETEXT: {
        POINT c = g.canvas.contextPoint();
        bool atPoint = g.canvas.selectionCount() == 0;  // on an object, paste is the normal offset paste
        g.canvas.paste(id == IDM_CTX_PASTETEXT, atPoint ? (int)c.x : -1, atPoint ? (int)c.y : -1);
        break;
      }
      case IDM_CTX_ADDTEXT: g.canvas.addObject(ObjType::Text, g.canvas.contextPoint().x, g.canvas.contextPoint().y); break;
      case IDM_CTX_ADDBUTTON: g.canvas.addObject(ObjType::Button, g.canvas.contextPoint().x, g.canvas.contextPoint().y); break;
      case IDM_CTX_ADDRECT: g.canvas.addObject(ObjType::Rect, g.canvas.contextPoint().x, g.canvas.contextPoint().y); break;
      case IDM_CTX_ADDIMAGE: cmdAddImage(g.canvas.contextPoint().x, g.canvas.contextPoint().y); break;
      case IDM_CTX_OPENLINK: openLink(g.canvas.contextLink()); break;
      case IDM_CTX_COPYLINK: {
        std::wstring t = g.canvas.contextLink();
        if (!t.empty() && OpenClipboard(g.hwnd)) {
          EmptyClipboard();
          HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
          if (mem) {
            memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
          }
          CloseClipboard();
        }
        break;
      }
      case IDM_FMT_BOLD: g.canvas.applyFormat(Canvas::FmtBold); break;
      case IDM_FMT_ITALIC: g.canvas.applyFormat(Canvas::FmtItalic); break;
      case IDM_FMT_UNDERLINE: g.canvas.applyFormat(Canvas::FmtUnderline); break;
      case IDM_FMT_STRIKE: g.canvas.applyFormat(Canvas::FmtStrike); break;
      case IDM_FMT_LINK: g.canvas.applyFormat(Canvas::FmtLink); break;
      case IDM_FMT_COLOR: g.canvas.applyFormat(Canvas::FmtColor); break;
      case IDM_FMT_CODE: g.canvas.applyFormat(Canvas::FmtCode); break;
      case IDM_FMT_CLEAR: g.canvas.applyFormat(Canvas::FmtClear); break;
      case IDM_FMT_BIGGER: g.canvas.changeTextSize(1); break;
      case IDM_FMT_SMALLER: g.canvas.changeTextSize(-1); break;
      case IDM_VIEW_FKEYS:
        g.fkShow = !g.fkShow;
        writeIniInt(L"fkeys", g.fkShow ? 1 : 0);
        layout();
        break;
      case IDM_EDIT_EDITTEXT: g.canvas.beginTextEdit(); break;
      case IDM_EDIT_REPLACE: cmdReplace(); break;
      case IDM_EDIT_PROPS: g.canvas.setEditMode(true); g.canvas.editProperties(); break;
      case IDM_EDIT_DUPLICATE: g.canvas.duplicateSelection(); break;
      case IDM_EDIT_DELETE: g.canvas.deleteSelection(); break;
      case IDM_EDIT_SELECTALL: g.canvas.selectAll(); break;
      case IDM_EDIT_FRONT: g.canvas.bringToFront(); break;
      case IDM_EDIT_BACK: g.canvas.sendToBack(); break;

      case IDM_PAGE_NEW: cmdNewPage(); break;
      case IDM_PAGE_RENAME: cmdRenamePage(); break;
      case IDM_PAGE_DELETE: cmdDeletePage(); break;
      case IDM_PAGE_PROPS: g.canvas.editPageProperties(); break;
      case IDM_PAGE_TAGS: cmdTagMenu(); break;
      case IDM_PAGE_MOVEUP: cmdMovePage(-1); break;
      case IDM_PAGE_MOVEDOWN: cmdMovePage(+1); break;
      case IDM_PAGE_BACK: historyStep(-1); break;
      case IDM_PAGE_FORWARD: historyStep(+1); break;
      case IDM_PAGE_FIND: SetFocus(g.search); SendMessageW(g.search, EM_SETSEL, 0, -1); break;

      case IDM_VIEW_EDITMODE: g.canvas.setEditMode(!g.canvas.editMode()); break;
      case IDM_VIEW_SNAP: g.canvas.setSnap(!g.canvas.snap()); break;
      case IDM_VIEW_ZOOMIN: g.canvas.zoomStep(+1); break;
      case IDM_VIEW_ZOOMOUT: g.canvas.zoomStep(-1); break;
      case IDM_VIEW_ZOOM100: g.canvas.setZoom(1.0); break;
      case IDM_VIEW_PALETTE: showPalette(); break;
      case IDM_HELP_ABOUT: cmdAbout(); break;

      case IDC_SEARCH:
        if (code == EN_CHANGE && !g.inRefresh) refreshPageList();
        break;
      case IDC_PAGES:
        if (code == LBN_SELCHANGE && !g.inRefresh) {
          int i = (int)SendMessageW(g.pageList, LB_GETCURSEL, 0, 0);
          if (i >= 0 && i < (int)g.listIds.size()) navigate(g.listIds[(size_t)i], true);
        }
        break;
      case IDC_BACKLINKS:
        if (code == LBN_SELCHANGE && !g.inRefresh) {
          int i = (int)SendMessageW(g.backList, LB_GETCURSEL, 0, 0);
          if (i >= 0 && i < (int)g.backIds.size()) navigate(g.backIds[(size_t)i], true);
        }
        break;
    }
  } catch (const DbError& e) {
    showError(widen(e.what()));
  }
  (void)ctl;
}

// ----------------------------------------------------------------- menus etc.

HMENU buildMenu() {
  HMENU bar = CreateMenu();
  auto add = [&](HMENU m, int id, const wchar_t* t) { AppendMenuW(m, MF_STRING, (UINT_PTR)id, t); };
  auto sep = [&](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };

  HMENU file = CreatePopupMenu();
  add(file, IDM_FILE_NEW, L"&New Cairn...\tCtrl+N");
  add(file, IDM_FILE_OPEN, L"&Open...\tCtrl+O");
  add(file, IDM_FILE_SAVECOPY, L"Save a &Copy As...");
  add(file, IDM_FILE_EXPORT, L"&Export as Website...");
  add(file, IDM_FILE_EXPORTMD, L"Export as &Markdown...");
  add(file, IDM_FILE_IMPORTMD, L"&Import Markdown...");
  sep(file);
  add(file, IDM_FILE_EXIT, L"E&xit");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

  HMENU edit = CreatePopupMenu();
  add(edit, IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
  add(edit, IDM_EDIT_REDO, L"&Redo\tCtrl+Y");
  sep(edit);
  add(edit, IDM_EDIT_CUT, L"Cu&t\tCtrl+X");
  add(edit, IDM_EDIT_COPY, L"&Copy\tCtrl+C");
  add(edit, IDM_EDIT_PASTE, L"&Paste\tCtrl+V");
  add(edit, IDM_EDIT_PASTETEXT, L"Paste as Plain Te&xt\tCtrl+Shift+V");
  sep(edit);
  add(edit, IDM_EDIT_ADDTEXT, L"Add &Text\tCtrl+Shift+T");
  add(edit, IDM_EDIT_ADDBUTTON, L"Add &Button\tCtrl+Shift+B");
  add(edit, IDM_EDIT_ADDRECT, L"Add Rect&angle\tCtrl+Shift+R");
  add(edit, IDM_EDIT_ADDFOLDER, L"Add &Folder Listing");
  add(edit, IDM_EDIT_ADDTABLE, L"Add Ta&ble");
  add(edit, IDM_EDIT_ADDAUDIO, L"Add Au&dio...");
  add(edit, IDM_EDIT_ADDIMAGE, L"Add &Image...\tCtrl+Shift+I");
  sep(edit);
  add(edit, IDM_EDIT_EDITTEXT, L"Edit Te&xt\tF2");
  {
    HMENU fmt = CreatePopupMenu();
    AppendMenuW(fmt, MF_STRING, IDM_FMT_BOLD, L"&Bold\tCtrl+B");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_ITALIC, L"&Italic\tCtrl+I");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_UNDERLINE, L"&Underline\tCtrl+U");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_STRIKE, L"&Strikethrough\tCtrl+Shift+X");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_CODE, L"&Code (monospace)");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_LINK, L"&Link...\tCtrl+K");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_COLOR, L"Text Co&lor...");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_BIGGER, L"&Bigger Text\tCtrl+]");
    AppendMenuW(fmt, MF_STRING, IDM_FMT_SMALLER, L"S&maller Text\tCtrl+[");
    AppendMenuW(fmt, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fmt, MF_STRING, IDM_FMT_CLEAR, L"C&lear Formatting");
    AppendMenuW(edit, MF_POPUP, (UINT_PTR)fmt, L"Text &Format");
  }
  add(edit, IDM_EDIT_REPLACE, L"Find and &Replace...\tCtrl+H");
  add(edit, IDM_EDIT_PROPS, L"&Properties...\tAlt+Enter");
  add(edit, IDM_EDIT_DUPLICATE, L"&Duplicate\tCtrl+D");
  add(edit, IDM_EDIT_DELETE, L"De&lete\tDel");
  add(edit, IDM_EDIT_SELECTALL, L"Select &All\tCtrl+A");
  sep(edit);
  add(edit, IDM_EDIT_FRONT, L"Bring to &Front");
  add(edit, IDM_EDIT_BACK, L"Send to Bac&k");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");

  HMENU page = CreatePopupMenu();
  add(page, IDM_PAGE_NEW, L"&New Page...\tCtrl+Shift+N");
  add(page, IDM_PAGE_RENAME, L"&Rename Page...");
  add(page, IDM_PAGE_DELETE, L"&Delete Page");
  add(page, IDM_PAGE_PROPS, L"Page &Properties...");
  add(page, IDM_PAGE_TAGS, L"Filter by &Tag...");
  add(page, IDM_PAGE_MOVEUP, L"Move Page &Up\tAlt+Up");
  add(page, IDM_PAGE_MOVEDOWN, L"Move Page Do&wn\tAlt+Down");
  sep(page);
  add(page, IDM_PAGE_BACK, L"&Back\tAlt+Left");
  add(page, IDM_PAGE_FORWARD, L"&Forward\tAlt+Right");
  sep(page);
  add(page, IDM_PAGE_FIND, L"F&ind Page\tCtrl+F");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)page, L"&Page");

  HMENU view = CreatePopupMenu();
  add(view, IDM_VIEW_PALETTE, L"&Command Palette...\tCtrl+P");
  add(view, IDM_VIEW_EDITMODE, L"&Edit Mode\tCtrl+E");
  add(view, IDM_VIEW_SNAP, L"&Snap to Grid");
  add(view, IDM_VIEW_FKEYS, L"&Function Key Bar");
  sep(view);
  add(view, IDM_VIEW_ZOOMIN, L"Zoom &In\tCtrl++");
  add(view, IDM_VIEW_ZOOMOUT, L"Zoom &Out\tCtrl+-");
  add(view, IDM_VIEW_ZOOM100, L"&Actual Size\tCtrl+0");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

  HMENU help = CreatePopupMenu();
  add(help, IDM_HELP_ABOUT, L"&About Cairn");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
  return bar;
}

HACCEL buildAccel() {
  const BYTE C = FVIRTKEY | FCONTROL, CS = FVIRTKEY | FCONTROL | FSHIFT, A = FVIRTKEY | FALT;
  ACCEL a[] = {
      {C, 'N', IDM_FILE_NEW},          {C, 'O', IDM_FILE_OPEN},
      {C, 'Z', IDM_EDIT_UNDO},         {C, 'Y', IDM_EDIT_REDO},
      {CS, 'Z', IDM_EDIT_REDO},        {C, 'D', IDM_EDIT_DUPLICATE},
      {C, 'A', IDM_EDIT_SELECTALL},    {C, 'E', IDM_VIEW_EDITMODE},
      {C, 'F', IDM_PAGE_FIND},         {C, 'H', IDM_EDIT_REPLACE},         {CS, 'T', IDM_EDIT_ADDTEXT},
      {CS, 'B', IDM_EDIT_ADDBUTTON},   {CS, 'R', IDM_EDIT_ADDRECT},
      {CS, 'N', IDM_PAGE_NEW},         {A, VK_LEFT, IDM_PAGE_BACK},
      {A, VK_RIGHT, IDM_PAGE_FORWARD},
      {A, VK_UP, IDM_PAGE_MOVEUP},     {A, VK_DOWN, IDM_PAGE_MOVEDOWN}, {A, VK_RETURN, IDM_EDIT_PROPS},
      {C, VK_OEM_PLUS, IDM_VIEW_ZOOMIN}, {C, VK_ADD, IDM_VIEW_ZOOMIN},
      {C, VK_OEM_MINUS, IDM_VIEW_ZOOMOUT}, {C, VK_SUBTRACT, IDM_VIEW_ZOOMOUT},
      {C, '0', IDM_VIEW_ZOOM100},       {C, 'C', IDM_EDIT_COPY},
      {C, 'X', IDM_EDIT_CUT},           {C, 'V', IDM_EDIT_PASTE},
      {CS, 'I', IDM_EDIT_ADDIMAGE},     {C, 'P', IDM_VIEW_PALETTE},
      {C, 'B', IDM_FMT_BOLD},           {C, 'I', IDM_FMT_ITALIC},
      {C, 'U', IDM_FMT_UNDERLINE},      {CS, 'X', IDM_FMT_STRIKE},
      {C, 'K', IDM_FMT_LINK},           {CS, 'V', IDM_EDIT_PASTETEXT},
      {C, VK_OEM_6, IDM_FMT_BIGGER},    {C, VK_OEM_4, IDM_FMT_SMALLER},
      {FVIRTKEY, VK_F1, IDM_HELP_ABOUT}, {FVIRTKEY, VK_F2, IDM_EDIT_EDITTEXT},
      {FVIRTKEY, VK_F3, IDM_VIEW_EDITMODE}, {FVIRTKEY, VK_F4, IDM_EDIT_PROPS},
      {FVIRTKEY, VK_F5, IDM_EDIT_DUPLICATE}, {FVIRTKEY, VK_F6, IDM_PAGE_RENAME},
      {FVIRTKEY, VK_F7, IDM_PAGE_NEW}, {FVIRTKEY, VK_F8, IDM_EDIT_DELETE},
      {FVIRTKEY, VK_F9, IDM_VIEW_PALETTE}, {FVIRTKEY, VK_F10, IDM_PAGE_FIND},
  };
  return CreateAcceleratorTableW(a, (int)(sizeof a / sizeof a[0]));
}

void setMenuText(HMENU m, int id, const std::wstring& text, bool enabled) {
  ModifyMenuW(m, (UINT)id, MF_BYCOMMAND | MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), (UINT_PTR)id,
              text.c_str());
}

void updateMenuState() {
  HMENU m = g.menu;
  bool has = g.doc != nullptr;
  bool edit = g.canvas.editMode();
  size_t nsel = g.canvas.selectionCount();

  std::wstring u = L"&Undo", r = L"&Redo";
  if (has && g.doc->canUndo()) u += L" " + widen(g.doc->undoLabel());
  if (has && g.doc->canRedo()) r += L" " + widen(g.doc->redoLabel());
  setMenuText(m, IDM_EDIT_UNDO, u + L"\tCtrl+Z", has && g.doc->canUndo());
  setMenuText(m, IDM_EDIT_REDO, r + L"\tCtrl+Y", has && g.doc->canRedo());

  auto en = [&](int id, bool on) { EnableMenuItem(m, (UINT)id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
  en(IDM_EDIT_EDITTEXT, edit && nsel == 1);
  en(IDM_EDIT_CUT, edit && nsel > 0);
  en(IDM_EDIT_COPY, edit && nsel > 0);
  en(IDM_EDIT_PASTE, has);
  en(IDM_EDIT_PASTETEXT, has);
  for (int id : {IDM_FMT_BOLD, IDM_FMT_ITALIC, IDM_FMT_UNDERLINE, IDM_FMT_STRIKE, IDM_FMT_LINK, IDM_FMT_COLOR,
                 IDM_FMT_CODE, IDM_FMT_CLEAR, IDM_FMT_BIGGER, IDM_FMT_SMALLER})
    en(id, edit && nsel > 0);
  en(IDM_EDIT_DUPLICATE, edit && nsel > 0);
  en(IDM_EDIT_DELETE, edit && nsel > 0);
  en(IDM_EDIT_FRONT, edit && nsel > 0);
  en(IDM_EDIT_BACK, edit && nsel > 0);
  en(IDM_EDIT_SELECTALL, edit);
  en(IDM_FILE_SAVECOPY, has);
  en(IDM_FILE_EXPORT, has);
  en(IDM_FILE_EXPORTMD, has);
  en(IDM_FILE_IMPORTMD, has);
  {
    int ci = currentPageIndex();
    en(IDM_PAGE_MOVEUP, ci > 0);
    en(IDM_PAGE_MOVEDOWN, ci >= 0 && ci + 1 < (int)g.doc->pages().size());
  }
  en(IDM_PAGE_BACK, g.hpos > 0);
  en(IDM_PAGE_FORWARD, g.hpos >= 0 && g.hpos + 1 < (int)g.hist.size());
  CheckMenuItem(m, IDM_VIEW_EDITMODE, MF_BYCOMMAND | (edit ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(m, IDM_VIEW_SNAP, MF_BYCOMMAND | (g.canvas.snap() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(m, IDM_VIEW_FKEYS, MF_BYCOMMAND | (g.fkShow ? MF_CHECKED : MF_UNCHECKED));
}


// ---------------------------------------------------------------- toolbar
// Icons are drawn here at runtime (no resource files to ship): simple flat glyphs in a
// colour-keyed bitmap, so they scale cleanly to 16/24 px for the current DPI.

enum Icon { I_TEXT, I_BUTTON, I_SHAPE, I_IMAGE, I_BOLD, I_ITALIC, I_UNDERLINE, I_LINK, I_ZOOMIN, I_ZOOMOUT,
            I_EDIT, I_SNAP, I_PAGENEW, I_BACK, I_FWD, I_UP, I_DOWN, I_PALETTE,
            I_UNDO, I_REDO, I_CUT, I_COPY, I_PASTE, I_DELETE, I_TAG, I_BIGGER, I_SMALLER, I_COUNT };

void drawIcon(HDC dc, int icon, int sz) {
  const COLORREF ink = RGB(0x2B, 0x3A, 0x4A), blue = RGB(0x2F, 0x6F, 0xC8), light = RGB(0xCF, 0xE0, 0xF4),
                 green = RGB(0x3C, 0x9A, 0x5A), sun = RGB(0xE0, 0xA0, 0x20);
  auto u = [&](double v) { return (int)std::lround(v * sz / 16.0); };
  int pw = std::max(1, u(1.4));
  HPEN pen = CreatePen(PS_SOLID, pw, ink);
  HPEN bpen = CreatePen(PS_SOLID, pw, blue);
  HBRUSH none = (HBRUSH)GetStockObject(NULL_BRUSH);
  HBRUSH lb = CreateSolidBrush(light), bb = CreateSolidBrush(blue), ib = CreateSolidBrush(ink),
         gb = CreateSolidBrush(green), sb = CreateSolidBrush(sun);
  HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, none);
  auto line = [&](double x1, double y1, double x2, double y2) {
    MoveToEx(dc, u(x1), u(y1), nullptr);
    LineTo(dc, u(x2), u(y2));
  };
  auto letter = [&](const wchar_t* t, bool bold, bool italic, bool under, COLORREF c) {
    LOGFONTW lf{};
    lf.lfHeight = -u(14);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = italic;
    lf.lfUnderline = under;
    lf.lfQuality = NONANTIALIASED_QUALITY;  // no AA: blends into the colour key would leave fringes
    wcscpy(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT r = {0, 0, sz, sz};
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(f);
  };
  auto poly = [&](std::initializer_list<POINT> pts, HBRUSH fill) {
    std::vector<POINT> v(pts);
    HGDIOBJ o = SelectObject(dc, fill);
    Polygon(dc, v.data(), (int)v.size());
    SelectObject(dc, o);
  };
  switch (icon) {
    case I_TEXT: letter(L"A", true, false, false, ink); break;
    case I_BOLD: letter(L"B", true, false, false, ink); break;
    case I_BIGGER:
      letter(L"A", false, false, false, ink);
      SelectObject(dc, bpen);
      poly({{u(12), u(1.5)}, {u(15.5), u(6.5)}, {u(8.5), u(6.5)}}, bb);
      break;
    case I_SMALLER:
      letter(L"A", false, false, false, ink);
      SelectObject(dc, bpen);
      poly({{u(8.5), u(9.5)}, {u(15.5), u(9.5)}, {u(12), u(14.5)}}, bb);
      break;
    case I_ITALIC: letter(L"I", false, true, false, ink); break;
    case I_UNDERLINE: letter(L"U", false, false, true, ink); break;
    case I_BUTTON:
      SelectObject(dc, bpen); SelectObject(dc, lb);
      RoundRect(dc, u(1), u(4), u(15), u(12), u(4), u(4));
      SelectObject(dc, bpen); line(5, 8, 11, 8);
      break;
    case I_SHAPE:
      SelectObject(dc, bpen); SelectObject(dc, lb);
      Rectangle(dc, u(2), u(3), u(14), u(13));
      break;
    case I_IMAGE:
      SelectObject(dc, pen); Rectangle(dc, u(1), u(2), u(15), u(14));
      SelectObject(dc, GetStockObject(NULL_PEN));
      poly({{u(2), u(13)}, {u(6), u(7)}, {u(9), u(11)}, {u(11), u(9)}, {u(14), u(13)}}, gb);
      SelectObject(dc, sb); Ellipse(dc, u(10), u(3.5), u(13), u(6.5));
      break;
    case I_LINK:
      SelectObject(dc, bpen);
      RoundRect(dc, u(1), u(5), u(9), u(11), u(6), u(6));
      RoundRect(dc, u(7), u(5), u(15), u(11), u(6), u(6));
      break;
    case I_ZOOMIN: case I_ZOOMOUT:
      SelectObject(dc, pen); Ellipse(dc, u(1.5), u(1.5), u(10.5), u(10.5));
      line(9.5, 9.5, 14.5, 14.5);
      SelectObject(dc, bpen); line(4, 6, 8, 6);
      if (icon == I_ZOOMIN) line(6, 4, 6, 8);
      break;
    case I_EDIT:
      SelectObject(dc, GetStockObject(NULL_PEN));
      poly({{u(2), u(14)}, {u(2.8), u(10.6)}, {u(11), u(2.4)}, {u(13.6), u(5)}, {u(5.4), u(13.2)}}, bb);
      SelectObject(dc, pen); line(2, 14, 14, 14);
      break;
    case I_SNAP:
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
          int cx = u(3 + a * 5), cy = u(3 + b * 5), r = std::max(1, u(1.2));
          SelectObject(dc, GetStockObject(NULL_PEN)); SelectObject(dc, ib);
          Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        }
      break;
    case I_PAGENEW:
      SelectObject(dc, pen); SelectObject(dc, none);
      poly({{u(3), u(1.5)}, {u(9), u(1.5)}, {u(12), u(4.5)}, {u(12), u(14.5)}, {u(3), u(14.5)}}, (HBRUSH)GetStockObject(WHITE_BRUSH));
      SelectObject(dc, bpen); line(9, 9, 15, 9); line(12, 6, 12, 12);
      break;
    case I_BACK: case I_FWD: {
      SelectObject(dc, GetStockObject(NULL_PEN));
      int d = icon == I_BACK ? 1 : -1;
      auto X = [&](double x) { return icon == I_BACK ? x : 16 - x; };
      (void)d;
      poly({{u(X(2)), u(8)}, {u(X(8)), u(2.5)}, {u(X(8)), u(6)}, {u(X(14)), u(6)}, {u(X(14)), u(10)}, {u(X(8)), u(10)}, {u(X(8)), u(13.5)}}, bb);
      break;
    }
    case I_UP: case I_DOWN: {
      SelectObject(dc, GetStockObject(NULL_PEN));
      auto Y = [&](double y) { return icon == I_UP ? y : 16 - y; };
      poly({{u(8), u(Y(2))}, {u(2.5), u(Y(8))}, {u(6), u(Y(8))}, {u(6), u(Y(14))}, {u(10), u(Y(14))}, {u(10), u(Y(8))}, {u(13.5), u(Y(8))}}, bb);
      break;
    }
    case I_UNDO: case I_REDO: {
      auto X = [&](double x) { return icon == I_UNDO ? x : 16 - x; };
      SelectObject(dc, bpen);
      Arc(dc, u(X(3)), u(4), u(X(14)), u(14), u(X(8.5)), u(4), u(X(14)), u(9));  // sweeps the right side
      if (icon == I_REDO) Arc(dc, u(2), u(4), u(13), u(14), u(2), u(9), u(7.5), u(4));
      SelectObject(dc, GetStockObject(NULL_PEN));
      poly({{u(X(2)), u(6)}, {u(X(8)), u(1.5)}, {u(X(8)), u(10)}}, bb);
      break;
    }
    case I_CUT:
      SelectObject(dc, pen);
      line(5, 2, 10, 10); line(11, 2, 6, 10);
      SelectObject(dc, bpen); Ellipse(dc, u(2.5), u(10), u(6.5), u(14)); Ellipse(dc, u(9.5), u(10), u(13.5), u(14));
      break;
    case I_COPY:
      SelectObject(dc, pen); SelectObject(dc, none); Rectangle(dc, u(2), u(2), u(10), u(11));
      SelectObject(dc, bpen); SelectObject(dc, lb); Rectangle(dc, u(6), u(5), u(14), u(14));
      break;
    case I_PASTE:
      SelectObject(dc, pen); SelectObject(dc, lb); Rectangle(dc, u(2.5), u(3), u(13.5), u(14.5));
      SelectObject(dc, ib); Rectangle(dc, u(5.5), u(1.5), u(10.5), u(4.5));
      SelectObject(dc, bpen); line(5, 8, 11, 8); line(5, 11, 11, 11);
      break;
    case I_DELETE: {
      HPEN red = CreatePen(PS_SOLID, std::max(pw, u(2)), RGB(0xC0, 0x39, 0x2B));
      SelectObject(dc, red);
      line(3.5, 3.5, 12.5, 12.5); line(12.5, 3.5, 3.5, 12.5);
      SelectObject(dc, pen);
      DeleteObject(red);
      break;
    }
    case I_TAG:
      SelectObject(dc, pen); SelectObject(dc, lb);
      poly({{u(2), u(3)}, {u(9), u(3)}, {u(14.5), u(8.5)}, {u(9), u(14)}, {u(2), u(7.5)}}, lb);
      SelectObject(dc, ib); Ellipse(dc, u(4), u(4.8), u(6), u(6.8));
      break;
    case I_PALETTE:
      SelectObject(dc, pen); SelectObject(dc, lb);
      RoundRect(dc, u(1), u(3), u(15), u(13), u(3), u(3));
      SelectObject(dc, bpen); line(4, 6, 7, 8); line(7, 8, 4, 10); line(8, 10.5, 12, 10.5);
      break;
  }
  SelectObject(dc, op);
  SelectObject(dc, obr);
  DeleteObject(pen); DeleteObject(bpen);
  DeleteObject(lb); DeleteObject(bb); DeleteObject(ib); DeleteObject(gb); DeleteObject(sb);
}

HIMAGELIST makeIconList(int sz) {
  HIMAGELIST il = ImageList_Create(sz, sz, ILC_COLOR24 | ILC_MASK, I_COUNT, 0);
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  const COLORREF key = RGB(255, 0, 255);
  for (int i = 0; i < I_COUNT; ++i) {
    HBITMAP bmp = CreateCompatibleBitmap(screen, sz, sz);
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH kb = CreateSolidBrush(key);
    RECT r = {0, 0, sz, sz};
    FillRect(dc, &r, kb);
    DeleteObject(kb);
    drawIcon(dc, i, sz);
    SelectObject(dc, old);
    ImageList_AddMasked(il, bmp, key);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return il;
}

struct TbSpec {
  int id;        // command id, or 0 for a separator
  int std;       // system image index (STD_*), or -1
  int own;       // our icon (Icon), or -1
  const wchar_t* label;  // shown beside the icon when non-null
  const wchar_t* tip;
  bool check;
};

const std::vector<TbSpec>& tbSpecs() {
  static const std::vector<TbSpec> v = {
      {IDM_PAGE_NEW, -1, I_PAGENEW, nullptr, L"New page (Ctrl+Shift+N)", false},
      {IDM_PAGE_BACK, -1, I_BACK, nullptr, L"Back (Alt+Left)", false},
      {IDM_PAGE_FORWARD, -1, I_FWD, nullptr, L"Forward (Alt+Right)", false},
      {IDM_PAGE_MOVEUP, -1, I_UP, nullptr, L"Move page up (Alt+Up)", false},
      {IDM_PAGE_MOVEDOWN, -1, I_DOWN, nullptr, L"Move page down (Alt+Down)", false},
      {IDM_PAGE_TAGS, -1, I_TAG, nullptr, L"Filter pages by tag", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_EDIT_UNDO, -1, I_UNDO, nullptr, L"Undo (Ctrl+Z)", false},
      {IDM_EDIT_REDO, -1, I_REDO, nullptr, L"Redo (Ctrl+Y)", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_EDIT_CUT, -1, I_CUT, nullptr, L"Cut (Ctrl+X)", false},
      {IDM_EDIT_COPY, -1, I_COPY, nullptr, L"Copy (Ctrl+C)", false},
      {IDM_EDIT_PASTE, -1, I_PASTE, nullptr, L"Paste (Ctrl+V)", false},
      {IDM_EDIT_DELETE, -1, I_DELETE, nullptr, L"Delete (Del)", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_EDIT_ADDTEXT, -1, I_TEXT, L"Text", L"Add a text box (Ctrl+Shift+T)", false},
      {IDM_EDIT_ADDBUTTON, -1, I_BUTTON, L"Button", L"Add a button (Ctrl+Shift+B)", false},
      {IDM_EDIT_ADDRECT, -1, I_SHAPE, L"Shape", L"Add a rectangle (Ctrl+Shift+R)", false},
      {IDM_EDIT_ADDIMAGE, -1, I_IMAGE, L"Image", L"Add images (Ctrl+Shift+I)", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_FMT_BOLD, -1, I_BOLD, nullptr, L"Bold (Ctrl+B)", false},
      {IDM_FMT_ITALIC, -1, I_ITALIC, nullptr, L"Italic (Ctrl+I)", false},
      {IDM_FMT_UNDERLINE, -1, I_UNDERLINE, nullptr, L"Underline (Ctrl+U)", false},
      {IDM_FMT_LINK, -1, I_LINK, nullptr, L"Link (Ctrl+K)", false},
      {IDM_FMT_BIGGER, -1, I_BIGGER, nullptr, L"Bigger text (Ctrl+])", false},
      {IDM_FMT_SMALLER, -1, I_SMALLER, nullptr, L"Smaller text (Ctrl+[)", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_VIEW_ZOOMOUT, -1, I_ZOOMOUT, nullptr, L"Zoom out (Ctrl+-)", false},
      {IDM_VIEW_ZOOMIN, -1, I_ZOOMIN, nullptr, L"Zoom in (Ctrl++)", false},
      {0, -1, -1, nullptr, nullptr, false},
      {IDM_VIEW_EDITMODE, -1, I_EDIT, L"Edit", L"Edit mode / Run mode (Ctrl+E)", true},
      {IDM_VIEW_SNAP, -1, I_SNAP, L"Snap", L"Snap to grid", true},
      {IDM_VIEW_PALETTE, -1, I_PALETTE, nullptr, L"Command palette (Ctrl+P)", false},
  };
  return v;
}

void createToolbar() {
  if (g.toolbar) { DestroyWindow(g.toolbar); g.toolbar = nullptr; }
  if (g.tbIcons) { ImageList_Destroy(g.tbIcons); g.tbIcons = nullptr; }
  bool large = g.dpi >= 120;
  int sz = large ? 24 : 16;
  g.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                              WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS |
                                  TBSTYLE_WRAPABLE | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
                              0, 0, 100, 30, g.hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR, g.hi, nullptr);
  SendMessageW(g.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(g.toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);  // text only where asked
  g.tbIcons = makeIconList(sz);
  SendMessageW(g.toolbar, TB_SETIMAGELIST, 0, (LPARAM)g.tbIcons);
  SendMessageW(g.toolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(sz, sz));
  SendMessageW(g.toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(sz + 10, sz + 10));
  std::vector<TBBUTTON> btns;
  for (const TbSpec& t : tbSpecs()) {
    TBBUTTON b{};
    if (t.id == 0) {
      b.fsStyle = BTNS_SEP;
      b.iBitmap = S(6);
    } else {
      b.idCommand = t.id;
      b.fsState = TBSTATE_ENABLED;
      b.fsStyle = (BYTE)(BTNS_BUTTON | BTNS_AUTOSIZE | (t.label ? BTNS_SHOWTEXT : 0) | (t.check ? BTNS_CHECK : 0));
      b.iBitmap = t.own;
      b.iString = t.label ? (INT_PTR)t.label : 0;
    }
    btns.push_back(b);
  }
  SendMessageW(g.toolbar, TB_ADDBUTTONSW, (WPARAM)btns.size(), (LPARAM)btns.data());
  SendMessageW(g.toolbar, WM_SETFONT, (WPARAM)g.font, TRUE);
}

void syncToolbar() {
  if (!g.toolbar) return;
  bool has = g.doc != nullptr;
  bool edit = g.canvas.editMode();
  size_t nsel = g.canvas.selectionCount();
  int ci = currentPageIndex();
  auto en = [&](int id, bool on) { SendMessageW(g.toolbar, TB_ENABLEBUTTON, (WPARAM)id, MAKELONG(on ? TRUE : FALSE, 0)); };
  auto chk = [&](int id, bool on) { SendMessageW(g.toolbar, TB_CHECKBUTTON, (WPARAM)id, MAKELONG(on ? TRUE : FALSE, 0)); };
  unsigned sty = g.canvas.editStyleFlags();  // B / I / U show the style at the caret while editing
  chk(IDM_FMT_BOLD, (sty & cairn::kBold) != 0);
  chk(IDM_FMT_ITALIC, (sty & cairn::kItalic) != 0);
  chk(IDM_FMT_UNDERLINE, (sty & cairn::kUnderline) != 0);
  en(IDM_PAGE_NEW, has);
  en(IDM_PAGE_TAGS, has);
  en(IDM_PAGE_BACK, g.hpos > 0);
  en(IDM_PAGE_FORWARD, g.hpos >= 0 && g.hpos + 1 < (int)g.hist.size());
  en(IDM_PAGE_MOVEUP, ci > 0);
  en(IDM_PAGE_MOVEDOWN, has && ci >= 0 && ci + 1 < (int)g.doc->pages().size());
  en(IDM_EDIT_UNDO, has && g.doc->canUndo());
  en(IDM_EDIT_REDO, has && g.doc->canRedo());
  en(IDM_EDIT_CUT, edit && nsel > 0);
  en(IDM_EDIT_COPY, edit && nsel > 0);
  en(IDM_EDIT_PASTE, has);
  en(IDM_EDIT_DELETE, edit && nsel > 0);
  for (int id : {IDM_EDIT_ADDTEXT, IDM_EDIT_ADDBUTTON, IDM_EDIT_ADDRECT, IDM_EDIT_ADDIMAGE, IDM_VIEW_ZOOMIN,
                 IDM_VIEW_ZOOMOUT, IDM_VIEW_EDITMODE, IDM_VIEW_SNAP, IDM_VIEW_PALETTE})
    en(id, has);
  bool fmt = edit && g.canvas.selectionHasText();
  for (int id : {IDM_FMT_BOLD, IDM_FMT_ITALIC, IDM_FMT_UNDERLINE, IDM_FMT_LINK}) en(id, fmt);
  bool sz = edit && g.canvas.selectionHasSizable();
  en(IDM_FMT_BIGGER, sz);
  en(IDM_FMT_SMALLER, sz);
  if (g.fkeys) InvalidateRect(g.fkeys, nullptr, FALSE);
  chk(IDM_VIEW_EDITMODE, edit);
  chk(IDM_VIEW_SNAP, g.canvas.snap());
}

const wchar_t* tooltipFor(int id) {
  for (const TbSpec& t : tbSpecs())
    if (t.id == id) return t.tip;
  return nullptr;
}

// ---------------------------------------------------------------- right-click menus

void canvasContextMenu(POINT screen) {
  if (!g.doc) return;
  bool edit = g.canvas.editMode();
  size_t nsel = g.canvas.selectionCount();
  std::wstring link = g.canvas.contextLink();
  HMENU m = CreatePopupMenu();
  auto add = [&](int id, const wchar_t* t, bool on = true) {
    AppendMenuW(m, MF_STRING | (on ? MF_ENABLED : MF_GRAYED), (UINT_PTR)id, t);
  };
  auto sep = [&]() {
    if (GetMenuItemCount(m) > 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  };
  if (!link.empty()) {
    add(IDM_CTX_OPENLINK, L"Open Link");
    add(IDM_CTX_COPYLINK, L"Copy Link Address");
  }
  if (!edit) {
    sep();
    add(IDM_PAGE_BACK, L"Back\tAlt+Left", g.hpos > 0);
    add(IDM_PAGE_FORWARD, L"Forward\tAlt+Right", g.hpos >= 0 && g.hpos + 1 < (int)g.hist.size());
    sep();
    add(IDM_VIEW_EDITMODE, L"Edit This Page\tCtrl+E");
  } else if (nsel > 0) {
    sep();
    add(IDM_EDIT_EDITTEXT, L"Edit Text\tF2", nsel == 1);
    add(IDM_EDIT_PROPS, L"Properties...\tAlt+Enter");
    sep();
    add(IDM_EDIT_CUT, L"Cut\tCtrl+X");
    add(IDM_EDIT_COPY, L"Copy\tCtrl+C");
    add(IDM_CTX_PASTE, L"Paste\tCtrl+V");
    add(IDM_EDIT_DUPLICATE, L"Duplicate\tCtrl+D");
    add(IDM_EDIT_DELETE, L"Delete\tDel");
    sep();
    add(IDM_EDIT_FRONT, L"Bring to Front");
    add(IDM_EDIT_BACK, L"Send to Back");
    if (nsel == 1 && g.canvas.contextOnTable()) {
      HMENU tm = CreatePopupMenu();
      AppendMenuW(tm, MF_STRING, IDM_TBL_ROW_ABOVE, L"Insert Row Above");
      AppendMenuW(tm, MF_STRING, IDM_TBL_ROW_BELOW, L"Insert Row Below");
      AppendMenuW(tm, MF_STRING, IDM_TBL_ROW_DEL, L"Delete Row");
      AppendMenuW(tm, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(tm, MF_STRING, IDM_TBL_COL_LEFT, L"Insert Column Left");
      AppendMenuW(tm, MF_STRING, IDM_TBL_COL_RIGHT, L"Insert Column Right");
      AppendMenuW(tm, MF_STRING, IDM_TBL_COL_DEL, L"Delete Column");
      sep();
      AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Table");
    }
    if (g.canvas.selectionHasText()) {
      HMENU f = CreatePopupMenu();
      AppendMenuW(f, MF_STRING, IDM_FMT_BOLD, L"Bold\tCtrl+B");
      AppendMenuW(f, MF_STRING, IDM_FMT_ITALIC, L"Italic\tCtrl+I");
      AppendMenuW(f, MF_STRING, IDM_FMT_UNDERLINE, L"Underline\tCtrl+U");
      AppendMenuW(f, MF_STRING, IDM_FMT_STRIKE, L"Strikethrough\tCtrl+Shift+X");
      AppendMenuW(f, MF_STRING, IDM_FMT_CODE, L"Code (monospace)");
      AppendMenuW(f, MF_STRING, IDM_FMT_LINK, L"Link...\tCtrl+K");
      AppendMenuW(f, MF_STRING, IDM_FMT_COLOR, L"Text Color...");
      AppendMenuW(f, MF_STRING, IDM_FMT_BIGGER, L"Bigger Text\tCtrl+]");
      AppendMenuW(f, MF_STRING, IDM_FMT_SMALLER, L"Smaller Text\tCtrl+[");
      AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(f, MF_STRING, IDM_FMT_CLEAR, L"Clear Formatting");
      sep();
      AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"Text Format");
    }
  } else {
    sep();
    add(IDM_CTX_PASTE, L"Paste Here\tCtrl+V");
    add(IDM_CTX_PASTETEXT, L"Paste as Plain Text Here\tCtrl+Shift+V");
    sep();
    add(IDM_CTX_ADDTEXT, L"Add Text Here");
    add(IDM_CTX_ADDBUTTON, L"Add Button Here");
    add(IDM_CTX_ADDRECT, L"Add Rectangle Here");
    add(IDM_CTX_ADDIMAGE, L"Add Image Here...");
    sep();
    add(IDM_EDIT_SELECTALL, L"Select All\tCtrl+A");
    add(IDM_PAGE_PROPS, L"Page Properties...");
    sep();
    add(IDM_VIEW_EDITMODE, L"Switch to Run Mode\tCtrl+E");
  }
  TrackPopupMenu(m, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.hwnd, nullptr);
  DestroyMenu(m);
}

// Index of the current page in the document's full page order, or -1.
int currentPageIndex() {
  if (!g.doc) return -1;
  const auto& ps = g.doc->pages();
  for (size_t i = 0; i < ps.size(); ++i)
    if (ps[i].id == g.canvas.pageId()) return (int)i;
  return -1;
}

void cmdTagMenu() {
  if (!g.doc) return;
  auto tags = g.doc->allTags();
  gTagNames.clear();
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kTagBase, L"Show All Pages");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (tags.empty()) {
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No tags yet (add them in Page Properties)");
  }
  for (size_t i = 0; i < tags.size() && i < 200; ++i) {
    gTagNames.push_back(tags[i].first);
    AppendMenuW(m, MF_STRING, (UINT_PTR)(kTagBase + 1 + (int)i),
                (L"#" + widen(tags[i].first) + L"   (" + std::to_wstring(tags[i].second) + L")").c_str());
  }
  POINT pt;
  GetCursorPos(&pt);
  TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(m);
}

void cmdMovePage(int delta) {
  int i = currentPageIndex();
  if (i < 0) return;
  g.doc->movePage(g.canvas.pageId(), i + delta);  // no-op at either end
}

// The list shows the real order only when no search filter is active.
bool listIsFullOrder() { return g.doc && g.listIds.size() == g.doc->pages().size(); }

// Drag a page up or down the list to reorder it. Selection under the pointer follows
// the drag so you can see where it will land; Esc or leaving the window cancels.
struct PageDrag { bool down = false, dragging = false; int from = -1; int y0 = 0; } pdrag;

int listItemAt(HWND h, int x, int y) {
  DWORD r = (DWORD)SendMessageW(h, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
  int n = (int)SendMessageW(h, LB_GETCOUNT, 0, 0);
  int i = (int)LOWORD(r);
  return n > 0 ? std::clamp(i, 0, n - 1) : -1;  // outside the last row clamps to the last row
}

LRESULT CALLBACK pageListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  switch (m) {
    case WM_LBUTTONDOWN:
      pdrag = {true, false, listItemAt(h, (short)LOWORD(l), (short)HIWORD(l)), (short)HIWORD(l)};
      break;
    case WM_MOUSEMOVE:
      if (pdrag.down && (w & MK_LBUTTON)) {
        int y = (short)HIWORD(l);
        if (!pdrag.dragging && std::abs(y - pdrag.y0) > S(4) && listIsFullOrder() && pdrag.from >= 0) {
          pdrag.dragging = true;
          SetCapture(h);
        }
        if (pdrag.dragging) {
          g.inRefresh = true;  // moving the highlight must not navigate
          SendMessageW(h, LB_SETCURSEL, (WPARAM)listItemAt(h, (short)LOWORD(l), y), 0);
          g.inRefresh = false;
          SetCursor(LoadCursor(nullptr, IDC_SIZENS));
          return 0;
        }
      }
      break;
    case WM_LBUTTONUP:
      if (pdrag.dragging) {
        int to = listItemAt(h, (short)LOWORD(l), (short)HIWORD(l));
        int from = pdrag.from;
        pdrag = {};
        ReleaseCapture();
        if (to >= 0 && to != from && from < (int)g.listIds.size()) g.doc->movePage(g.listIds[(size_t)from], to);
        refreshPageList();  // also restores the highlight if nothing moved
        return 0;
      }
      pdrag = {};
      break;
    case WM_CAPTURECHANGED:
      if (pdrag.dragging && (HWND)l != h) {  // capture taken by someone else: abandon the drag
        pdrag = {};
        refreshPageList();
      }
      break;
    case WM_KEYDOWN:
      if (w == VK_ESCAPE && pdrag.dragging) {
        pdrag = {};
        ReleaseCapture();
        refreshPageList();
        return 0;
      }
      break;
  }
  return DefSubclassProc(h, m, w, l);
}

void pageContextMenu(POINT screen) {
  if (!g.doc) return;
  POINT cp = screen;
  ScreenToClient(g.pageList, &cp);
  DWORD r = (DWORD)SendMessageW(g.pageList, LB_ITEMFROMPOINT, 0, MAKELPARAM(cp.x, cp.y));
  if (HIWORD(r) == 0 && (int)LOWORD(r) < (int)g.listIds.size()) navigate(g.listIds[LOWORD(r)], true);
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, IDM_PAGE_NEW, L"New Page...");
  AppendMenuW(m, MF_STRING, IDM_PAGE_RENAME, L"Rename...");
  AppendMenuW(m, MF_STRING, IDM_PAGE_PROPS, L"Page Properties...");
  AppendMenuW(m, MF_STRING, IDM_PAGE_TAGS, L"Filter by Tag...");
  AppendMenuW(m, MF_STRING, IDM_PAGE_MOVEUP, L"Move Up\tAlt+Up");
  AppendMenuW(m, MF_STRING, IDM_PAGE_MOVEDOWN, L"Move Down\tAlt+Down");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, IDM_PAGE_DELETE, L"Delete Page");
  TrackPopupMenu(m, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.hwnd, nullptr);
  DestroyMenu(m);
}

LRESULT CALLBACK searchProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (m == WM_KEYDOWN) {
    if (w == VK_ESCAPE) {
      SetWindowTextW(h, L"");
      SetFocus(g.canvas.hwnd());
      return 0;
    }
    if (w == VK_RETURN || w == VK_DOWN) {
      SetFocus(g.pageList);
      return 0;
    }
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
  return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------- main window

bool overSplitter(int x) {
  RECT rc;
  GetClientRect(g.hwnd, &rc);
  int lw = leftWidth(rc);
  return x >= lw && x < lw + S(5);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      g.hwnd = h;
      g.dpi = dpiFor(h);
      HINSTANCE hi = g.hi;
      g.menu = buildMenu();
      SetMenu(h, g.menu);
      g.search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_SEARCH, hi, nullptr);
      SendMessageW(g.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find pages (Ctrl+F)");
      SetWindowSubclass(g.search, searchProc, 1, 0);
      g.pageList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                                   0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_PAGES, hi, nullptr);
      SetWindowSubclass(g.pageList, pageListProc, 2, 0);
      g.linkLabel = CreateWindowExW(0, L"STATIC", L"Linked from this page", WS_CHILD | WS_VISIBLE, 0, 0, 10,
                                    10, h, (HMENU)(INT_PTR)IDC_LINKLABEL, hi, nullptr);
      g.backList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                                   0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_BACKLINKS, hi, nullptr);
      g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                 0, h, (HMENU)(INT_PTR)IDC_STATUS, hi, nullptr);
      createToolbar();
      createFKeys();
      g.canvas.create(h, IDC_CANVAS, hi);
      g.canvas.onContextMenu = [](POINT p) { canvasContextMenu(p); };
      SetTimer(h, 77, 150, nullptr);  // keeps toolbar enable/check state in step with undo, clipboard, etc.
      g.canvas.onNavigate = [](int64_t id) { navigate(id, true); };
      g.canvas.onStateChanged = [] { updateStatus(); };
      g.canvas.onOpenLink = [](const std::wstring& t) { openLink(t); };
      g.canvas.setEditMode(g.startEdit);
      g.canvas.setSnap(g.startSnap);
      rebuildFont();
      layout();
      return 0;
    }
    case WM_SIZE:
      layout();
      return 0;
    case WM_TIMER:
      if (w == 77) syncToolbar();
      return 0;
    case WM_NOTIFY: {
      auto* hdr = (NMHDR*)l;
      if (hdr && hdr->code == TTN_GETDISPINFOW) {
        auto* di = (NMTTDISPINFOW*)l;
        if (const wchar_t* t = tooltipFor((int)hdr->idFrom)) {
          di->lpszText = const_cast<wchar_t*>(t);
          return 0;
        }
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = (MINMAXINFO*)l;
      mm->ptMinTrackSize = {S(640), S(420)};
      return 0;
    }
    case WM_DPICHANGED: {
      g.dpi = HIWORD(w);
      const RECT* r = (const RECT*)l;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      rebuildFont();
      createToolbar();
      rebuildFont();
      g.canvas.onDpiChanged();
      layout();
      return 0;
    }
    case WM_SETFOCUS:
      SetFocus(g.canvas.hwnd());
      return 0;
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(h, &rc);
      FillRect((HDC)w, &rc, GetSysColorBrush(COLOR_BTNFACE));
      return 1;
    }
    case WM_INITMENUPOPUP:
      updateMenuState();
      return 0;
    case WM_COMMAND:
      onCommand(LOWORD(w), HIWORD(w), (HWND)l);
      return 0;
    case WM_CONTEXTMENU:
      if ((HWND)w == g.pageList) {
        POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
        if (l == -1) {  // keyboard-invoked: anchor to the list
          RECT r;
          GetWindowRect(g.pageList, &r);
          pt = {r.left + 20, r.top + 20};
        }
        pageContextMenu(pt);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (overSplitter((short)LOWORD(l))) {
        g.dragSplit = true;
        SetCapture(h);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (g.dragSplit) {
        int x = (short)LOWORD(l);
        g.splitX = std::max(120, MulDiv(x - S(2), 96, g.dpi));
        layout();
      }
      return 0;
    case WM_LBUTTONUP:
      if (g.dragSplit) {
        g.dragSplit = false;
        ReleaseCapture();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        if (g.dragSplit || overSplitter(pt.x)) {
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;
    case WM_CLOSE:
      saveSettings();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      g.canvas.setDocument(nullptr);
      g.doc.reset();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int nShow) {
  g.hi = hi;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (image decoding) needs COM
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  setupIniPath();
  g.startEdit = readIniInt(L"edit", 0) != 0;
  g.startSnap = readIniInt(L"snap", 1) != 0;
  g.splitX = std::clamp(readIniInt(L"split", 250), 120, 800);

  if (!Canvas::registerClass(hi)) return 1;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hi;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
  wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.lpszClassName = kAppClass;
  if (!RegisterClassExW(&wc)) return 1;

  // Restore the previous window rectangle if it is still on a monitor.
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1100, ht = 720;
  if (readIniInt(L"w", 0) > 0) {
    RECT r = {readIniInt(L"x", 0), readIniInt(L"y", 0), 0, 0};
    r.right = r.left + readIniInt(L"w", 1100);
    r.bottom = r.top + readIniInt(L"h", 720);
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
      x = r.left; y = r.top; w = r.right - r.left; ht = r.bottom - r.top;
    }
  }

  g.accel = buildAccel();
  CreateWindowExW(0, kAppClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, ht, nullptr,
                  nullptr, hi, nullptr);
  if (!g.hwnd) return 1;

  // Decide what to open: command line, then last file, then a default one in Documents.
  bool opened = false;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc > 1) {
    std::wstring p = argv[1];
    opened = openDoc(p, !std::filesystem::exists(std::filesystem::path(p)), false);
  }
  if (argv) LocalFree(argv);
  if (!opened) {
    std::wstring last = readIni(L"last");
    if (!last.empty() && std::filesystem::exists(std::filesystem::path(last)))
      opened = openDoc(last, false, false);
  }
  if (!opened) {
    std::wstring dir = knownFolder(CSIDL_PERSONAL) + L"\\Cairn";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring def = dir + L"\\My Cairn.cairn";
    if (std::filesystem::exists(std::filesystem::path(def)) && openDoc(def, false, true)) opened = true;
    else opened = openDoc(uniquePath(dir, L"My Cairn"), true, false);
  }

  ShowWindow(g.hwnd, readIniInt(L"max", 0) ? SW_SHOWMAXIMIZED : nShow);
  UpdateWindow(g.hwnd);
  SetFocus(g.canvas.hwnd());

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // While typing in a text box, Ctrl+Z/A/etc. belong to the text box, not to the page.
    wchar_t cls[16] = {};
    HWND f = GetFocus();
    if (f) GetClassNameW(f, cls, 16);
    bool typing = f && (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RICHEDIT50W") == 0);  // the live text editor
    if (!typing && TranslateAcceleratorW(g.hwnd, g.accel, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g.canvas.setDocument(nullptr);
  g.doc.reset();
  CoUninitialize();
  return (int)msg.wParam;
}

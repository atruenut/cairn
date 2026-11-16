// The page surface: custom-drawn, double-buffered GDI. Hosts a real EDIT child
// only while a text object is being edited (see DEVLOG entry 3 for why).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "document.h"
#include "rich.h"
#include "util.h"

namespace cairn {

using FontOf = std::function<HFONT(unsigned flags, int sizePct)>;

class Canvas {
 public:
  Canvas() = default;
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  static bool registerClass(HINSTANCE hi);
  HWND create(HWND parent, int id, HINSTANCE hi);
  HWND hwnd() const { return hwnd_; }

  void setDocument(Document* d);
  void setPage(int64_t pageId);
  int64_t pageId() const { return pageId_; }
  void docChanged();  // call after any document mutation, including undo/redo
  void onDpiChanged();

  void setEditMode(bool on);
  bool editMode() const { return edit_; }
  void setSnap(bool on);
  bool snap() const { return snap_; }
  void setZoom(double z);
  void zoomStep(int dir);  // +1 / -1 through a fixed ladder of zoom levels
  double zoom() const { return zoom_; }

  // Commands (all no-ops when they don't apply).
  void addObject(ObjType t, int px = -1, int py = -1);  // px,py: client point to place it at
  void deleteSelection();
  void duplicateSelection();
  void selectAll();
  void nudge(int dx, int dy);
  void bringToFront();
  void sendToBack();
  void editProperties();  // selected object, or the page when nothing is selected
  void beginTextEdit();
  void setMarked(bool m) { marked_ = m; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
  bool beginCellEdit(const Object& o, size_t row, size_t col);  // type straight into one table cell
  void cellNavigate(int dir);                                   // Tab / Shift+Tab inside a cell editor
  bool contextOnTable() const;                                  // is the right-click point on a table cell?
  void tableOp(int op);  // 0/1 row above/below, 2 delete row, 3/4 column left/right, 5 delete column
  void editPageProperties();

  // Media and clipboard.
  bool addImageBytes(const std::string& bytes, const std::string& name, int px = -1, int py = -1);
  void addFilesAt(const std::vector<std::wstring>& files, int px = -1, int py = -1);
  void copySelection(bool cut);
  void paste(bool plainOnly = false, int px = -1, int py = -1);  // plainOnly: ignore rich (HTML) clipboard text
  // Rich text formatting. While a text box is being edited it applies to the selected
  // characters; otherwise to all of every selected text object.
  enum FormatCmd { FmtBold = 1, FmtItalic, FmtUnderline, FmtStrike, FmtLink, FmtColor, FmtClear, FmtCode };
  void applyFormat(int cmd);
  // Text size: dir > 0 bigger, < 0 smaller. While editing it resizes the selected characters
  // (a percentage of the box's size); otherwise it resizes the font of every selected text
  // object and button.
  void changeTextSize(int dir);
  bool selectionHasSizable() const;
  unsigned editStyleFlags();  // style bits in effect at the editor's caret/selection (0 if not editing)
  void recordEdit(bool typing);               // note the editor's state for its undo history
  void editUndoRedo(bool redo);
  void applyEditStyling();                    // paint editRuns_ onto the live editor
  bool editSelBytes(size_t& a, size_t& b);    // the editor's selection as UTF-8 byte offsets  // any selected Text or Button

  size_t selectionCount() const { return sel_.size(); }
  bool selectionHasText() const;  // any selected Text object (for the Format menu)
  // Right-click support: where it happened (client px) and the web link under it, if any.
  POINT contextPoint() const { return ctx_; }
  std::wstring contextLink() const { return ctxLink_; }
  std::function<void(POINT screen)> onContextMenu;
  std::wstring selectionSummary() const;

  std::function<void(int64_t)> onNavigate;  // a button was clicked in Run mode
  std::function<void(const std::wstring&)> onOpenLink;  // a button with a file/URL was clicked
  std::function<void()> onActivate;         // this pane was clicked or got focus (split view)
  std::function<void()> onStateChanged;     // selection, zoom, mode or snap changed

 private:
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT proc(UINT, WPARAM, LPARAM);

  // geometry
  double scale() const { return zoom_ * dpi_ / 96.0; }
  const Page* curPage() const { return doc_ ? doc_->page(pageId_) : nullptr; }
  POINT origin() const;
  RECT objRectPx(const Object& o) const;
  std::wstring urlAt(const Object& o, int x, int y);  // link under a client point, or empty
  POINT toPagePt(int px, int py) const;
  int snapv(int v) const;
  const Object& eff(const Object& o) const;  // object with any in-flight drag applied

  // painting
  void paint(HDC dc);
  void drawObject(HDC dc, const Object& o, bool pressed);
  void drawSelection(HDC dc);
  HFONT fontFor(int pt, bool bold);
  HFONT fontStyled(int pt, unsigned flags);
  FontOf richFont(const Object& o);
  struct FormatArg {
    bool set = true;
    std::string url;
    int color = -1;
  };
  bool askFormat(int cmd, const Runs& runs, size_t a, size_t b, FormatArg& arg);
  static void applyFormatArg(Runs& runs, size_t textLen, size_t a, size_t b, int cmd, const FormatArg& arg);
  void syncEditRuns();
  std::wstring editText() const;
  void pasteRichIntoEditor();
  int measureRichHeight(const std::string& utf8, const std::string& runsEnc, int pt, int widthUnits);
  void clearFonts();

  // hit testing
  const Object* objectAt(int px, int py) const;
  int handleAt(int px, int py) const;  // -1 if none
  bool isSelected(int64_t id) const;
  void pruneSelection();
  void notify();

  // scrolling
  void updateScroll();
  void setScroll(int bar, int pos);
  void scrollBy(int dx, int dy);

  // mouse / keyboard
  void onLButtonDown(int x, int y, WPARAM keys);
  void onMouseMove(int x, int y, WPARAM keys);
  void onLButtonUp(int x, int y);
  void onDoubleClick(int x, int y);
  bool onKeyDown(WPARAM vk);
  void cancelDrag();

  // inline editing
  void endEdit(bool commit, bool refocus);

  // media helpers
  struct ImgEntry {
    HBITMAP src = nullptr;
    int w = 0, h = 0;
    HBITMAP scaled = nullptr;  // cache of src resampled to the last drawn size
    int sw = 0, sh = 0;
    int sframe = -1;                 // which animation frame `scaled` was made from
    std::vector<HBITMAP> frames;     // animated GIF: composed frames (src == frames[cur])
    std::vector<int> delays;         // ms per frame
    int cur = 0;
    DWORD next = 0;                  // tick at which to advance
    unsigned long long drawnPaint = 0;  // paint number that last drew it (visibility test)
  };
  void animTick();
  ImgEntry* imageFor(int64_t blobId);
  void clearImages();
  void drawImage(HDC dc, const Object& o, const RECT& r);
  void drawFolder(HDC dc, const Object& o, const RECT& r);
  void drawTable(HDC dc, const Object& o, const RECT& r);
  void drawAudio(HDC dc, const Object& o, const RECT& r);
  bool audioStart(const Object& o);   // writes the clip to a temp file and plays it; false if it cannot
  void audioStop();
  void audioNotify(WPARAM w);         // MM_MCINOTIFY: the clip finished (loop it) or was stopped
  void audioAutoplay();               // Run mode: start the first "play when the page opens" clip
  bool addAudioBytes(const std::string& bytes, const std::string& name, int px, int py);
  bool marked_ = false;
  // Sound is per process (one MCI device alias), so its state is shared by every pane.
  static inline int64_t audioObj_ = 0;
  static inline bool audioLoop_ = false;
  static inline DWORD audioTick_ = 0;  // when the current pass started: a clip that "ends" at once must not loop forever
  static inline std::wstring audioDir_;
  static inline std::vector<Canvas*> all_;  // every live pane, so a playing clip can repaint all of them
  std::string withWikiRuns(const Object& o) const;
  struct FolderEntry { std::wstring name; bool dir; };
  struct FolderCache { std::wstring path; DWORD tick = 0; std::vector<FolderEntry> items; bool ok = false; };
  const FolderCache& folderItems(const Object& o);
  std::wstring folderPath(const Object& o) const;
  int folderRowAt(const Object& o, int px, int py) const;  // entry index under a client point, or -1
  std::map<int64_t, FolderCache> folderCache_;
  std::map<int64_t, int> folderScroll_;
  void placeNew(int w, int h, int px, int py, int& x, int& y);
  int64_t addTextObject(const std::string& utf8, int px, int py, const std::string& runsEnc = "");
  int measureTextHeight(const std::string& utf8, int pt, int widthUnits);
  LRESULT procInner(UINT, WPARAM, LPARAM);

  HWND hwnd_ = nullptr;
  HINSTANCE hi_ = nullptr;
  Document* doc_ = nullptr;
  int64_t pageId_ = 0;
  bool edit_ = true;
  bool snap_ = true;
  double zoom_ = 1.0;
  int dpi_ = 96;
  int scrollX_ = 0, scrollY_ = 0;
  bool inScroll_ = false;

  std::vector<int64_t> sel_;
  std::map<std::pair<int, int>, HFONT> fonts_;
  std::map<int64_t, ImgEntry> images_;
  unsigned long long paintCount_ = 0;
  bool animTimer_ = false;
  int pasteCount_ = 0;
  bool cutPending_ = false;
  DWORD clipSeq_ = 0;

  enum class Drag { None, Pending, Move, Resize, Marquee, Press };
  Drag drag_ = Drag::None;
  POINT dragStart_{0, 0};
  int dragHandle_ = -1;
  bool dragShift_ = false;
  std::vector<Object> dragOrig_;
  std::map<int64_t, Object> dragNow_;
  RECT marquee_{0, 0, 0, 0};
  std::vector<int64_t> marqueeBase_;  // selection to extend when Shift-marqueeing
  int64_t pressed_ = 0;               // Run mode: button currently held down
  bool pressInside_ = false;          // ...and whether the cursor is still over it

  POINT ctx_{-1, -1};
  std::wstring ctxLink_;
  HWND editWnd_ = nullptr;
  Runs editRuns_;           // formatting of the text being edited, kept in step with the edit box
  std::string editSynced_;  // the text editRuns_ currently describes
  bool editIsText_ = false, inFormat_ = false;
  bool editRich_ = false, styling_ = false;  // editing in a RichEdit that mirrors editRuns_
  // The live editor has its own undo (RichEdit's native one would record our restyling passes).
  struct EditSnap { std::string text, runs; size_t caret = 0; };
  std::vector<EditSnap> editUndo_, editRedo_;
  EditSnap editCur_;
  unsigned long lastEditTick_ = 0;
  bool lastTyping_ = false, restoring_ = false;
  // "Typing style": bold/italic/... toggled with nothing selected apply to the next typed text.
  bool pendActive_ = false;
  size_t pendCaret_ = 0;
  unsigned pendFlags_ = 0;
  int64_t editId_ = 0;
  int editRow_ = -1, editCol_ = -1;  // >= 0 while a table cell is being edited
};

}  // namespace cairn

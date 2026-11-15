#include "propform.h"

#include <commdlg.h>

#include <algorithm>
#include <cstdlib>

namespace cairn {

HFONT PropForm::font_ = nullptr;

namespace {
const wchar_t* kFormClass = L"CairnPropForm";
constexpr int kIdColorBase = 1000;  // swatch buttons: base + field index
constexpr int kIdClearBase = 2000;  // "None" buttons
constexpr int kIdCtlBase = 3000;    // data controls: base + field index

bool ensureClass() {
  static bool done = false;
  if (done) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = &PropForm::wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kFormClass;
  done = RegisterClassExW(&wc) != 0;
  return done;
}
}  // namespace

int PropForm::addText(const std::wstring& label, const std::wstring& v, bool multiline) {
  Field f;
  f.kind = KText; f.label = label; f.text = v; f.multiline = multiline;
  f_.push_back(f);
  return (int)f_.size() - 1;
}
int PropForm::addNumber(const std::wstring& label, int v, int mn, int mx) {
  Field f;
  f.kind = KNumber; f.label = label; f.num = v; f.mn = mn; f.mx = mx;
  f_.push_back(f);
  return (int)f_.size() - 1;
}
int PropForm::addCombo(const std::wstring& label, const std::vector<std::wstring>& items, int sel) {
  Field f;
  f.kind = KCombo; f.label = label; f.items = items; f.sel = sel;
  f_.push_back(f);
  return (int)f_.size() - 1;
}
int PropForm::addCheck(const std::wstring& label, bool v) {
  Field f;
  f.kind = KCheck; f.label = label; f.chk = v;
  f_.push_back(f);
  return (int)f_.size() - 1;
}
int PropForm::addColor(const std::wstring& label, int rgbOrNone, bool allowNone) {
  Field f;
  f.kind = KColor; f.label = label; f.color = rgbOrNone; f.allowNone = allowNone;
  f_.push_back(f);
  return (int)f_.size() - 1;
}

LRESULT CALLBACK PropForm::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  PropForm* self = (PropForm*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) {
    self = (PropForm*)((CREATESTRUCTW*)l)->lpCreateParams;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    if (self) self->hwnd_ = h;
  }
  return self ? self->proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

void PropForm::build() {
  auto S = [&](int v) { return MulDiv(v, dpi_, 96); };
  int y = S(14);
  const int labelX = S(14), labelW = S(130), ctlX = S(150), ctlW = S(290);
  HINSTANCE hi = GetModuleHandleW(nullptr);
  auto setFont = [&](HWND c) {
    if (font_) SendMessageW(c, WM_SETFONT, (WPARAM)font_, TRUE);
  };

  for (size_t i = 0; i < f_.size(); ++i) {
    Field& f = f_[i];
    int rowH = (f.kind == KText && f.multiline) ? S(96) : S(26);
    if (f.kind != KCheck) {
      HWND lab = CreateWindowExW(0, L"STATIC", f.label.c_str(), WS_CHILD | WS_VISIBLE, labelX,
                                 y + S(3), labelW, S(20), hwnd_, nullptr, hi, nullptr);
      setFont(lab);
    }
    int id = kIdCtlBase + (int)i;
    switch (f.kind) {
      case KText: {
        DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (f.multiline) st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
        f.ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", f.text.c_str(), st, ctlX, y, ctlW,
                                rowH - S(2), hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        break;
      }
      case KNumber:
        f.ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(f.num).c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, ctlX, y,
                                S(100), rowH - S(2), hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        break;
      case KCombo: {
        f.ctl = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                ctlX, y, ctlW, S(200), hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        for (const auto& it : f.items) SendMessageW(f.ctl, CB_ADDSTRING, 0, (LPARAM)it.c_str());
        SendMessageW(f.ctl, CB_SETCURSEL, (WPARAM)f.sel, 0);
        break;
      }
      case KCheck:
        f.ctl = CreateWindowExW(0, L"BUTTON", f.label.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, ctlX, y,
                                ctlW, rowH - S(2), hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(f.ctl, BM_SETCHECK, f.chk ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
      case KColor:
        f.ctl = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                ctlX, y, S(110), rowH - S(2), hwnd_,
                                (HMENU)(INT_PTR)(kIdColorBase + (int)i), hi, nullptr);
        if (f.allowNone) {
          HWND none = CreateWindowExW(0, L"BUTTON", L"None", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      ctlX + S(118), y, S(70), rowH - S(2), hwnd_,
                                      (HMENU)(INT_PTR)(kIdClearBase + (int)i), hi, nullptr);
          setFont(none);
        }
        break;
    }
    setFont(f.ctl);
    y += rowH + S(6);
  }

  y += S(10);
  int bw = S(88), bh = S(28), right = S(14) + labelW + ctlW + S(6) + S(8);
  HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            right - 2 * bw - S(8), y, bw, bh, hwnd_, (HMENU)(INT_PTR)IDOK, hi, nullptr);
  HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                right - bw, y, bw, bh, hwnd_, (HMENU)(INT_PTR)IDCANCEL, hi, nullptr);
  setFont(ok);
  setFont(cancel);
}

void PropForm::collect() {
  for (Field& f : f_) {
    switch (f.kind) {
      case KText: {
        int n = GetWindowTextLengthW(f.ctl);
        std::wstring w((size_t)n + 1, L'\0');
        GetWindowTextW(f.ctl, &w[0], n + 1);
        w.resize((size_t)n);
        f.text = w;
        break;
      }
      case KNumber: {
        wchar_t buf[32] = {};
        GetWindowTextW(f.ctl, buf, 31);
        wchar_t* end = nullptr;
        long v = wcstol(buf, &end, 10);
        if (end != buf) f.num = (int)std::clamp<long>(v, f.mn, f.mx);  // junk keeps the old value
        break;
      }
      case KCombo: f.sel = (int)SendMessageW(f.ctl, CB_GETCURSEL, 0, 0); break;
      case KCheck: f.chk = SendMessageW(f.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED; break;
      case KColor: break;  // already kept up to date by the swatch handlers
    }
  }
}

LRESULT PropForm::proc(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_COMMAND: {
      int id = LOWORD(w);
      if (id == IDOK) {
        collect();
        ok_ = done_ = true;
        return 0;
      }
      if (id == IDCANCEL) {
        done_ = true;
        return 0;
      }
      if (id >= kIdColorBase && id < kIdColorBase + 100) {
        Field& f = f_[(size_t)(id - kIdColorBase)];
        static COLORREF custom[16];
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = hwnd_;
        cc.rgbResult = toColorRef(f.color >= 0 ? f.color : 0xFFFFFF);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (ChooseColorW(&cc)) {
          f.color = fromColorRef(cc.rgbResult);
          InvalidateRect(f.ctl, nullptr, TRUE);
        }
        return 0;
      }
      if (id >= kIdClearBase && id < kIdClearBase + 100) {
        Field& f = f_[(size_t)(id - kIdClearBase)];
        f.color = -1;
        InvalidateRect(f.ctl, nullptr, TRUE);
        return 0;
      }
      break;
    }
    case WM_DRAWITEM: {
      auto* di = (DRAWITEMSTRUCT*)l;
      int idx = (int)di->CtlID - kIdColorBase;
      if (idx < 0 || idx >= (int)f_.size()) break;
      const Field& f = f_[(size_t)idx];
      RECT r = di->rcItem;
      FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
      DrawEdge(di->hDC, &r, (di->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
      RECT sw = r;
      InflateRect(&sw, -5, -5);
      if (f.color >= 0) {
        HBRUSH b = CreateSolidBrush(toColorRef(f.color));
        FillRect(di->hDC, &sw, b);
        DeleteObject(b);
        FrameRect(di->hDC, &sw, (HBRUSH)GetStockObject(BLACK_BRUSH));
      } else {
        SetBkMode(di->hDC, TRANSPARENT);
        SelectObject(di->hDC, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(di->hDC, L"(none)", -1, &sw, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      if (di->itemState & ODS_FOCUS) {
        RECT fr = r;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(di->hDC, &fr);
      }
      return TRUE;
    }
    case WM_CLOSE:
      done_ = true;
      return 0;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

bool PropForm::run() {
  if (!ensureClass()) return false;
  dpi_ = dpiFor(owner_);
  auto S = [&](int v) { return MulDiv(v, dpi_, 96); };

  int h = S(14);
  for (const Field& f : f_) h += ((f.kind == KText && f.multiline) ? S(96) : S(26)) + S(6);
  h += S(10) + S(28) + S(14);
  int clientW = S(14) + S(130) + S(290) + S(20);

  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  DWORD ex = WS_EX_DLGMODALFRAME;
  RECT rc = {0, 0, clientW, h};
  AdjustWindowRectExForDpi(&rc, style, FALSE, ex, (UINT)dpi_);

  RECT orc{};
  GetWindowRect(owner_ ? owner_ : GetDesktopWindow(), &orc);
  int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  int x = orc.left + ((orc.right - orc.left) - ww) / 2;
  int y = orc.top + ((orc.bottom - orc.top) - wh) / 3;

  hwnd_ = CreateWindowExW(ex, kFormClass, title_.c_str(), style, x, y, ww, wh, owner_, nullptr,
                          GetModuleHandleW(nullptr), this);
  if (!hwnd_) return false;
  build();

  // First editable control gets focus.
  for (const Field& f : f_)
    if (f.kind != KColor && f.ctl) {
      SetFocus(f.ctl);
      if (f.kind == KText || f.kind == KNumber) SendMessageW(f.ctl, EM_SETSEL, 0, -1);
      break;
    }

  if (owner_) EnableWindow(owner_, FALSE);
  ShowWindow(hwnd_, SW_SHOW);

  MSG msg;
  while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd_, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (!done_) PostQuitMessage(0);  // WM_QUIT arrived mid-dialog: let the app see it too

  if (owner_) EnableWindow(owner_, TRUE);
  DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  if (owner_) {
    SetForegroundWindow(owner_);
    UpdateWindow(owner_);  // repaint now, before the caller starts slow work (disk commit)
  }
  return ok_;
}

}  // namespace cairn

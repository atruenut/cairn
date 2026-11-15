// A tiny modal "form" built at runtime: label + control rows, OK/Cancel.
// Used for object properties, page properties and simple text prompts, so
// there is no dialog-template resource to keep in sync with code.
#pragma once

#include <string>
#include <vector>

#include "util.h"

namespace cairn {

class PropForm {
 public:
  PropForm(HWND owner, const std::wstring& title) : owner_(owner), title_(title) {}

  int addText(const std::wstring& label, const std::wstring& v, bool multiline = false);
  int addNumber(const std::wstring& label, int v, int mn, int mx);
  int addCombo(const std::wstring& label, const std::vector<std::wstring>& items, int sel);
  int addCheck(const std::wstring& label, bool v);
  int addColor(const std::wstring& label, int rgbOrNone, bool allowNone);

  bool run();  // true if the user pressed OK

  std::wstring text(int i) const { return f_[(size_t)i].text; }
  int number(int i) const { return f_[(size_t)i].num; }
  int combo(int i) const { return f_[(size_t)i].sel; }
  bool check(int i) const { return f_[(size_t)i].chk; }
  int color(int i) const { return f_[(size_t)i].color; }  // -1 = none

  static void setFont(HFONT f) { font_ = f; }
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);  // window-class entry point

 private:
  enum Kind { KText, KNumber, KCombo, KCheck, KColor };
  struct Field {
    Kind kind;
    std::wstring label, text;
    int num = 0, mn = 0, mx = 0, sel = 0, color = 0;
    bool chk = false, allowNone = false, multiline = false;
    std::vector<std::wstring> items;
    HWND ctl = nullptr;
  };

  LRESULT proc(UINT, WPARAM, LPARAM);
  void build();
  void collect();

  HWND owner_, hwnd_ = nullptr;
  std::wstring title_;
  std::vector<Field> f_;
  bool done_ = false, ok_ = false;
  int dpi_ = 96;
  static HFONT font_;
};

}  // namespace cairn

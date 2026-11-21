// Cairn's scripting language: a small Scheme-style Lisp. It is closed in the same way the old line language
// was: no files, no network, no way to run a program. Everything that touches the outside world goes through a
// ScriptHost (the application decides what is allowed), and every run has a step budget so a mistake such as an
// endless loop stops with a message instead of hanging the window.
//
// The same language is implemented a second time in JavaScript (src/core/lisp_js.cpp) so exported web pages
// behave like the app; tests/lisp_cases.txt is run against both.
#pragma once

#include <memory>
#include <string>

#include "script.h"

namespace cairn {

// True when `source` is written in the Lisp language: its first real token is "(" (comment lines are skipped).
// Anything else is the older line-based command language.
bool looksLikeLisp(const std::string& source);

class Lisp {
 public:
  Lisp();
  ~Lisp();
  Lisp(const Lisp&) = delete;
  Lisp& operator=(const Lisp&) = delete;

  // Syntax check only (balanced brackets, strings closed); nothing runs.
  static ScriptResult check(const std::string& source);

  // Evaluates every form in `source`. Definitions persist in this session between runs.
  ScriptResult run(const std::string& source, ScriptHost& host);
  // Runs the timers that are due at `nowMs` (the session clock, in ms). Call regularly while a page is shown.
  ScriptResult tick(long nowMs, ScriptHost& host);

  void setClock(long ms);
  long clock() const;
  bool hasTimers() const;
  long nextDue() const;  // clock time of the earliest timer; -1 when there is none
  void cancelTimers();
  void reset();  // forgets all definitions and timers
  void seed(unsigned s);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

// Runs a program and returns what it said, one message per line (test and tooling helper; no page, no audio).
std::string lispRunCapture(const std::string& source, std::string* error = nullptr);

}  // namespace cairn

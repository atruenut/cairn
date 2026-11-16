// Button scripts: a tiny, closed command language. It has no loops, no files, no network and no way to run
// anything: every effect goes through a ScriptHost, which is the application deciding what is allowed.
// A script is one statement per line; "#" starts a comment.
//
//   goto "Page title"           goto next | prev | first | last          back
//   set n = n + 1               (numbers and "strings"; an unset variable is 0; + joins strings)
//   text "Object" = "Clicked " + n          (set the text of the text object with that script name)
//   play "Clip"                 stop
//   open "https://example.org"  (only what a link button could open)
//   say "hello"                 (a message box)
//   if n >= 3 then say "Three!"             (one statement after "then"; = != < > <= >=)
//
// Termination is guaranteed: a script runs each line at most once.
#pragma once

#include <map>
#include <string>

namespace cairn {

constexpr size_t kScriptMaxLines = 200, kScriptMaxLine = 2000, kScriptMaxString = 10000;

struct ScriptHost {
  virtual ~ScriptHost() = default;
  virtual bool gotoPage(const std::string& title) = 0;  // false: no such page
  virtual bool gotoRelative(const std::string& which) = 0;  // "next" "prev" "first" "last"
  virtual bool goBack() = 0;
  virtual bool setText(const std::string& name, const std::string& text) = 0;  // false: no such text object
  virtual bool play(const std::string& name) = 0;                              // false: no such clip
  virtual void stop() = 0;
  virtual bool open(const std::string& target) = 0;
  virtual void say(const std::string& message) = 0;
};

struct ScriptValue {
  bool isStr = false;
  double n = 0;
  std::string s;
};
using ScriptVars = std::map<std::string, ScriptValue>;  // names are lower-case
struct ScriptResult {
  bool ok = true;
  int line = 0;        // 1-based, when !ok
  std::string error;   // when !ok
};

// Syntax check only; nothing runs. Use when a script is edited.
ScriptResult checkScript(const std::string& source);
// Parses, then runs line by line, stopping at the first error. `vars` persists between runs.
ScriptResult runScript(const std::string& source, ScriptVars& vars, ScriptHost& host);

}  // namespace cairn

#include "script.h"

#include "lisp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace cairn {
namespace {

enum class Tok { End, Ident, Num, Str, Op };
struct Token {
  Tok kind = Tok::End;
  std::string text;  // identifier (lower-cased), string contents, or operator
  double num = 0;
};

struct ParseError {
  std::string msg;
};

// ------------------------------------------------------------------ lexer
std::vector<Token> lex(const std::string& line) {
  std::vector<Token> out;
  size_t i = 0;
  while (i < line.size()) {
    char c = line[i];
    if (c == ' ' || c == '\t') { ++i; continue; }
    if (c == '#') break;
    Token t;
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t j = i;
      while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_')) ++j;
      t.kind = Tok::Ident;
      for (size_t k = i; k < j; ++k) t.text += (char)std::tolower((unsigned char)line[k]);
      i = j;
    } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < line.size() && std::isdigit((unsigned char)line[i + 1]))) {
      size_t j = i;
      bool dot = false;
      while (j < line.size() && (std::isdigit((unsigned char)line[j]) || (line[j] == '.' && !dot))) {
        if (line[j] == '.') dot = true;
        ++j;
      }
      if (j - i > 15) throw ParseError{"number too long"};
      t.kind = Tok::Num;
      t.num = std::strtod(line.substr(i, j - i).c_str(), nullptr);
      i = j;
    } else if (c == '"') {
      ++i;
      bool closed = false;
      while (i < line.size()) {
        if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
          t.text += line[i + 1];
          i += 2;
        } else if (line[i] == '"') {
          closed = true;
          ++i;
          break;
        } else {
          t.text += line[i++];
        }
        if (t.text.size() > kScriptMaxString) throw ParseError{"string too long"};
      }
      if (!closed) throw ParseError{"unterminated string"};
      t.kind = Tok::Str;
    } else {
      std::string op(1, c);
      if ((c == '<' || c == '>' || c == '!' || c == '=') && i + 1 < line.size() && line[i + 1] == '=') op += '=', ++i;
      else if (c == '<' && i + 1 < line.size() && line[i + 1] == '>') op += '>', ++i;
      static const std::string ok = "+-()=<>!";
      if (ok.find(c) == std::string::npos || op == "!") throw ParseError{std::string("unexpected '") + c + "'"};
      t.kind = Tok::Op;
      t.text = op;
      ++i;
    }
    out.push_back(std::move(t));
  }
  return out;
}

// ------------------------------------------------------------------ AST
struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
  enum Kind { Num, Str, Var, Add, Sub, Neg } kind = Num;
  double num = 0;
  std::string str;
  ExprP a, b;
};

struct Stmt;
using StmtP = std::shared_ptr<Stmt>;
struct Stmt {
  enum Kind { Goto, GotoRel, Back, Set, Text, Play, Stop, Open, Say, If } kind = Say;
  std::string name;  // variable / relative keyword
  ExprP e1, e2;      // goto/play/open/say: e1; set: e1 = value; text: e1 = object name, e2 = value; if: e1 op e2
  std::string op;    // if: comparison ("" = truthiness of e1)
  StmtP then;
};

struct Parser {
  const std::vector<Token>& t;
  size_t i = 0;
  explicit Parser(const std::vector<Token>& tokens) : t(tokens) {}

  const Token* peek() const { return i < t.size() ? &t[i] : nullptr; }
  bool isOp(const char* o) const { return peek() && peek()->kind == Tok::Op && peek()->text == o; }
  bool isWord(const char* w) const { return peek() && peek()->kind == Tok::Ident && peek()->text == w; }

  ExprP primary(int depth) {
    if (depth > 20) throw ParseError{"expression too deeply nested"};
    if (!peek()) throw ParseError{"expected a value"};
    auto e = std::make_shared<Expr>();
    const Token& k = *peek();
    if (k.kind == Tok::Num) { e->kind = Expr::Num; e->num = k.num; ++i; return e; }
    if (k.kind == Tok::Str) { e->kind = Expr::Str; e->str = k.text; ++i; return e; }
    if (k.kind == Tok::Ident) { e->kind = Expr::Var; e->str = k.text; ++i; return e; }
    if (isOp("-")) { ++i; e->kind = Expr::Neg; e->a = primary(depth + 1); return e; }
    if (isOp("(")) {
      ++i;
      ExprP inner = expr(depth + 1);
      if (!isOp(")")) throw ParseError{"missing )"};
      ++i;
      return inner;
    }
    throw ParseError{"expected a value"};
  }

  ExprP expr(int depth = 0) {
    ExprP l = primary(depth);
    while (isOp("+") || isOp("-")) {
      auto e = std::make_shared<Expr>();
      e->kind = isOp("+") ? Expr::Add : Expr::Sub;
      ++i;
      e->a = l;
      e->b = primary(depth);
      l = e;
    }
    return l;
  }

  StmtP statement(bool allowIf) {
    if (!peek() || peek()->kind != Tok::Ident) throw ParseError{"a command is expected here"};
    std::string w = peek()->text;
    ++i;
    auto s = std::make_shared<Stmt>();
    if (w == "goto") {
      if (isWord("next") || isWord("prev") || isWord("first") || isWord("last")) {
        s->kind = Stmt::GotoRel;
        s->name = peek()->text;
        ++i;
      } else {
        s->kind = Stmt::Goto;
        s->e1 = expr();
      }
    } else if (w == "back") {
      s->kind = Stmt::Back;
    } else if (w == "stop") {
      s->kind = Stmt::Stop;
    } else if (w == "set") {
      if (!peek() || peek()->kind != Tok::Ident) throw ParseError{"set needs a variable name"};
      s->kind = Stmt::Set;
      s->name = peek()->text;
      ++i;
      if (!isOp("=")) throw ParseError{"set needs ="};
      ++i;
      s->e1 = expr();
    } else if (w == "text") {
      s->kind = Stmt::Text;
      s->e1 = expr();
      if (!isOp("=")) throw ParseError{"text needs =, as in: text \"Name\" = \"value\""};
      ++i;
      s->e2 = expr();
    } else if (w == "play") {
      s->kind = Stmt::Play;
      s->e1 = expr();
    } else if (w == "open") {
      s->kind = Stmt::Open;
      s->e1 = expr();
    } else if (w == "say") {
      s->kind = Stmt::Say;
      s->e1 = expr();
    } else if (w == "if") {
      if (!allowIf) throw ParseError{"if cannot be nested inside another if"};
      s->kind = Stmt::If;
      s->e1 = expr();
      static const char* ops[] = {"==", "=", "!=", "<>", "<=", ">=", "<", ">"};
      for (const char* o : ops)
        if (isOp(o)) {
          s->op = o;
          ++i;
          s->e2 = expr();
          break;
        }
      if (!isWord("then")) throw ParseError{"if needs then"};
      ++i;
      s->then = statement(false);
    } else {
      throw ParseError{"unknown command \"" + w + "\""};
    }
    if (!allowIf) return s;  // the caller checks the tail
    if (peek()) throw ParseError{"unexpected text after the command"};
    return s;
  }
};

struct Program {
  std::vector<std::pair<int, StmtP>> stmts;  // (source line, statement)
};

// Returns an error result, or ok with the parsed program.
ScriptResult parse(const std::string& src, Program& prog) {
  size_t pos = 0;
  int lineNo = 0;
  size_t count = 0;
  while (pos <= src.size()) {
    size_t nl = src.find('\n', pos);
    std::string line = src.substr(pos, (nl == std::string::npos ? src.size() : nl) - pos);
    pos = nl == std::string::npos ? src.size() + 1 : nl + 1;
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    try {
      if (line.size() > kScriptMaxLine) throw ParseError{"line too long"};
      std::vector<Token> toks = lex(line);
      if (toks.empty()) continue;
      if (++count > kScriptMaxLines) throw ParseError{"too many lines (limit 200)"};
      Parser p(toks);
      StmtP s = p.statement(true);
      // a statement after "then" is parsed with allowIf=false, which skips the tail check: do it here
      if (p.peek()) throw ParseError{"unexpected text after the command"};
      prog.stmts.emplace_back(lineNo, s);
    } catch (const ParseError& e) {
      return {false, lineNo, e.msg};
    }
  }
  return {};
}

// ------------------------------------------------------------------ evaluation
std::string numText(double n) {
  if (std::isnan(n) || std::isinf(n)) return "0";
  char buf[64];
  if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", n);
  else std::snprintf(buf, sizeof buf, "%g", n);
  return buf;
}

std::string show(const ScriptValue& v) { return v.isStr ? v.s : numText(v.n); }

struct Eval {
  ScriptVars& vars;
  ScriptValue run(const ExprP& e) {
    ScriptValue v;
    switch (e->kind) {
      case Expr::Num: v.n = e->num; break;
      case Expr::Str: v.isStr = true; v.s = e->str; break;
      case Expr::Var: {
        auto it = vars.find(e->str);
        if (it != vars.end()) v = it->second;  // unset: 0
        break;
      }
      case Expr::Neg: {
        v = run(e->a);
        if (v.isStr) throw ParseError{"can't negate text"};
        v.n = -v.n;
        break;
      }
      case Expr::Add: {
        ScriptValue a = run(e->a), b = run(e->b);
        if (a.isStr || b.isStr) {
          v.isStr = true;
          v.s = show(a) + show(b);
          if (v.s.size() > kScriptMaxString) v.s.resize(kScriptMaxString);
        } else {
          v.n = a.n + b.n;
        }
        break;
      }
      case Expr::Sub: {
        ScriptValue a = run(e->a), b = run(e->b);
        if (a.isStr || b.isStr) throw ParseError{"can't subtract text"};
        v.n = a.n - b.n;
        break;
      }
    }
    return v;
  }
};

bool truthy(const ScriptValue& v) { return v.isStr ? !v.s.empty() : v.n != 0; }

bool compare(const ScriptValue& a, const ScriptValue& b, const std::string& op) {
  int c;
  if (!a.isStr && !b.isStr) c = a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
  else c = show(a).compare(show(b)) < 0 ? -1 : show(a) == show(b) ? 0 : 1;
  if (op == "==" || op == "=") return c == 0;
  if (op == "!=" || op == "<>") return c != 0;
  if (op == "<") return c < 0;
  if (op == ">") return c > 0;
  if (op == "<=") return c <= 0;
  return c >= 0;
}

// Runs one statement; returns an error message, or "" on success.
std::string exec(const Stmt& s, ScriptVars& vars, ScriptHost& host) {
  Eval ev{vars};
  switch (s.kind) {
    case Stmt::Goto: {
      std::string t = show(ev.run(s.e1));
      return host.gotoPage(t) ? "" : "no page called \"" + t + "\"";
    }
    case Stmt::GotoRel: return host.gotoRelative(s.name) ? "" : "there is no " + s.name + " page";
    case Stmt::Back: return host.goBack() ? "" : "nothing to go back to";
    case Stmt::Stop: host.stop(); return "";
    case Stmt::Set: vars[s.name] = ev.run(s.e1); return "";
    case Stmt::Text: {
      std::string n = show(ev.run(s.e1)), v = show(ev.run(s.e2));
      return host.setText(n, v) ? "" : "no text object named \"" + n + "\" on this page";
    }
    case Stmt::Play: {
      std::string n = show(ev.run(s.e1));
      return host.play(n) ? "" : "no clip named \"" + n + "\" on this page";
    }
    case Stmt::Open: {
      std::string t = show(ev.run(s.e1));
      return host.open(t) ? "" : "can't open \"" + t + "\"";
    }
    case Stmt::Say: host.say(show(ev.run(s.e1))); return "";
    case Stmt::If: {
      ScriptValue a = ev.run(s.e1);
      bool yes = s.op.empty() ? truthy(a) : compare(a, ev.run(s.e2), s.op);
      return yes ? exec(*s.then, vars, host) : "";
    }
  }
  return "";
}

}  // namespace

// ------------------------------------------------------------------ conversion to Lisp
namespace {
std::string lispStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o + "\"";
}

std::string lispNum(double d) {
  char b[64];
  if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(b, sizeof b, "%.0f", d);
  else std::snprintf(b, sizeof b, "%.17g", d);
  return b;
}

std::string toLispExpr(const ExprP& e, std::vector<std::string>& vars) {
  switch (e->kind) {
    case Expr::Num: return lispNum(e->num);
    case Expr::Str: return lispStr(e->str);
    case Expr::Var:
      if (std::find(vars.begin(), vars.end(), e->str) == vars.end()) vars.push_back(e->str);
      return "v:" + e->str;
    case Expr::Add: return "(l+ " + toLispExpr(e->a, vars) + " " + toLispExpr(e->b, vars) + ")";
    case Expr::Sub: return "(l- " + toLispExpr(e->a, vars) + " " + toLispExpr(e->b, vars) + ")";
    case Expr::Neg: return "(l- 0 " + toLispExpr(e->a, vars) + ")";
  }
  return "0";
}

std::string toLispStmt(const Stmt& s, std::vector<std::string>& vars) {
  auto str = [&](const ExprP& e) { return "(str " + toLispExpr(e, vars) + ")"; };
  switch (s.kind) {
    case Stmt::Goto: return "(goto " + str(s.e1) + ")";
    case Stmt::GotoRel: return "(goto '" + s.name + ")";
    case Stmt::Back: return "(back)";
    case Stmt::Stop: return "(stop)";
    case Stmt::Set:
      if (std::find(vars.begin(), vars.end(), s.name) == vars.end()) vars.push_back(s.name);
      return "(set! v:" + s.name + " " + toLispExpr(s.e1, vars) + ")";
    case Stmt::Text: return "(set-text! " + str(s.e1) + " " + str(s.e2) + ")";
    case Stmt::Play: return "(play " + str(s.e1) + ")";
    case Stmt::Open: return "(open " + str(s.e1) + ")";
    case Stmt::Say: return "(say " + str(s.e1) + ")";
    case Stmt::If: {
      std::string cond = s.op.empty() ? "(l-true? " + toLispExpr(s.e1, vars) + ")"
                                      : "(l-cmp " + lispStr(s.op) + " " + toLispExpr(s.e1, vars) + " " + toLispExpr(s.e2, vars) + ")";
      return "(when " + cond + " " + toLispStmt(*s.then, vars) + ")";
    }
  }
  return "";
}
}  // namespace

std::string legacyToLisp(const std::string& source, std::vector<std::string>& vars) {
  Program p;
  if (!parse(source, p).ok) return "";
  std::string out;
  for (auto& st : p.stmts) out += toLispStmt(*st.second, vars) + "\n";
  return out.empty() ? "(begin)\n" : out;
}

ScriptResult checkScript(const std::string& source) {
  if (looksLikeLisp(source)) return Lisp::check(source);
  Program p;
  return parse(source, p);
}

ScriptResult runScript(const std::string& source, ScriptVars& vars, ScriptHost& host) {
  Program p;
  ScriptResult r = parse(source, p);
  if (!r.ok) return r;
  for (const auto& [line, st] : p.stmts) {
    try {
      std::string err = exec(*st, vars, host);
      if (!err.empty()) return {false, line, err};
    } catch (const ParseError& e) {
      return {false, line, e.msg};
    }
  }
  return {};
}

}  // namespace cairn

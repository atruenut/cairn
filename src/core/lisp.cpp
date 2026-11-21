#include "lisp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

namespace cairn {
namespace {

constexpr size_t kMaxSource = 40000, kMaxList = 20000, kMaxStr = 100000, kMaxTimers = 50;
constexpr int kMaxDepth = 180, kMaxNest = 120;
constexpr long kSteps = 400000;

struct LispError {
  std::string msg;
  int line = 0;
};

struct Val;
struct Env;
using V = std::shared_ptr<Val>;
using EnvP = std::shared_ptr<Env>;
struct Interp;
using Builtin = std::function<V(Interp&, std::vector<V>&)>;

struct Val {
  enum K { Void, Bool, Num, Str, Sym, List, Lambda, Fn } k = Void;
  double n = 0;
  bool b = false;
  std::string s;                 // Str contents, Sym name, Fn name
  std::vector<V> l;              // List elements; Lambda body forms
  std::vector<std::string> params;
  std::string rest;              // name of the rest parameter, or ""
  EnvP env;                      // Lambda closure
  const Builtin* fn = nullptr;   // Fn
  int line = 0;                  // for forms read from source
};

struct Env {
  std::unordered_map<std::string, V> vars;
  EnvP parent;
};

V mk(Val::K k) {
  V v = std::make_shared<Val>();
  v->k = k;
  return v;
}
V mkNum(double n) { V v = mk(Val::Num); v->n = n; return v; }
V mkBool(bool b) { V v = mk(Val::Bool); v->b = b; return v; }
V mkStr(const std::string& s) { V v = mk(Val::Str); v->s = s; return v; }
V mkSym(const std::string& s) { V v = mk(Val::Sym); v->s = s; return v; }
V mkList(std::vector<V> l = {}) { V v = mk(Val::List); v->l = std::move(l); return v; }
V mkVoid() { return mk(Val::Void); }

[[noreturn]] void fail(const std::string& m) { throw LispError{m, 0}; }

std::string fmtNum(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char b[64];
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    if (d == 0) return "0";
    std::snprintf(b, sizeof b, "%.0f", d);
  } else {
    std::snprintf(b, sizeof b, "%.10g", d);
  }
  return b;
}

std::string quoteStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o + "\"";
}

std::string show(const V& v, bool write) {
  switch (v->k) {
    case Val::Void: return "";
    case Val::Bool: return v->b ? "#t" : "#f";
    case Val::Num: return fmtNum(v->n);
    case Val::Str: return write ? quoteStr(v->s) : v->s;
    case Val::Sym: return v->s;
    case Val::Lambda: return "#<procedure>";
    case Val::Fn: return "#<procedure " + v->s + ">";
    case Val::List: {
      std::string o = "(";
      for (size_t i = 0; i < v->l.size(); ++i) {
        if (i) o += ' ';
        o += show(v->l[i], true);
      }
      return o + ")";
    }
  }
  return "";
}

bool truthy(const V& v) { return !(v->k == Val::Bool && !v->b); }

bool equalV(const V& a, const V& b) {
  if (a->k != b->k) return false;
  switch (a->k) {
    case Val::Void: return true;
    case Val::Bool: return a->b == b->b;
    case Val::Num: return a->n == b->n;
    case Val::Str: case Val::Sym: return a->s == b->s;
    case Val::List:
      if (a->l.size() != b->l.size()) return false;
      for (size_t i = 0; i < a->l.size(); ++i) if (!equalV(a->l[i], b->l[i])) return false;
      return true;
    default: return a.get() == b.get();
  }
}

// ------------------------------------------------------------------ reader
struct Reader {
  const std::string& src;
  size_t i = 0;
  int line = 1;
  explicit Reader(const std::string& s) : src(s) {}

  void skip() {
    while (i < src.size()) {
      char c = src[i];
      if (c == '\n') { ++line; ++i; }
      else if (c == ' ' || c == '\t' || c == '\r') ++i;
      else if (c == ';') { while (i < src.size() && src[i] != '\n') ++i; }
      else break;
    }
  }
  bool atEnd() { skip(); return i >= src.size(); }

  V read(int depth) {
    skip();
    if (i >= src.size()) throw LispError{"The script ends in the middle of an expression (a ) is missing)", line};
    if (depth > kMaxNest) throw LispError{"Brackets are nested too deeply", line};
    char c = src[i];
    int startLine = line;
    if (c == ')') throw LispError{"Unexpected )", line};
    if (c == '(') {
      ++i;
      V l = mkList();
      l->line = startLine;
      for (;;) {
        skip();
        if (i >= src.size()) throw LispError{"A ( on line " + std::to_string(startLine) + " is never closed", startLine};
        if (src[i] == ')') { ++i; break; }
        l->l.push_back(read(depth + 1));
        if (l->l.size() > kMaxList) throw LispError{"That list is too long", startLine};
      }
      return l;
    }
    if (c == '\'') {
      ++i;
      V q = mkList({mkSym("quote"), read(depth + 1)});
      q->line = startLine;
      return q;
    }
    if (c == '"') {
      ++i;
      std::string s;
      for (;;) {
        if (i >= src.size()) throw LispError{"A string that starts on line " + std::to_string(startLine) + " is never closed", startLine};
        char d = src[i++];
        if (d == '"') break;
        if (d == '\n') ++line;
        if (d == '\\' && i < src.size()) {
          char e = src[i++];
          if (e == 'n') s += '\n';
          else if (e == 't') s += '\t';
          else s += e;
        } else s += d;
        if (s.size() > kMaxStr) throw LispError{"That string is too long", startLine};
      }
      V v = mkStr(s);
      v->line = startLine;
      return v;
    }
    size_t j = i;
    while (j < src.size() && !std::isspace((unsigned char)src[j]) && src[j] != '(' && src[j] != ')' &&
           src[j] != '"' && src[j] != ';' && src[j] != '\'')
      ++j;
    std::string tok = src.substr(i, j - i);
    i = j;
    V v;
    if (tok == "#t") v = mkBool(true);
    else if (tok == "#f") v = mkBool(false);
    else {
      char* end = nullptr;
      double d = std::strtod(tok.c_str(), &end);
      bool numeric = !tok.empty() && end && *end == 0 && (std::isdigit((unsigned char)tok[0]) ||
                     ((tok[0] == '-' || tok[0] == '+' || tok[0] == '.') && tok.size() > 1 &&
                      (std::isdigit((unsigned char)tok[1]) || tok[1] == '.')));
      if (numeric && tok.find_first_of("xXnNiI") == std::string::npos) v = mkNum(d);
      else v = mkSym(tok);
    }
    v->line = startLine;
    return v;
  }
};

// ------------------------------------------------------------------ interpreter
struct Timer {
  int id;
  long due;
  long every;  // 0: once
  V fn;
};

struct Interp {
  EnvP global = std::make_shared<Env>();
  ScriptHost* host = nullptr;
  long clock = 0, steps = 0;
  int depth = 0, nextTimer = 1;
  bool running = false;
  std::vector<Timer> timers;
  std::mt19937 rng{std::random_device{}()};
  std::map<std::string, Builtin> table;  // owns the Builtin objects Val::fn points at

  Interp();

  void spend() {
    if (++steps > kSteps) fail("This script ran for too long and was stopped (is there a loop that never ends?)");
  }

  V lookup(const std::string& name, const EnvP& e) {
    for (Env* c = e.get(); c; c = c->parent.get()) {
      auto it = c->vars.find(name);
      if (it != c->vars.end()) return it->second;
    }
    fail("I don't know what \"" + name + "\" is (check the spelling, or define it first)");
  }

  V apply(const V& f, std::vector<V>& args);
  V eval(V x, EnvP env);
  // Binds a call to a closure; returns the env to evaluate the body in.
  EnvP bindCall(const V& f, std::vector<V>& args) {
    EnvP e = std::make_shared<Env>();
    e->parent = f->env;
    size_t np = f->params.size();
    if (args.size() < np || (f->rest.empty() && args.size() > np))
      fail("A procedure expected " + std::to_string(np) + (f->rest.empty() ? "" : " or more") + " argument" +
           (np == 1 ? "" : "s") + " but got " + std::to_string(args.size()));
    for (size_t i = 0; i < np; ++i) e->vars[f->params[i]] = args[i];
    if (!f->rest.empty()) e->vars[f->rest] = mkList(std::vector<V>(args.begin() + (long)np, args.end()));
    return e;
  }
  V makeLambda(const V& paramSpec, const V& form, size_t bodyFrom, const EnvP& env, const std::string& name);

  // host plumbing
  ScriptHost& H() {
    if (!host) fail("This can't be used here");
    return *host;
  }
  static ScriptValue toHost(const V& v) {
    ScriptValue s;
    if (v->k == Val::Num) s.n = v->n;
    else { s.isStr = true; s.s = show(v, false); }
    return s;
  }
  static V fromHost(const ScriptValue& s) { return s.isStr ? mkStr(s.s) : mkNum(s.n); }
};

struct DepthGuard {
  Interp& in;
  explicit DepthGuard(Interp& i) : in(i) {
    if (++in.depth > kMaxDepth) { --in.depth; fail("This is nested too deeply (does a procedure call itself forever?)"); }
  }
  ~DepthGuard() { --in.depth; }
};

V Interp::makeLambda(const V& spec, const V& form, size_t from, const EnvP& env, const std::string& name) {
  (void)name;
  V f = mk(Val::Lambda);
  f->env = env;
  if (spec->k == Val::Sym) f->rest = spec->s;
  else if (spec->k == Val::List) {
    for (size_t i = 0; i < spec->l.size(); ++i) {
      const V& p = spec->l[i];
      if (p->k != Val::Sym) fail("A parameter name must be a plain word");
      if (p->s == ".") {
        if (i + 2 != spec->l.size() || spec->l[i + 1]->k != Val::Sym) fail("Bad parameter list");
        f->rest = spec->l[i + 1]->s;
        break;
      }
      f->params.push_back(p->s);
    }
  } else fail("Bad parameter list");
  if (from >= form->l.size()) fail("A procedure needs a body");
  f->l.assign(form->l.begin() + (long)from, form->l.end());
  return f;
}

enum Sf { SF_NONE, SF_QUOTE, SF_IF, SF_COND, SF_DEFINE, SF_SET, SF_LAMBDA, SF_LET, SF_LETSTAR, SF_BEGIN, SF_AND, SF_OR,
          SF_WHEN, SF_UNLESS, SF_WHILE, SF_DOTIMES };
const std::unordered_map<std::string, Sf>& sfTable() {
  static const std::unordered_map<std::string, Sf> t = {
      {"quote", SF_QUOTE}, {"if", SF_IF}, {"cond", SF_COND}, {"define", SF_DEFINE}, {"set!", SF_SET},
      {"lambda", SF_LAMBDA}, {"let", SF_LET}, {"let*", SF_LETSTAR}, {"begin", SF_BEGIN}, {"and", SF_AND},
      {"or", SF_OR}, {"when", SF_WHEN}, {"unless", SF_UNLESS}, {"while", SF_WHILE}, {"dotimes", SF_DOTIMES}};
  return t;
}

V Interp::apply(const V& f, std::vector<V>& args) {
  if (f->k == Val::Fn) return (*f->fn)(*this, args);
  if (f->k != Val::Lambda) fail("That is not something that can be called: " + show(f, true));
  DepthGuard dg(*this);
  EnvP e = bindCall(f, args);
  V r = mkVoid();
  for (const V& form : f->l) r = eval(form, e);
  return r;
}

V Interp::eval(V x, EnvP env) {
  DepthGuard dg(*this);
  for (;;) {
    spend();
    switch (x->k) {
      case Val::Sym: return lookup(x->s, env);
      case Val::List: break;
      default: return x;
    }
    if (x->l.empty()) return x;
    const V& head = x->l[0];
    size_t n = x->l.size();
    Sf sf = SF_NONE;
    if (head->k == Val::Sym) {
      auto it = sfTable().find(head->s);
      if (it != sfTable().end()) sf = it->second;
    }
    auto need = [&](size_t lo) {
      if (n < lo) fail("\"" + head->s + "\" needs more parts: " + show(x, true));
    };
    switch (sf) {
      case SF_QUOTE: need(2); return x->l[1];
      case SF_IF: {
        need(3);
        if (truthy(eval(x->l[1], env))) { V t = x->l[2]; x = t; continue; }
        if (n > 3) { V t = x->l[3]; x = t; continue; }
        return mkVoid();
      }
      case SF_COND: {
        V pick;
        for (size_t i = 1; i < n; ++i) {
          const V& cl = x->l[i];
          if (cl->k != Val::List || cl->l.empty()) fail("Each cond clause must look like (test result ...)");
          bool isElse = cl->l[0]->k == Val::Sym && cl->l[0]->s == "else";
          V t = isElse ? mkBool(true) : eval(cl->l[0], env);
          if (!truthy(t)) continue;
          if (cl->l.size() == 1) return t;
          for (size_t j = 1; j + 1 < cl->l.size(); ++j) eval(cl->l[j], env);
          pick = cl->l.back();
          break;
        }
        if (!pick) return mkVoid();
        x = pick;
        continue;
      }
      case SF_DEFINE: {
        need(3);
        const V& target = x->l[1];
        if (target->k == Val::Sym) {
          if (n != 3) fail("define takes a name and one value");
          env->vars[target->s] = eval(x->l[2], env);
          return mkVoid();
        }
        if (target->k == Val::List && !target->l.empty() && target->l[0]->k == Val::Sym) {
          V spec = mkList(std::vector<V>(target->l.begin() + 1, target->l.end()));
          env->vars[target->l[0]->s] = makeLambda(spec, x, 2, env, target->l[0]->s);
          return mkVoid();
        }
        fail("define needs a name: (define name value) or (define (name arg ...) body)");
      }
      case SF_SET: {
        need(3);
        if (x->l[1]->k != Val::Sym) fail("set! needs a name");
        V v = eval(x->l[2], env);
        for (Env* c = env.get(); c; c = c->parent.get()) {
          auto it = c->vars.find(x->l[1]->s);
          if (it != c->vars.end()) { it->second = v; return mkVoid(); }
        }
        fail("set! can only change something that was defined first: " + x->l[1]->s);
      }
      case SF_LAMBDA: need(3); return makeLambda(x->l[1], x, 2, env, "");
      case SF_LET: case SF_LETSTAR: {
        need(3);
        size_t bi = 1;
        std::string loopName;
        if (sf == SF_LET && x->l[1]->k == Val::Sym) { loopName = x->l[1]->s; bi = 2; need(4); }
        const V& binds = x->l[bi];
        if (binds->k != Val::List) fail("let needs a list of (name value) pairs");
        EnvP e = std::make_shared<Env>();
        e->parent = env;
        std::vector<std::string> names;
        std::vector<V> vals;
        for (const V& b : binds->l) {
          if (b->k != Val::List || b->l.size() != 2 || b->l[0]->k != Val::Sym) fail("Each let binding must look like (name value)");
          V v = eval(b->l[1], sf == SF_LETSTAR ? e : env);
          if (sf == SF_LETSTAR) e->vars[b->l[0]->s] = v;
          else { names.push_back(b->l[0]->s); vals.push_back(v); }
        }
        if (!loopName.empty()) {
          V spec = mkList();
          for (const std::string& nm : names) spec->l.push_back(mkSym(nm));
          V f = makeLambda(spec, x, bi + 1, e, loopName);
          e->vars[loopName] = f;
          EnvP ne = bindCall(f, vals);
          for (size_t i = 0; i + 1 < f->l.size(); ++i) eval(f->l[i], ne);
          V last = f->l.back();
          x = last;
          env = ne;
          continue;
        }
        if (sf == SF_LET) for (size_t i = 0; i < names.size(); ++i) e->vars[names[i]] = vals[i];
        for (size_t i = bi + 1; i + 1 < n; ++i) eval(x->l[i], e);
        V last = x->l[n - 1];
        x = last;
        env = e;
        continue;
      }
      case SF_BEGIN: {
        if (n == 1) return mkVoid();
        for (size_t i = 1; i + 1 < n; ++i) eval(x->l[i], env);
        V last = x->l[n - 1];
        x = last;
        continue;
      }
      case SF_AND: {
        if (n == 1) return mkBool(true);
        bool tail = true;
        for (size_t i = 1; i + 1 < n; ++i)
          if (!truthy(eval(x->l[i], env))) { tail = false; break; }
        if (!tail) return mkBool(false);
        V last = x->l[n - 1];
        x = last;
        continue;
      }
      case SF_OR: {
        if (n == 1) return mkBool(false);
        for (size_t i = 1; i + 1 < n; ++i) {
          V v = eval(x->l[i], env);
          if (truthy(v)) return v;
        }
        V last = x->l[n - 1];
        x = last;
        continue;
      }
      case SF_WHEN: case SF_UNLESS: {
        need(2);
        bool t = truthy(eval(x->l[1], env));
        if (t != (sf == SF_WHEN) || n == 2) return mkVoid();
        for (size_t i = 2; i + 1 < n; ++i) eval(x->l[i], env);
        V last = x->l[n - 1];
        x = last;
        continue;
      }
      case SF_WHILE: {
        need(2);
        while (truthy(eval(x->l[1], env))) {
          spend();
          for (size_t i = 2; i < n; ++i) eval(x->l[i], env);
        }
        return mkVoid();
      }
      case SF_DOTIMES: {
        need(3);
        const V& spec = x->l[1];
        if (spec->k != Val::List || spec->l.size() != 2 || spec->l[0]->k != Val::Sym) fail("dotimes looks like (dotimes (i 10) ...)");
        V cnt = eval(spec->l[1], env);
        if (cnt->k != Val::Num) fail("dotimes needs a number of times");
        EnvP e = std::make_shared<Env>();
        e->parent = env;
        for (long i = 0; i < (long)cnt->n; ++i) {
          spend();
          e->vars[spec->l[0]->s] = mkNum((double)i);
          for (size_t j = 2; j < n; ++j) eval(x->l[j], e);
        }
        return mkVoid();
      }
      case SF_NONE: break;
    }
    // procedure call
    V f = eval(head, env);
    std::vector<V> args;
    args.reserve(n - 1);
    for (size_t i = 1; i < n; ++i) args.push_back(eval(x->l[i], env));
    if (f->k == Val::Fn) return (*f->fn)(*this, args);
    if (f->k != Val::Lambda) fail("That is not something that can be called: " + show(f, true));
    EnvP ne = bindCall(f, args);
    for (size_t i = 0; i + 1 < f->l.size(); ++i) eval(f->l[i], ne);
    V last = f->l.back();
    x = last;
    env = ne;
  }
}

// ------------------------------------------------------------------ builtins
double numArg(const std::vector<V>& a, size_t i, const char* who) {
  if (i >= a.size()) fail(std::string(who) + " needs more arguments");
  if (a[i]->k != Val::Num) fail(std::string(who) + " needs a number but got " + show(a[i], true));
  return a[i]->n;
}
const std::string& strArg(const std::vector<V>& a, size_t i, const char* who) {
  if (i >= a.size()) fail(std::string(who) + " needs more arguments");
  if (a[i]->k != Val::Str) fail(std::string(who) + " needs a string (text in quotes) but got " + show(a[i], true));
  return a[i]->s;
}
const std::vector<V>& listArg(const std::vector<V>& a, size_t i, const char* who) {
  if (i >= a.size()) fail(std::string(who) + " needs more arguments");
  if (a[i]->k != Val::List) fail(std::string(who) + " needs a list but got " + show(a[i], true));
  return a[i]->l;
}
void arity(const std::vector<V>& a, size_t lo, size_t hi, const char* who) {
  if (a.size() < lo || a.size() > hi) {
    std::string want = lo == hi ? std::to_string(lo) : std::to_string(lo) + (hi > 100 ? " or more" : " to " + std::to_string(hi));
    fail(std::string(who) + " needs " + want + " argument" + (lo == 1 && hi == 1 ? "" : "s") + " but got " + std::to_string(a.size()));
  }
}

bool lessV(Interp& in, const V& a, const V& b, const V& cmp) {
  if (cmp) {
    std::vector<V> args{a, b};
    return truthy(in.apply(cmp, args));
  }
  if (a->k == Val::Num && b->k == Val::Num) return a->n < b->n;
  if (a->k == Val::Str && b->k == Val::Str) return a->s < b->s;
  fail("sort can compare numbers or strings, or give it your own (lambda (a b) ...)");
}

std::vector<V> mergeSort(Interp& in, std::vector<V> v, const V& cmp) {
  if (v.size() < 2) return v;
  size_t mid = v.size() / 2;
  std::vector<V> a = mergeSort(in, std::vector<V>(v.begin(), v.begin() + (long)mid), cmp);
  std::vector<V> b = mergeSort(in, std::vector<V>(v.begin() + (long)mid, v.end()), cmp);
  std::vector<V> out;
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    in.spend();
    if (lessV(in, b[j], a[i], cmp)) out.push_back(b[j++]);
    else out.push_back(a[i++]);
  }
  while (i < a.size()) out.push_back(a[i++]);
  while (j < b.size()) out.push_back(b[j++]);
  return out;
}

Interp::Interp() {
  auto def = [&](const std::string& name, Builtin f) {
    auto it = table.emplace(name, std::move(f)).first;
    V v = mk(Val::Fn);
    v->s = name;
    v->fn = &it->second;
    global->vars[name] = v;
  };
  global->vars["pi"] = mkNum(3.14159265358979323846);
  global->vars["true"] = mkBool(true);
  global->vars["false"] = mkBool(false);

  // ---- arithmetic
  def("+", [](Interp&, std::vector<V>& a) {
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i]->k != Val::Num) fail("+ adds numbers, but got " + show(a[i], true) + " (to join text use str)");
      s += a[i]->n;
    }
    return mkNum(s);
  });
  def("*", [](Interp&, std::vector<V>& a) {
    double s = 1;
    for (size_t i = 0; i < a.size(); ++i) s *= numArg(a, i, "*");
    return mkNum(s);
  });
  def("-", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1000, "-");
    double s = numArg(a, 0, "-");
    if (a.size() == 1) return mkNum(-s);
    for (size_t i = 1; i < a.size(); ++i) s -= numArg(a, i, "-");
    return mkNum(s);
  });
  def("/", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1000, "/");
    double s = numArg(a, 0, "/");
    if (a.size() == 1) { if (s == 0) fail("Division by zero"); return mkNum(1 / s); }
    for (size_t i = 1; i < a.size(); ++i) {
      double d = numArg(a, i, "/");
      if (d == 0) fail("Division by zero");
      s /= d;
    }
    return mkNum(s);
  });
  def("mod", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "mod");
    double x = numArg(a, 0, "mod"), y = numArg(a, 1, "mod");
    if (y == 0) fail("Division by zero");
    double r = std::fmod(x, y);
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return mkNum(r);
  });
  def("quotient", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "quotient");
    double y = numArg(a, 1, "quotient");
    if (y == 0) fail("Division by zero");
    return mkNum(std::trunc(numArg(a, 0, "quotient") / y));
  });
  auto un = [&](const char* name, double (*f)(double)) {
    std::string nm = name;
    def(nm, [f, nm](Interp&, std::vector<V>& a) {
      arity(a, 1, 1, nm.c_str());
      return mkNum(f(numArg(a, 0, nm.c_str())));
    });
  };
  un("abs", [](double x) { return std::fabs(x); });
  un("floor", [](double x) { return std::floor(x); });
  un("ceiling", [](double x) { return std::ceil(x); });
  un("truncate", [](double x) { return std::trunc(x); });
  un("round", [](double x) { return std::floor(x + 0.5); });
  un("sin", [](double x) { return std::sin(x); });
  un("cos", [](double x) { return std::cos(x); });
  un("tan", [](double x) { return std::tan(x); });
  un("atan", [](double x) { return std::atan(x); });
  un("exp", [](double x) { return std::exp(x); });
  un("sqrt", [](double x) -> double {
    if (x < 0) fail("sqrt of a negative number");
    return std::sqrt(x);
  });
  un("log", [](double x) -> double {
    if (x <= 0) fail("log needs a number above zero");
    return std::log(x);
  });
  def("expt", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "expt");
    return mkNum(std::pow(numArg(a, 0, "expt"), numArg(a, 1, "expt")));
  });
  def("min", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1000, "min");
    double m = numArg(a, 0, "min");
    for (size_t i = 1; i < a.size(); ++i) m = std::min(m, numArg(a, i, "min"));
    return mkNum(m);
  });
  def("max", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1000, "max");
    double m = numArg(a, 0, "max");
    for (size_t i = 1; i < a.size(); ++i) m = std::max(m, numArg(a, i, "max"));
    return mkNum(m);
  });
  def("rgb", [](Interp&, std::vector<V>& a) {
    arity(a, 3, 3, "rgb");
    auto ch = [&](size_t i) { return (long)std::max(0.0, std::min(255.0, std::floor(numArg(a, i, "rgb")))); };
    return mkNum((double)((ch(0) << 16) | (ch(1) << 8) | ch(2)));
  });
  // Used when older line-style scripts are converted (legacyToLisp in script.cpp): they keep their own rules
  // for + on text, comparisons and "is this true".
  auto legacyShow = [](const V& v) -> std::string {
    if (v->k != Val::Num) return show(v, false);
    double n = v->n;
    if (std::isnan(n) || std::isinf(n)) return "0";
    char b[64];
    if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(b, sizeof b, "%.0f", n);
    else std::snprintf(b, sizeof b, "%g", n);
    return b;
  };
  def("l+", [legacyShow](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "l+");
    if (a[0]->k == Val::Str || a[1]->k == Val::Str) return mkStr(legacyShow(a[0]) + legacyShow(a[1]));
    return mkNum(numArg(a, 0, "l+") + numArg(a, 1, "l+"));
  });
  def("l-", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "l-");
    if (a[0]->k == Val::Str || a[1]->k == Val::Str) fail("can't subtract text");
    return mkNum(numArg(a, 0, "l-") - numArg(a, 1, "l-"));
  });
  def("l-true?", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "l-true?");
    return mkBool(a[0]->k == Val::Str ? !a[0]->s.empty() : a[0]->n != 0);
  });
  def("l-cmp", [legacyShow](Interp&, std::vector<V>& a) {
    arity(a, 3, 3, "l-cmp");
    int c;
    if (a[1]->k == Val::Num && a[2]->k == Val::Num) c = a[1]->n < a[2]->n ? -1 : a[1]->n > a[2]->n ? 1 : 0;
    else { std::string x = legacyShow(a[1]), y = legacyShow(a[2]); c = x < y ? -1 : x == y ? 0 : 1; }
    const std::string& op = a[0]->s;
    if (op == "==" || op == "=") return mkBool(c == 0);
    if (op == "!=" || op == "<>") return mkBool(c != 0);
    if (op == "<") return mkBool(c < 0);
    if (op == ">") return mkBool(c > 0);
    if (op == "<=") return mkBool(c <= 0);
    return mkBool(c >= 0);
  });
  def("random", [](Interp& in, std::vector<V>& a) {
    arity(a, 0, 1, "random");
    double u = std::uniform_real_distribution<double>(0.0, 1.0)(in.rng);
    if (a.empty()) return mkNum(u);
    double n = numArg(a, 0, "random");
    if (n < 1) fail("random needs a number of 1 or more");
    return mkNum(std::floor(u * n));
  });
  // ---- comparison and logic
  auto cmp = [&](const char* name, bool (*f)(double, double)) {
    std::string nm = name;
    def(nm, [f, nm](Interp&, std::vector<V>& a) {
      arity(a, 2, 1000, nm.c_str());
      for (size_t i = 0; i + 1 < a.size(); ++i)
        if (!f(numArg(a, i, nm.c_str()), numArg(a, i + 1, nm.c_str()))) return mkBool(false);
      return mkBool(true);
    });
  };
  cmp("=", [](double x, double y) { return x == y; });
  cmp("<", [](double x, double y) { return x < y; });
  cmp(">", [](double x, double y) { return x > y; });
  cmp("<=", [](double x, double y) { return x <= y; });
  cmp(">=", [](double x, double y) { return x >= y; });
  def("not", [](Interp&, std::vector<V>& a) { arity(a, 1, 1, "not"); return mkBool(!truthy(a[0])); });
  def("equal?", [](Interp&, std::vector<V>& a) { arity(a, 2, 2, "equal?"); return mkBool(equalV(a[0], a[1])); });
  def("eq?", [](Interp&, std::vector<V>& a) { arity(a, 2, 2, "eq?"); return mkBool(equalV(a[0], a[1])); });
  auto pred = [&](const char* name, std::function<bool(const V&)> f) {
    std::string nm = name;
    def(nm, [f, nm](Interp&, std::vector<V>& a) { arity(a, 1, 1, nm.c_str()); return mkBool(f(a[0])); });
  };
  pred("number?", [](const V& v) { return v->k == Val::Num; });
  pred("string?", [](const V& v) { return v->k == Val::Str; });
  pred("symbol?", [](const V& v) { return v->k == Val::Sym; });
  pred("boolean?", [](const V& v) { return v->k == Val::Bool; });
  pred("list?", [](const V& v) { return v->k == Val::List; });
  pred("null?", [](const V& v) { return v->k == Val::List && v->l.empty(); });
  pred("procedure?", [](const V& v) { return v->k == Val::Lambda || v->k == Val::Fn; });
  pred("zero?", [](const V& v) { return v->k == Val::Num && v->n == 0; });
  pred("even?", [](const V& v) { return v->k == Val::Num && std::fmod(v->n, 2) == 0; });
  pred("odd?", [](const V& v) { return v->k == Val::Num && std::fmod(std::fabs(v->n), 2) == 1; });
  // ---- lists
  def("list", [](Interp&, std::vector<V>& a) { return mkList(a); });
  def("cons", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "cons");
    std::vector<V> r{a[0]};
    for (const V& v : listArg(a, 1, "cons")) r.push_back(v);
    if (r.size() > kMaxList) fail("That list is too long");
    return mkList(r);
  });
  def("car", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "car");
    const auto& l = listArg(a, 0, "car");
    if (l.empty()) fail("car of an empty list");
    return l[0];
  });
  def("cdr", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "cdr");
    const auto& l = listArg(a, 0, "cdr");
    if (l.empty()) fail("cdr of an empty list");
    return mkList(std::vector<V>(l.begin() + 1, l.end()));
  });
  def("length", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "length");
    if (a[0]->k == Val::Str) return mkNum((double)a[0]->s.size());
    return mkNum((double)listArg(a, 0, "length").size());
  });
  def("nth", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "nth");
    const auto& l = listArg(a, 0, "nth");
    double i = numArg(a, 1, "nth");
    if (i < 0 || i >= (double)l.size()) fail("nth: the list has " + std::to_string(l.size()) + " items, so there is no item " + fmtNum(i) + " (counting starts at 0)");
    return l[(size_t)i];
  });
  def("append", [](Interp&, std::vector<V>& a) {
    std::vector<V> r;
    for (size_t i = 0; i < a.size(); ++i)
      for (const V& v : listArg(a, i, "append")) r.push_back(v);
    if (r.size() > kMaxList) fail("That list is too long");
    return mkList(r);
  });
  def("reverse", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "reverse");
    std::vector<V> r(listArg(a, 0, "reverse"));
    std::reverse(r.begin(), r.end());
    return mkList(r);
  });
  def("range", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 3, "range");
    double lo = 0, hi, st = 1;
    if (a.size() == 1) hi = numArg(a, 0, "range");
    else { lo = numArg(a, 0, "range"); hi = numArg(a, 1, "range"); if (a.size() == 3) st = numArg(a, 2, "range"); }
    if (st == 0) fail("range: the step can't be 0");
    std::vector<V> r;
    for (double x = lo; st > 0 ? x < hi : x > hi; x += st) {
      r.push_back(mkNum(x));
      if (r.size() > kMaxList) fail("That list is too long");
    }
    return mkList(r);
  });
  def("member", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "member");
    for (const V& v : listArg(a, 1, "member")) if (equalV(v, a[0])) return mkBool(true);
    return mkBool(false);
  });
  def("map", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "map");
    std::vector<V> r;
    for (const V& v : listArg(a, 1, "map")) {
      std::vector<V> args{v};
      r.push_back(in.apply(a[0], args));
    }
    return mkList(r);
  });
  def("filter", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "filter");
    std::vector<V> r;
    for (const V& v : listArg(a, 1, "filter")) {
      std::vector<V> args{v};
      if (truthy(in.apply(a[0], args))) r.push_back(v);
    }
    return mkList(r);
  });
  def("reduce", [](Interp& in, std::vector<V>& a) {
    arity(a, 3, 3, "reduce");
    V acc = a[1];
    for (const V& v : listArg(a, 2, "reduce")) {
      std::vector<V> args{acc, v};
      acc = in.apply(a[0], args);
    }
    return acc;
  });
  def("for-each", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "for-each");
    for (const V& v : listArg(a, 1, "for-each")) {
      std::vector<V> args{v};
      in.apply(a[0], args);
    }
    return mkVoid();
  });
  def("apply", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "apply");
    std::vector<V> args(listArg(a, 1, "apply"));
    return in.apply(a[0], args);
  });
  def("sort", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 2, "sort");
    return mkList(mergeSort(in, listArg(a, 0, "sort"), a.size() > 1 ? a[1] : V()));
  });
  // ---- text
  def("str", [](Interp&, std::vector<V>& a) {
    std::string s;
    for (const V& v : a) s += show(v, false);
    if (s.size() > kMaxStr) fail("That string is too long");
    return mkStr(s);
  });
  def("string-append", [](Interp&, std::vector<V>& a) {
    std::string s;
    for (size_t i = 0; i < a.size(); ++i) s += strArg(a, i, "string-append");
    if (s.size() > kMaxStr) fail("That string is too long");
    return mkStr(s);
  });
  def("string-length", [](Interp&, std::vector<V>& a) { arity(a, 1, 1, "string-length"); return mkNum((double)strArg(a, 0, "string-length").size()); });
  def("substring", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 3, "substring");
    const std::string& s = strArg(a, 0, "substring");
    double lo = numArg(a, 1, "substring"), hi = a.size() > 2 ? numArg(a, 2, "substring") : (double)s.size();
    lo = std::max(0.0, std::min(lo, (double)s.size()));
    hi = std::max(lo, std::min(hi, (double)s.size()));
    return mkStr(s.substr((size_t)lo, (size_t)(hi - lo)));
  });
  def("string-upcase", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "string-upcase");
    std::string s = strArg(a, 0, "string-upcase");
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return mkStr(s);
  });
  def("string-downcase", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "string-downcase");
    std::string s = strArg(a, 0, "string-downcase");
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return mkStr(s);
  });
  def("string-contains?", [](Interp&, std::vector<V>& a) {
    arity(a, 2, 2, "string-contains?");
    return mkBool(strArg(a, 0, "string-contains?").find(strArg(a, 1, "string-contains?")) != std::string::npos);
  });
  def("number->string", [](Interp&, std::vector<V>& a) { arity(a, 1, 1, "number->string"); return mkStr(fmtNum(numArg(a, 0, "number->string"))); });
  def("string->number", [](Interp&, std::vector<V>& a) {
    arity(a, 1, 1, "string->number");
    const std::string& s = strArg(a, 0, "string->number");
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (s.empty() || !end || *end != 0 || s.find_first_of("xXnNiI ") != std::string::npos) return mkBool(false);
    return mkNum(d);
  });
  // ---- the page (through the host)
  def("goto", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 1, "goto");
    ScriptHost& h = in.H();
    if (a[0]->k == Val::Sym) {
      const std::string& w = a[0]->s;
      if (w == "back") { if (!h.goBack()) fail("There is no page to go back to"); return mkVoid(); }
      if (w != "next" && w != "prev" && w != "first" && w != "last") fail("goto knows 'next 'prev 'first 'last 'back, or a page title in quotes");
      if (!h.gotoRelative(w)) fail("There is no " + w + " page to go to");
      return mkVoid();
    }
    const std::string& t = strArg(a, 0, "goto");
    if (!h.gotoPage(t)) fail("There is no page called \"" + t + "\"");
    return mkVoid();
  });
  def("back", [](Interp& in, std::vector<V>& a) {
    arity(a, 0, 0, "back");
    if (!in.H().goBack()) fail("There is no page to go back to");
    return mkVoid();
  });
  def("say", [](Interp& in, std::vector<V>& a) {
    std::string s;
    for (const V& v : a) s += show(v, false);
    in.H().say(s);
    return mkVoid();
  });
  def("open", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 1, "open");
    if (!in.H().open(strArg(a, 0, "open"))) fail("That can't be opened");
    return mkVoid();
  });
  def("play", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 1, "play");
    const std::string& n = strArg(a, 0, "play");
    if (!in.H().play(n)) fail("I couldn't play a sound called \"" + n + "\"");
    return mkVoid();
  });
  def("stop", [](Interp& in, std::vector<V>& a) { arity(a, 0, 0, "stop"); in.H().stop(); return mkVoid(); });
  def("page-title", [](Interp& in, std::vector<V>& a) { arity(a, 0, 0, "page-title"); return mkStr(in.H().pageTitle()); });
  def("get", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "get");
    const std::string& name = strArg(a, 0, "get");
    std::string prop = a[1]->k == Val::Sym || a[1]->k == Val::Str ? a[1]->s : "";
    ScriptValue sv;
    if (!in.H().getProp(name, prop, sv)) fail("There is no object called \"" + name + "\" with a '" + prop + " (give an object a script name in its Properties)");
    return Interp::fromHost(sv);
  });
  def("text", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 1, "text");
    ScriptValue sv;
    if (!in.H().getProp(strArg(a, 0, "text"), "text", sv)) fail("There is no text object called \"" + a[0]->s + "\"");
    return Interp::fromHost(sv);
  });
  def("set-prop!", [](Interp& in, std::vector<V>& a) {
    arity(a, 3, 3, "set-prop!");
    const std::string& name = strArg(a, 0, "set-prop!");
    std::string prop = a[1]->k == Val::Sym || a[1]->k == Val::Str ? a[1]->s : "";
    if (!in.H().setProp(name, prop, Interp::toHost(a[2])))
      fail("I can't set '" + prop + " on \"" + name + "\" (is there an object with that script name? x y w h text fill color line can be set)");
    return mkVoid();
  });
  def("set-text!", [](Interp& in, std::vector<V>& a) {
    arity(a, 2, 2, "set-text!");
    const std::string& name = strArg(a, 0, "set-text!");
    if (!in.H().setProp(name, "text", Interp::toHost(a[1]))) fail("There is no text object called \"" + name + "\" (give it a script name in its Properties)");
    return mkVoid();
  });
  // ---- time
  def("now", [](Interp& in, std::vector<V>& a) { arity(a, 0, 0, "now"); return mkNum((double)in.clock); });
  auto addTimer = [&](const char* name, bool repeat) {
    std::string nm = name;
    def(nm, [repeat, nm](Interp& in, std::vector<V>& a) {
      arity(a, 2, 2, nm.c_str());
      double ms = numArg(a, 0, nm.c_str());
      if (a[1]->k != Val::Lambda && a[1]->k != Val::Fn) fail(nm + " needs a procedure to run, like (lambda () ...)");
      if (in.timers.size() >= kMaxTimers) fail("Too many timers are running");
      long d = (long)std::max(repeat ? 20.0 : 0.0, std::min(ms, 3600000.0));
      Timer t{in.nextTimer++, in.clock + d, repeat ? d : 0, a[1]};
      in.timers.push_back(t);
      return mkNum(t.id);
    });
  };
  addTimer("after", false);
  addTimer("every", true);
  def("cancel", [](Interp& in, std::vector<V>& a) {
    arity(a, 1, 1, "cancel");
    double id = numArg(a, 0, "cancel");
    in.timers.erase(std::remove_if(in.timers.begin(), in.timers.end(), [&](const Timer& t) { return t.id == (int)id; }), in.timers.end());
    return mkVoid();
  });
  def("cancel-timers", [](Interp& in, std::vector<V>& a) { arity(a, 0, 0, "cancel-timers"); in.timers.clear(); return mkVoid(); });
}

ScriptResult toResult(const LispError& e) {
  ScriptResult r;
  r.ok = false;
  r.line = e.line;
  r.error = e.msg;
  return r;
}

}  // namespace

struct Lisp::Impl {
  Interp in;
};

Lisp::Lisp() : p_(new Impl) {}
Lisp::~Lisp() { reset(); }

bool looksLikeLisp(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (std::isspace((unsigned char)c)) { ++i; continue; }
    if (c == ';' || (c == '#' && (i + 1 >= s.size() || s[i + 1] == ' ' || s[i + 1] == '#' || s[i + 1] == '\n' || s[i + 1] == '\r'))) {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    return c == '(' || c == '\'';
  }
  return false;
}

ScriptResult Lisp::check(const std::string& source) {
  if (source.size() > kMaxSource) return {false, 1, "That script is too long"};
  try {
    Reader r(source);
    while (!r.atEnd()) r.read(0);
  } catch (const LispError& e) {
    return toResult(e);
  }
  return {};
}

namespace {
struct RunGuard {
  Interp& in;
  ScriptHost* saved;
  RunGuard(Interp& i, ScriptHost& h) : in(i), saved(i.host) {
    if (in.running) fail("A script can't start another script");
    in.running = true;
    in.host = &h;
    in.steps = 0;
    in.depth = 0;
  }
  ~RunGuard() {
    in.running = false;
    in.host = saved;
  }
};
}  // namespace

ScriptResult Lisp::run(const std::string& source, ScriptHost& host) {
  if (source.size() > kMaxSource) return {false, 1, "That script is too long"};
  int line = 0;
  try {
    RunGuard g(p_->in, host);
    Reader r(source);
    std::vector<V> forms;
    while (!r.atEnd()) forms.push_back(r.read(0));
    for (const V& f : forms) {
      line = f->line;
      p_->in.eval(f, p_->in.global);
    }
  } catch (const LispError& e) {
    LispError x = e;
    if (!x.line) x.line = line;
    return toResult(x);
  }
  return {};
}

ScriptResult Lisp::tick(long nowMs, ScriptHost& host) {
  Interp& in = p_->in;
  if (nowMs > in.clock) in.clock = nowMs;
  if (in.timers.empty()) return {};
  try {
    RunGuard g(in, host);
    for (int guard = 0; guard < 1000; ++guard) {
      size_t best = in.timers.size();
      for (size_t i = 0; i < in.timers.size(); ++i)
        if (in.timers[i].due <= in.clock && (best == in.timers.size() || in.timers[i].due < in.timers[best].due)) best = i;
      if (best == in.timers.size()) break;
      Timer t = in.timers[best];
      if (t.every) {
        long next = t.due + t.every;
        in.timers[best].due = next > in.clock ? next : in.clock + t.every;  // fell behind: skip, don't burst
      } else {
        in.timers.erase(in.timers.begin() + (long)best);
      }
      in.steps = 0;
      std::vector<V> none;
      in.apply(t.fn, none);
    }
  } catch (const LispError& e) {
    in.timers.clear();  // a timer that fails once would only fail again
    LispError x = e;
    return toResult(x);
  }
  return {};
}

void Lisp::setClock(long ms) { p_->in.clock = ms; }
long Lisp::clock() const { return p_->in.clock; }
bool Lisp::hasTimers() const { return !p_->in.timers.empty(); }
long Lisp::nextDue() const {
  long best = -1;
  for (const Timer& t : p_->in.timers) if (best < 0 || t.due < best) best = t.due;
  return best;
}
void Lisp::cancelTimers() { p_->in.timers.clear(); }
void Lisp::reset() {
  Interp& in = p_->in;
  in.timers.clear();
  for (auto& kv : in.global->vars) {  // break closure cycles through the global environment
    if (kv.second->k == Val::Lambda) kv.second->env.reset();
  }
  in.global->vars.clear();
  Interp fresh;
  in.global = fresh.global;
  in.table.swap(fresh.table);
  in.clock = 0;
  in.steps = 0;
  in.depth = 0;
}
void Lisp::seed(unsigned s) { p_->in.rng.seed(s); }

namespace {
struct CaptureHost : ScriptHost {
  std::string out;
  bool gotoPage(const std::string& t) override { out += "[goto " + t + "]\n"; return true; }
  bool gotoRelative(const std::string& w) override { out += "[goto " + w + "]\n"; return true; }
  bool goBack() override { out += "[back]\n"; return true; }
  bool setText(const std::string&, const std::string&) override { return false; }
  bool play(const std::string& n) override { out += "[play " + n + "]\n"; return true; }
  void stop() override { out += "[stop]\n"; }
  bool open(const std::string& t) override { out += "[open " + t + "]\n"; return true; }
  void say(const std::string& m) override { out += m + "\n"; }
  std::map<std::string, ScriptValue> props;
  bool getProp(const std::string& n, const std::string& p, ScriptValue& v) override {
    auto it = props.find(n + "/" + p);
    if (it == props.end()) return false;
    v = it->second;
    return true;
  }
  bool setProp(const std::string& n, const std::string& p, const ScriptValue& v) override {
    if (n != "box") return false;
    props[n + "/" + p] = v;
    return true;
  }
  std::string pageTitle() override { return "Test page"; }
};
}  // namespace

// Test helper: runs `source`, then advances virtual time to 5 seconds in 10 ms steps so timers run too.
std::string lispRunCapture(const std::string& source, std::string* error) {
  Lisp lisp;
  CaptureHost h;
  h.props["box/text"] = ScriptValue{true, 0, "hello"};
  h.props["box/x"] = ScriptValue{false, 10, ""};
  lisp.seed(1);
  ScriptResult r = lisp.run(source, h);
  for (long t = 10; r.ok && t <= 5000 && lisp.hasTimers(); t += 10) r = lisp.tick(t, h);
  if (!r.ok) {
    if (error) *error = r.error;
    h.out += "ERROR: " + r.error + "\n";
  }
  return h.out;
}

}  // namespace cairn

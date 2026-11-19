#include "document.h"
#include "media.h"
#include "rich.h"

#include "sha256.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace cairn {

namespace {

constexpr int kAppId = 0x4341524E;  // "CARN"
constexpr int kSchemaVersion = 2;
constexpr size_t kMaxUndo = 500;

bool byZ(const Object& a, const Object& b) {
  return a.z != b.z ? a.z < b.z : a.id < b.id;
}

Page headerOnly(const Page& p) {
  Page c = p;
  c.objects.clear();
  return c;
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

Page rowToPage(Stmt& s) {
  Page p;
  p.id = s.colInt(0);
  p.title = s.colText(1);
  p.w = (int)s.colInt(2);
  p.h = (int)s.colInt(3);
  p.bg = (int)s.colInt(4);
  p.sort = (int)s.colInt(5);
  p.tags = s.colText(6);
  p.created = s.colInt(7);
  p.modified = s.colInt(8);
  return p;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

std::unique_ptr<Document> Document::create(const std::string& path, bool seed) {
  namespace fs = std::filesystem;
  if (fs::exists(fs::u8path(path))) throw DbError("File already exists: " + path);
  std::unique_ptr<Document> d(new Document());
  d->path_ = path;
  d->db_.open(path, true);
  try {
    d->initSchema();
    d->loadAll();
    if (seed) d->seedStarter();
  } catch (...) {
    d->db_.close();
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
    throw;
  }
  return d;
}

std::unique_ptr<Document> Document::open(const std::string& path) {
  std::unique_ptr<Document> d(new Document());
  d->path_ = path;
  d->db_.open(path, false);
  int version = 0;
  try {
    version = d->checkHeader();
    d->loadAll();
  } catch (const DbError& e) {
    throw DbError("Not a Cairn file, or the file is damaged. (" + std::string(e.what()) + ")");
  }
  if (version < kSchemaVersion) d->migrate(version);  // errors here are real errors, not "wrong file"
  d->purgeUnusedBlobs();
  return d;
}

Document::~Document() = default;

void Document::initSchema() {
  db_.exec("PRAGMA journal_mode=DELETE");  // keep a Cairn to exactly one file
  db_.exec(("PRAGMA application_id=" + std::to_string(kAppId)).c_str());
  db_.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
  db_.exec(R"SQL(
    BEGIN;
    CREATE TABLE page(
      id INTEGER PRIMARY KEY, title TEXT NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,
      bg INTEGER NOT NULL, sort INTEGER NOT NULL, tags TEXT NOT NULL DEFAULT '',
      created INTEGER NOT NULL, modified INTEGER NOT NULL);
    CREATE TABLE object(
      id INTEGER PRIMARY KEY, page_id INTEGER NOT NULL, type INTEGER NOT NULL,
      x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,
      z INTEGER NOT NULL, props TEXT NOT NULL DEFAULT '');
    CREATE INDEX object_page ON object(page_id);
    CREATE TABLE link(
      src_page INTEGER NOT NULL, src_object INTEGER NOT NULL, target_page INTEGER NOT NULL);
    CREATE INDEX link_target ON link(target_page);
    CREATE INDEX link_src ON link(src_page);
    CREATE VIRTUAL TABLE fts USING fts5(title, body, page_id UNINDEXED);
    CREATE TABLE blob(
      id INTEGER PRIMARY KEY, hash TEXT NOT NULL UNIQUE, mime TEXT NOT NULL,
      name TEXT NOT NULL, data BLOB NOT NULL);
    COMMIT;
  )SQL");
}

int Document::checkHeader() {
  {
    Stmt s(db_, "PRAGMA application_id");
    if (!s.step() || s.colInt(0) != kAppId) throw DbError("wrong application id");
  }
  Stmt s(db_, "PRAGMA user_version");
  s.step();
  int v = (int)s.colInt(0);
  if (v > kSchemaVersion) throw DbError("This file was made by a newer version of Cairn");
  return v;
}

// Additive, one step per version, each in a transaction so a failure leaves the file as it was.
void Document::migrate(int from) {
  if (from < 2) {
    db_.exec(R"SQL(
      BEGIN;
      CREATE TABLE IF NOT EXISTS blob(
        id INTEGER PRIMARY KEY, hash TEXT NOT NULL UNIQUE, mime TEXT NOT NULL,
        name TEXT NOT NULL, data BLOB NOT NULL);
      PRAGMA user_version=2;
      COMMIT;
    )SQL");
  }
}

size_t Document::purgeUnusedBlobs() {
  try {
    std::set<int64_t> used;
    for (const Page& p : pages_)
      for (const Object& o : p.objects)
        if (o.type == ObjType::Image || o.type == ObjType::Audio) used.insert(o.propInt("blob", 0));
    std::vector<int64_t> dead;
    {
      Stmt s(db_, "SELECT id FROM blob");
      while (s.step())
        if (!used.count(s.colInt(0))) dead.push_back(s.colInt(0));
    }
    if (dead.empty()) return 0;
    db_.exec("BEGIN IMMEDIATE");
    for (int64_t id : dead) {
      Stmt d(db_, "DELETE FROM blob WHERE id=?");
      d.bindInt(1, id).run();
    }
    db_.exec("COMMIT");
    return dead.size();
  } catch (const DbError&) {
    return 0;  // housekeeping only: never block opening a file over it
  }
}

void Document::loadAll() {
  pages_.clear();
  int64_t maxPage = 0, maxObj = 0;
  {
    Stmt s(db_, "SELECT id,title,w,h,bg,sort,tags,created,modified FROM page ORDER BY sort,id");
    while (s.step()) {
      pages_.push_back(rowToPage(s));
      maxPage = std::max(maxPage, pages_.back().id);
    }
  }
  {
    Stmt s(db_, "SELECT id,page_id,type,x,y,w,h,z,props FROM object ORDER BY z,id");
    while (s.step()) {
      Object o;
      o.id = s.colInt(0);
      o.pageId = s.colInt(1);
      o.type = (ObjType)s.colInt(2);
      o.x = (int)s.colInt(3);
      o.y = (int)s.colInt(4);
      o.w = (int)s.colInt(5);
      o.h = (int)s.colInt(6);
      o.z = (int)s.colInt(7);
      o.props = decodeProps(s.colText(8));
      maxObj = std::max(maxObj, o.id);
      if (Page* p = findPageMut(o.pageId)) p->objects.push_back(std::move(o));
    }
  }
  nextPageId_ = std::max(nextPageId_, maxPage + 1);
  nextObjId_ = std::max(nextObjId_, maxObj + 1);
}

// ------------------------------------------------------------------ lookups

const Page* Document::page(int64_t id) const {
  for (const Page& p : pages_)
    if (p.id == id) return &p;
  return nullptr;
}

Page* Document::findPageMut(int64_t id) {
  for (Page& p : pages_)
    if (p.id == id) return &p;
  return nullptr;
}

const Object* Document::object(int64_t id) const {
  for (const Page& p : pages_)
    for (const Object& o : p.objects)
      if (o.id == id) return &o;
  return nullptr;
}

int Document::maxZ(const Page& p) {
  int m = 0;
  for (const Object& o : p.objects) m = std::max(m, o.z);
  return m;
}
int Document::minZ(const Page& p) {
  int m = 0;
  for (const Object& o : p.objects) m = std::min(m, o.z);
  return m;
}

void Document::fireChanged() {
  if (onChange) onChange();
}

// ------------------------------------------------------- low-level mutation
// These touch both SQLite and the in-memory mirror. They must only be called
// inside a transaction (see commit / replay).

void Document::putObjectRaw(const Object& o) {
  Stmt s(db_,
         "INSERT OR REPLACE INTO object(id,page_id,type,x,y,w,h,z,props) VALUES(?,?,?,?,?,?,?,?,?)");
  s.bindInt(1, o.id).bindInt(2, o.pageId).bindInt(3, (int)o.type);
  s.bindInt(4, o.x).bindInt(5, o.y).bindInt(6, o.w).bindInt(7, o.h).bindInt(8, o.z);
  s.bindText(9, encodeProps(o.props));
  s.run();
  if (Page* p = findPageMut(o.pageId)) {
    auto it = std::find_if(p->objects.begin(), p->objects.end(),
                           [&](const Object& x) { return x.id == o.id; });
    if (it != p->objects.end()) *it = o;
    else p->objects.push_back(o);
    std::stable_sort(p->objects.begin(), p->objects.end(), byZ);
  }
  dirtyPages_.insert(o.pageId);
}

void Document::eraseObjectRaw(int64_t id, int64_t pageId) {
  Stmt s(db_, "DELETE FROM object WHERE id=?");
  s.bindInt(1, id).run();
  if (Page* p = findPageMut(pageId)) {
    p->objects.erase(std::remove_if(p->objects.begin(), p->objects.end(),
                                    [&](const Object& x) { return x.id == id; }),
                     p->objects.end());
  }
  dirtyPages_.insert(pageId);
}

void Document::applyPageState(const std::optional<Page>& st, int64_t id) {
  dirtyPages_.insert(id);
  if (!st) {
    Stmt s(db_, "DELETE FROM page WHERE id=?");
    s.bindInt(1, id).run();
    pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
                                [&](const Page& p) { return p.id == id; }),
                 pages_.end());
    return;
  }
  const Page& p = *st;
  Stmt s(db_,
         "INSERT OR REPLACE INTO page(id,title,w,h,bg,sort,tags,created,modified) "
         "VALUES(?,?,?,?,?,?,?,?,?)");
  s.bindInt(1, p.id).bindText(2, p.title).bindInt(3, p.w).bindInt(4, p.h).bindInt(5, p.bg);
  s.bindInt(6, p.sort).bindText(7, p.tags).bindInt(8, p.created).bindInt(9, p.modified);
  s.run();
  if (Page* m = findPageMut(p.id)) {
    std::vector<Object> keep = std::move(m->objects);
    *m = p;
    m->objects = std::move(keep);
  } else {
    pages_.push_back(headerOnly(p));
  }
  std::stable_sort(pages_.begin(), pages_.end(), [](const Page& a, const Page& b) {
    return a.sort != b.sort ? a.sort < b.sort : a.id < b.id;
  });
}

void Document::applyPageChange(const PageChange& c, bool forward) {
  const auto& state = forward ? c.after : c.before;
  const auto& other = forward ? c.before : c.after;
  int64_t id = state ? state->id : (other ? other->id : 0);
  applyPageState(state, id);
}

void Document::applyObjChange(const ObjChange& c, bool forward) {
  const auto& state = forward ? c.after : c.before;
  const auto& other = forward ? c.before : c.after;
  if (state) putObjectRaw(*state);
  else if (other) eraseObjectRaw(other->id, other->pageId);
}

void Document::runEntry(const UndoEntry& e, bool forward) {
  // Pages first, then objects, in both directions. Object operations tolerate
  // a missing page (page deleted forward) and a freshly restored empty page
  // (page restored backward) alike.
  for (const PageChange& c : e.pages) applyPageChange(c, forward);
  if (forward) {
    for (const ObjChange& c : e.objs) applyObjChange(c, true);
  } else {
    for (auto it = e.objs.rbegin(); it != e.objs.rend(); ++it) applyObjChange(*it, false);
  }
}

void Document::reindexFts(const Page& p) {
  {
    Stmt d(db_, "DELETE FROM fts WHERE page_id=?");
    d.bindInt(1, p.id).run();
  }
  std::string body;
  for (const Object& o : p.objects) {
    if (o.type == ObjType::Text) body += o.prop("text") + "\n";
    else if (o.type == ObjType::Button) body += o.prop("label") + "\n";
    else if (o.type == ObjType::Image) body += o.prop("name") + "\n";
    else if (o.type == ObjType::Table) body += o.prop("data") + "\n";
  }
  body += p.tags + "\n";  // so a plain-word search also finds pages by tag
  Stmt i(db_, "INSERT INTO fts(title,body,page_id) VALUES(?,?,?)");
  i.bindText(1, p.title).bindText(2, body).bindInt(3, p.id).run();
}

void Document::rebuildLinks(const Page& p) {
  {
    Stmt d(db_, "DELETE FROM link WHERE src_page=?");
    d.bindInt(1, p.id).run();
  }
  for (const Object& o : p.objects) {
    if (o.type != ObjType::Button) continue;
    int64_t target = o.propInt("target", 0);
    if (target <= 0) continue;
    Stmt i(db_, "INSERT INTO link(src_page,src_object,target_page) VALUES(?,?,?)");
    i.bindInt(1, p.id).bindInt(2, o.id).bindInt(3, target).run();
  }
}

void Document::finalizeDirty() {
  int64_t now = (int64_t)std::time(nullptr);
  for (int64_t id : dirtyPages_) {
    Page* p = findPageMut(id);
    if (!p) {
      Stmt a(db_, "DELETE FROM fts WHERE page_id=?");
      a.bindInt(1, id).run();
      Stmt b(db_, "DELETE FROM link WHERE src_page=?");
      b.bindInt(1, id).run();
      Stmt c(db_, "DELETE FROM object WHERE page_id=?");
      c.bindInt(1, id).run();
      continue;
    }
    p->modified = now;
    Stmt u(db_, "UPDATE page SET modified=? WHERE id=?");
    u.bindInt(1, now).bindInt(2, id).run();
    reindexFts(*p);
    rebuildLinks(*p);
  }
  dirtyPages_.clear();
}

// Runs fn inside a transaction. On any failure, rolls back and rebuilds the
// in-memory mirror from disk so memory can never drift from the file.
template <class Fn>
static void inTransaction(Db& db, Fn&& fn, std::function<void()> recover) {
  try {
    db.exec("BEGIN IMMEDIATE");
    fn();
    db.exec("COMMIT");
  } catch (...) {
    try {
      db.exec("ROLLBACK");
    } catch (...) {
    }
    recover();
    throw;
  }
}

void Document::commit(UndoEntry e) {
  inTransaction(
      db_,
      [&] {
        runEntry(e, true);
        finalizeDirty();
      },
      [&] {
        dirtyPages_.clear();
        loadAll();
      });
  stack_.resize(cursor_);
  stack_.push_back(std::move(e));
  if (stack_.size() > kMaxUndo) stack_.erase(stack_.begin());
  cursor_ = stack_.size();
  ++revision_;
  fireChanged();
}

bool Document::undo() {
  if (!canUndo()) return false;
  const UndoEntry& e = stack_[cursor_ - 1];
  inTransaction(
      db_, [&] { runEntry(e, false); finalizeDirty(); },
      [&] { dirtyPages_.clear(); loadAll(); });
  --cursor_;
  ++revision_;
  fireChanged();
  return true;
}

bool Document::redo() {
  if (!canRedo()) return false;
  const UndoEntry& e = stack_[cursor_];
  inTransaction(
      db_, [&] { runEntry(e, true); finalizeDirty(); },
      [&] { dirtyPages_.clear(); loadAll(); });
  ++cursor_;
  ++revision_;
  fireChanged();
  return true;
}

void Document::clearHistory() {
  stack_.clear();
  cursor_ = 0;
}

// --------------------------------------------------------------- public API

int64_t Document::addPage(const std::string& title) {
  Page p;
  p.id = nextPageId_++;
  p.title = title.empty() ? "Untitled" : title;
  p.sort = 0;
  for (const Page& q : pages_) p.sort = std::max(p.sort, q.sort + 1);
  p.created = p.modified = (int64_t)std::time(nullptr);
  UndoEntry e;
  e.label = "Add Page";
  e.pages.push_back({std::nullopt, p});
  commit(std::move(e));
  return p.id;
}

int64_t Document::addPageWithObjects(const std::string& title, int w, int h, const std::string& tags,
                                     std::vector<Object> protos, const char* label) {
  Page p;
  p.id = nextPageId_++;
  p.title = title.empty() ? "Untitled" : title;
  p.w = std::clamp(w, 200, 8000);
  p.h = std::clamp(h, 200, 8000);
  p.tags = normalizeTags(tags);
  p.sort = 0;
  for (const Page& q : pages_) p.sort = std::max(p.sort, q.sort + 1);
  p.created = p.modified = (int64_t)std::time(nullptr);
  UndoEntry e;
  e.label = label;
  e.pages.push_back({std::nullopt, p});
  int z = 0;
  for (Object& o : protos) {
    o.id = nextObjId_++;
    o.pageId = p.id;
    o.z = ++z;
    e.objs.push_back({std::nullopt, std::move(o)});
  }
  int64_t id = p.id;
  commit(std::move(e));
  return id;
}

bool Document::renamePage(int64_t id, const std::string& title, int* linksUpdated) {
  if (linksUpdated) *linksUpdated = 0;
  const Page* p = page(id);
  std::string t = title.empty() ? "Untitled" : title;
  if (!p || p->title == t) return false;
  Page after = headerOnly(*p);
  after.title = t;
  UndoEntry e;
  e.label = "Rename Page";
  e.pages.push_back({headerOnly(*p), after});

  bool writable = t.size() <= 120 && t.find_first_of("[]\r\n") == std::string::npos &&
                  t.find_first_not_of(" \t") != std::string::npos;
  int rewritten = 0;
  if (writable) {
    for (const Page& pg : pages_) {
      for (const Object& o : pg.objects) {
        if (o.type != ObjType::Text) continue;
        std::string text = o.prop("text");
        std::vector<WikiLink> wl = findWikiLinks(text);
        bool any = false;
        for (const WikiLink& w : wl) if (pageByTitle(w.title) == id) { any = true; break; }
        if (!any) continue;
        Runs runs = decodeRuns(o.prop("runs"), text.size());
        for (size_t k = wl.size(); k-- > 0;) {  // back to front so earlier offsets stay valid
          if (pageByTitle(wl[k].title) != id) continue;
          std::string next = text;
          next.replace(wl[k].start, wl[k].len, "[[" + t + "]]");
          shiftRunsForEdit(runs, text, next);
          text = std::move(next);
          ++rewritten;
        }
        Object a = o;
        a.setProp("text", text);
        a.setProp("runs", encodeRuns(runs));
        e.objs.push_back({o, a});
      }
    }
  }
  commit(std::move(e));
  if (linksUpdated) *linksUpdated = rewritten;
  return true;
}

bool Document::setPageProps(int64_t id, int w, int h, int bg, const std::string* tags) {
  const Page* p = page(id);
  w = std::clamp(w, 200, 8000);
  h = std::clamp(h, 200, 8000);
  std::string newTags = tags ? normalizeTags(*tags) : (p ? p->tags : std::string());
  if (!p || (p->w == w && p->h == h && p->bg == bg && p->tags == newTags)) return false;
  Page after = headerOnly(*p);
  after.w = w;
  after.h = h;
  after.bg = bg;
  after.tags = newTags;
  UndoEntry e;
  e.label = "Page Properties";
  e.pages.push_back({headerOnly(*p), after});
  commit(std::move(e));
  return true;
}

bool Document::movePage(int64_t id, int newIndex) {
  int from = -1;
  for (size_t i = 0; i < pages_.size(); ++i)
    if (pages_[i].id == id) from = (int)i;
  if (from < 0) return false;
  int to = std::clamp(newIndex, 0, (int)pages_.size() - 1);
  if (to == from) return false;
  std::vector<int64_t> order;
  for (const Page& p : pages_) order.push_back(p.id);
  order.erase(order.begin() + from);
  order.insert(order.begin() + to, id);
  UndoEntry e;
  e.label = "Move Page";
  for (size_t i = 0; i < order.size(); ++i) {
    const Page* p = page(order[i]);
    if (p->sort == (int)i) continue;
    Page after = headerOnly(*p);
    after.sort = (int)i;  // renumber densely; only pages whose number changed are touched
    e.pages.push_back({headerOnly(*p), after});
  }
  if (e.pages.empty()) return false;
  commit(std::move(e));
  return true;
}

bool Document::deletePage(int64_t id) {
  const Page* p = page(id);
  if (!p || pages_.size() <= 1) return false;
  UndoEntry e;
  e.label = "Delete Page";
  e.pages.push_back({headerOnly(*p), std::nullopt});
  for (const Object& o : p->objects) e.objs.push_back({o, std::nullopt});
  commit(std::move(e));
  return true;
}

int64_t Document::addObject(int64_t pageId, Object proto, const char* label) {
  auto ids = addObjects(pageId, {std::move(proto)}, label);
  return ids.empty() ? 0 : ids[0];
}

namespace {
int maxGroup(const Page& p) {
  int m = 0;
  for (const Object& o : p.objects) m = std::max(m, o.propInt("group", 0));
  return m;
}
}  // namespace

std::vector<int64_t> Document::addObjects(int64_t pageId, std::vector<Object> protos,
                                          const char* label) {
  const Page* p = page(pageId);
  if (!p || protos.empty()) return {};
  int z = maxZ(*p);
  {  // copies get fresh group numbers (so a pasted copy never joins the original's group); a group that
     // arrives with a single member is no group at all
    std::map<int, int> fresh, count;
    for (const Object& o : protos)
      if (int g = o.propInt("group", 0)) ++count[g];
    int next = maxGroup(*p);
    for (Object& o : protos) {
      int g = o.propInt("group", 0);
      if (!g) continue;
      if (count[g] < 2) {
        o.props.erase("group");
      } else {
        if (!fresh.count(g)) fresh[g] = ++next;
        o.setPropInt("group", fresh[g]);
      }
    }
  }
  UndoEntry e;
  e.label = label;
  std::vector<int64_t> ids;
  for (Object& o : protos) {
    o.id = nextObjId_++;
    o.pageId = pageId;
    o.z = ++z;
    ids.push_back(o.id);
    e.objs.push_back({std::nullopt, std::move(o)});
  }
  commit(std::move(e));
  return ids;
}

bool Document::updateObjects(const std::vector<Object>& afters, const char* label) {
  UndoEntry e;
  e.label = label;
  for (const Object& a : afters) {
    const Object* b = object(a.id);
    if (!b || *b == a) continue;
    Object fixed = a;
    fixed.pageId = b->pageId;  // moving between pages is not supported here
    e.objs.push_back({*b, fixed});
  }
  if (e.objs.empty()) return false;
  commit(std::move(e));
  return true;
}

bool Document::deleteObjects(int64_t pageId, const std::vector<int64_t>& ids, const char* label) {
  const Page* p = page(pageId);
  if (!p) return false;
  UndoEntry e;
  e.label = label;
  for (const Object& o : p->objects)
    if (std::find(ids.begin(), ids.end(), o.id) != ids.end()) e.objs.push_back({o, std::nullopt});
  if (e.objs.empty()) return false;
  commit(std::move(e));
  return true;
}

bool Document::reorder(int64_t pageId, const std::vector<int64_t>& ids, bool toFront) {
  const Page* p = page(pageId);
  if (!p) return false;
  std::vector<Object> sel;
  int othersMax = 0, othersMin = 0;
  bool anyOther = false;
  for (const Object& o : p->objects) {
    if (std::find(ids.begin(), ids.end(), o.id) != ids.end()) {
      sel.push_back(o);
    } else {
      othersMax = anyOther ? std::max(othersMax, o.z) : o.z;
      othersMin = anyOther ? std::min(othersMin, o.z) : o.z;
      anyOther = true;
    }
  }
  if (sel.empty() || !anyOther) return false;
  std::stable_sort(sel.begin(), sel.end(), byZ);
  int n = (int)sel.size();
  for (int k = 0; k < n; ++k) sel[k].z = toFront ? othersMax + 1 + k : othersMin - n + k;
  return updateObjects(sel, toFront ? "Bring to Front" : "Send to Back");
}

bool Document::group(int64_t pageId, const std::vector<int64_t>& ids) {
  const Page* p = page(pageId);
  if (!p) return false;
  std::vector<Object> changed;
  for (const Object& o : p->objects)
    if (std::find(ids.begin(), ids.end(), o.id) != ids.end()) changed.push_back(o);
  if (changed.size() < 2) return false;
  int g = maxGroup(*p) + 1;
  for (Object& o : changed) o.setPropInt("group", g);
  // Anything that was grouped with a member but is not itself selected leaves that old group: members of a
  // group are always exactly the objects sharing its number, so the old group simply shrinks.
  return updateObjects(changed, "Group");
}

bool Document::ungroup(int64_t pageId, const std::vector<int64_t>& ids) {
  const Page* p = page(pageId);
  if (!p) return false;
  std::vector<int> gs;
  for (const Object& o : p->objects)
    if (std::find(ids.begin(), ids.end(), o.id) != ids.end() && o.propInt("group", 0) != 0) gs.push_back(o.propInt("group", 0));
  std::vector<Object> changed;
  for (const Object& o : p->objects)
    if (o.propInt("group", 0) != 0 && std::find(gs.begin(), gs.end(), o.propInt("group", 0)) != gs.end()) {
      Object c = o;
      c.props.erase("group");
      changed.push_back(c);
    }
  return !changed.empty() && updateObjects(changed, "Ungroup");
}

std::vector<int64_t> Document::groupMates(int64_t objectId) const {
  const Object* o = object(objectId);
  if (!o) return {};
  int g = o->propInt("group", 0);
  if (g == 0) return {objectId};
  const Page* p = page(o->pageId);
  std::vector<int64_t> out;
  if (p)
    for (const Object& m : p->objects)
      if (m.propInt("group", 0) == g) out.push_back(m.id);
  return out.empty() ? std::vector<int64_t>{objectId} : out;
}

bool Document::reorderStep(int64_t pageId, const std::vector<int64_t>& ids, bool forward) {
  const Page* p = page(pageId);
  if (!p) return false;
  std::vector<Object> stack = p->objects;  // already in stacking order, bottom first
  std::stable_sort(stack.begin(), stack.end(), byZ);
  auto isSel = [&](const Object& o) { return std::find(ids.begin(), ids.end(), o.id) != ids.end(); };
  bool moved = false;
  int n = (int)stack.size();
  if (forward) {
    for (int i = n - 2; i >= 0; --i)
      if (isSel(stack[i]) && !isSel(stack[i + 1])) std::swap(stack[i], stack[i + 1]), moved = true;
  } else {
    for (int i = 1; i < n; ++i)
      if (isSel(stack[i]) && !isSel(stack[i - 1])) std::swap(stack[i], stack[i - 1]), moved = true;
  }
  if (!moved) return false;
  int base = p->objects.empty() ? 0 : p->objects.front().z;  // keep the page's z range where it was
  std::vector<Object> changed;
  for (int i = 0; i < n; ++i)
    if (stack[i].z != base + i) {
      stack[i].z = base + i;
      changed.push_back(stack[i]);
    }
  return !changed.empty() && updateObjects(changed, forward ? "Bring Forward" : "Send Backward");
}

// ------------------------------------------------------------------ queries

std::vector<int64_t> Document::search(const std::string& text) const {
  std::vector<std::string> words;
  {
    std::string cur;
    for (char c : text) {
      if (std::isspace((unsigned char)c)) {
        if (!cur.empty()) words.push_back(cur), cur.clear();
      } else if (c != '"') {
        cur += c;
      }
    }
    if (!cur.empty()) words.push_back(cur);
  }
  // "#tag" words filter by tag (prefix match, all must match); the rest is full-text search.
  std::vector<std::string> tagTerms, textWords;
  for (const std::string& w : words) {
    if (w[0] == '#') {
      if (w.size() > 1) tagTerms.push_back(lower(w.substr(1)));
    } else {
      textWords.push_back(w);
    }
  }
  if (!tagTerms.empty()) {
    std::string rest;
    for (const std::string& w : textWords) rest += (rest.empty() ? "" : " ") + w;
    std::vector<int64_t> out;
    for (int64_t id : search(rest)) {
      const Page* p = page(id);
      if (!p) continue;
      auto have = splitTags(p->tags);
      bool all = true;
      for (const std::string& term : tagTerms) {
        bool found = false;
        for (const std::string& t : have)
          if (lower(t).compare(0, term.size(), term) == 0) { found = true; break; }
        if (!found) { all = false; break; }
      }
      if (all) out.push_back(id);
    }
    return out;
  }
  std::vector<int64_t> out;
  if (words.empty()) {
    for (const Page& p : pages_) out.push_back(p.id);
    return out;
  }

  std::set<int64_t> hit;
  std::string q;
  for (const std::string& w : words) q += "\"" + w + "\"* ";
  try {
    Stmt s(db_, "SELECT page_id FROM fts WHERE fts MATCH ?");
    s.bindText(1, q);
    while (s.step()) hit.insert(s.colInt(0));
  } catch (const DbError&) {
    // Odd punctuation can make FTS reject the query; the title match below still works.
  }
  std::string needle = lower(text);
  for (const Page& p : pages_)
    if (lower(p.title).find(needle) != std::string::npos) hit.insert(p.id);

  for (const Page& p : pages_)
    if (hit.count(p.id)) out.push_back(p.id);
  return out;
}

std::vector<std::pair<std::string, int>> Document::allTags() const {
  std::map<std::string, std::pair<std::string, int>> m;  // lowercase -> (display, count)
  for (const Page& p : pages_)
    for (const std::string& t : splitTags(p.tags)) {
      std::string k = lower(t);
      auto it = m.find(k);
      if (it == m.end()) m.emplace(k, std::make_pair(t, 1));
      else ++it->second.second;
    }
  std::vector<std::pair<std::string, int>> out;
  for (auto& kv : m) out.push_back(kv.second);
  return out;
}

std::vector<std::pair<int64_t, std::string>> Document::backlinks(int64_t pageId) const {
  std::vector<std::pair<int64_t, std::string>> out;
  Stmt s(db_, "SELECT DISTINCT src_page FROM link WHERE target_page=?");
  s.bindInt(1, pageId);
  std::set<int64_t> ids;
  while (s.step()) ids.insert(s.colInt(0));
  const Page* me = page(pageId);
  if (me) {  // [[Title]] mentions: scanned, so a link made before its page existed still counts
    for (const Page& p : pages_) {
      if (ids.count(p.id) || p.id == pageId) continue;
      for (const Object& o : p.objects) {
        if (o.type != ObjType::Text) continue;
        bool hit = false;
        for (const WikiLink& w : findWikiLinks(o.prop("text")))
          if (pageByTitle(w.title) == pageId) { hit = true; break; }
        if (hit) { ids.insert(p.id); break; }
      }
    }
  }
  for (const Page& p : pages_)
    if (ids.count(p.id) && p.id != pageId) out.emplace_back(p.id, p.title);
  return out;
}

int64_t Document::pageByTitle(const std::string& title) const {
  auto fold = [](std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  std::string want = fold(title);
  for (const Page& p : pages_)
    if (fold(p.title) == want) return p.id;
  return 0;
}

void Document::saveCopy(const std::string& path) const {
  namespace fs = std::filesystem;
  if (fs::exists(fs::u8path(path))) throw DbError("File already exists: " + path);
  std::string q;
  for (char c : path) {
    if (c == '\'') q += '\'';
    q += c;
  }
  db_.exec(("VACUUM INTO '" + q + "'").c_str());
}

// -------------------------------------------------------------------- blobs

int64_t Document::addBlob(const std::string& data, const std::string& mime, const std::string& name) {
  if (data.empty()) throw DbError("Empty attachment");
  std::string hash = sha256Hex(data);
  {
    Stmt s(db_, "SELECT id FROM blob WHERE hash=?");
    s.bindText(1, hash);
    if (s.step()) return s.colInt(0);
  }
  Stmt i(db_, "INSERT INTO blob(hash,mime,name,data) VALUES(?,?,?,?)");
  i.bindText(1, hash).bindText(2, mime).bindText(3, name).bindBlob(4, data);
  i.run();
  return sqlite3_last_insert_rowid(db_.handle());
}

bool Document::blob(int64_t id, std::string& data, std::string* mime, std::string* name) const {
  Stmt s(db_, "SELECT data,mime,name FROM blob WHERE id=?");
  s.bindInt(1, id);
  if (!s.step()) return false;
  data = s.colBlob(0);
  if (mime) *mime = s.colText(1);
  if (name) *name = s.colText(2);
  return true;
}

// ------------------------------------------------- clipboard / transfer format
// "CAIRNOBJ1\n" then, per object, a header line
//   O <type> <x> <y> <w> <h> <propsLen> <blobLen> <mimeLen> <nameLen>\n
// followed by exactly that many raw bytes of props, blob, mime and name.
// Length-prefixed so nothing in the data can be mistaken for structure.

namespace {
constexpr const char* kMagic = "CAIRNOBJ1\n";
constexpr size_t kMaxPayload = 512u * 1024 * 1024;
}  // namespace

std::string Document::exportObjects(const std::vector<int64_t>& ids) const {
  std::string out = kMagic;
  for (int64_t id : ids) {
    const Object* o = object(id);
    if (!o) continue;
    std::string props = encodeProps(o->props), data, mime, name;
    if (o->type == ObjType::Image || o->type == ObjType::Audio) blob(o->propInt("blob", 0), data, &mime, &name);
    out += "O " + std::to_string((int)o->type) + " " + std::to_string(o->x) + " " +
           std::to_string(o->y) + " " + std::to_string(o->w) + " " + std::to_string(o->h) + " " +
           std::to_string(props.size()) + " " + std::to_string(data.size()) + " " +
           std::to_string(mime.size()) + " " + std::to_string(name.size()) + "\n";
    out += props + data + mime + name;
  }
  return out;
}

std::vector<int64_t> Document::importObjects(int64_t pageId, const std::string& payload, int dx,
                                             int dy, const char* label) {
  struct Rec {
    Object o;
    std::string data, mime, name;
  };
  std::vector<Rec> recs;
  size_t magicLen = std::strlen(kMagic);
  if (!page(pageId) || payload.size() < magicLen || payload.size() > kMaxPayload ||
      payload.compare(0, magicLen, kMagic) != 0)
    return {};
  size_t pos = magicLen;
  while (pos < payload.size()) {
    size_t nl = payload.find('\n', pos);
    if (nl == std::string::npos || nl - pos > 200) return {};
    std::string line = payload.substr(pos, nl - pos);
    pos = nl + 1;
    int type = 0, x = 0, y = 0, w = 0, h = 0;
    unsigned long long pl = 0, bl = 0, ml = 0, nlen = 0;
    char tag = 0;
    if (std::sscanf(line.c_str(), "%c %d %d %d %d %d %llu %llu %llu %llu", &tag, &type, &x, &y, &w,
                    &h, &pl, &bl, &ml, &nlen) != 10 ||
        tag != 'O' || type < 1 || type > 8)
      return {};
    unsigned long long total = pl + bl + ml + nlen;
    if (total < pl || total > payload.size() - pos) return {};  // overflow or truncated
    Rec r;
    r.o.type = (ObjType)type;
    r.o.x = x; r.o.y = y; r.o.w = std::max(w, 8); r.o.h = std::max(h, 8);
    r.o.props = decodeProps(payload.substr(pos, (size_t)pl));
    pos += (size_t)pl;
    r.data = payload.substr(pos, (size_t)bl);
    pos += (size_t)bl;
    r.mime = payload.substr(pos, (size_t)ml);
    pos += (size_t)ml;
    r.name = payload.substr(pos, (size_t)nlen);
    pos += (size_t)nlen;
    recs.push_back(std::move(r));
  }
  if (recs.empty()) return {};

  std::vector<Object> objs;
  for (Rec& r : recs) {
    if (r.o.type == ObjType::Image) {
      if (r.data.empty()) continue;  // an image with no bytes is unusable
      r.o.setPropInt("blob", (long)addBlob(r.data, r.mime, r.name));
    } else if (r.o.type == ObjType::Audio) {
      AudioKind ak;
      if (!sniffAudio(r.data, ak)) continue;  // pasted bytes that are not audio are dropped, never stored
      r.o.setProp("ext", ak.ext);
      r.o.setPropInt("blob", (long)addBlob(r.data, ak.mime, r.name));
    }
    r.o.x += dx;
    r.o.y += dy;
    objs.push_back(std::move(r.o));
  }
  return addObjects(pageId, std::move(objs), label);
}

// ------------------------------------------------------------------ starter

void Document::seedStarter() {
  int64_t home = addPage("Welcome");
  int64_t tour = addPage("Quick tour");
  int64_t scripts = addPage("Buttons and scripts");

  auto text = [](int x, int y, int w, int h, const char* t, int size, bool bold, int color) {
    Object o;
    o.type = ObjType::Text;
    o.x = x; o.y = y; o.w = w; o.h = h;
    o.setProp("text", t);
    o.setPropInt("size", size);
    o.setPropInt("bold", bold);
    o.setPropInt("color", color);
    return o;
  };
  auto button = [](int x, int y, int w, int h, const char* label, int64_t target) {
    Object o;
    o.type = ObjType::Button;
    o.x = x; o.y = y; o.w = w; o.h = h;
    o.setProp("label", label);
    o.setPropInt("target", (long)target);
    return o;
  };
  auto panel = [](int x, int y, int w, int h, int fill, int line) {
    Object o;
    o.type = ObjType::Rect;
    o.x = x; o.y = y; o.w = w; o.h = h;
    o.setPropInt("fill", fill);
    o.setPropInt("line", line);
    o.setPropInt("lw", 1);
    o.setPropInt("round", 6);
    return o;
  };

  addObjects(home,
             {panel(40, 40, 880, 120, 0xEEF3F8, 0x9DB3C9),
              text(60, 52, 840, 48, "Welcome to Cairn", 28, true, 0x1F3A5F),
              text(60, 104, 840, 48,
                   "Every page is a canvas. Put text, buttons and shapes anywhere, then link pages "
                   "together.",
                   13, false, 0x33414F),
              text(40, 190, 880, 190,
                   "Two modes\n"
                   "  Edit mode (Ctrl+E): click to select, drag to move, drag handles to resize, "
                   "double-click text to type.\n"
                   "  Run mode (Ctrl+E again): buttons work, and nothing moves by accident.\n\n"
                   "Everything saves the moment you make it. There is no Save step, and Ctrl+Z "
                   "undoes almost anything.",
                   14, false, 0x222222),
              button(40, 420, 200, 44, "Take the quick tour", tour)},
             "Starter");
  // Two real columns of separate objects: spaces can't align text in a proportional font.
  struct Row { const char* what; const char* how; };
  const Row rows[] = {
      {"Add objects", "Edit menu, or Ctrl+Shift+T / B / R for text, button, rectangle"},
      {"Move and resize", "Drag an object; drag a handle to resize. Arrow keys nudge (Shift = 10)"},
      {"Edit text", "Double-click text, or press F2. Ctrl+Enter finishes"},
      {"Duplicate", "Ctrl+D"},
      {"Link pages", "Double-click a button and choose its target page"},
      {"Find a page", "Ctrl+F, then type. Search looks inside every page, not just titles"},
      {"Backlinks", "The panel under the page list shows which pages link here"},
      {"Undo", "Ctrl+Z and Ctrl+Y, even for deleted pages"},
  };
  std::vector<Object> tourObjs;
  tourObjs.push_back(text(40, 30, 880, 44, "Quick tour", 26, true, 0x1F3A5F));
  int ry = 96;
  for (const Row& r : rows) {
    tourObjs.push_back(text(40, ry, 190, 36, r.what, 14, true, 0x1F3A5F));
    tourObjs.push_back(text(240, ry, 680, 36, r.how, 14, false, 0x222222));
    ry += 44;
  }
  tourObjs.push_back(button(40, 470, 200, 44, "Back to Welcome", home));
  tourObjs.push_back(button(260, 470, 240, 44, "Buttons and scripts", scripts));
  addObjects(tour, std::move(tourObjs), "Starter");

  // A scripting page that explains itself: the buttons on it really run the scripts it describes.
  auto scriptButton = [](int x, int y, int w, int h, const char* label, const char* src) {
    Object o;
    o.type = ObjType::Button;
    o.x = x; o.y = y; o.w = w; o.h = h;
    o.setProp("label", label);
    o.setProp("script", src);
    return o;
  };
  auto named = [](Object o, const char* name) {
    o.setProp("sname", name);
    return o;
  };
  std::vector<Object> so;
  so.push_back(text(40, 30, 880, 44, "Buttons and scripts", 26, true, 0x1F3A5F));
  so.push_back(text(40, 84, 880, 150,
                    "A script is a few lines of instructions you attach to a button. When you click the "
                    "button in Run mode (Ctrl+E), Cairn follows the lines one by one, top to bottom.\n\n"
                    "To write one: switch to Edit mode, double-click a button, and type lines into the "
                    "Script box. Each line is one instruction. Lines starting with # are notes to yourself.",
                    14, false, 0x222222));
  so.push_back(text(40, 250, 420, 28, "Try it: a counter", 16, true, 0x1F3A5F));
  so.push_back(named(text(40, 286, 420, 34, "Not clicked yet", 16, false, 0x222222), "Counter"));
  so.push_back(scriptButton(40, 330, 200, 44, "Click me",
                            "set n = n + 1\ntext \"Counter\" = \"Clicked \" + n + \" times\"\n"
                            "if n >= 3 then say \"That's three!\""));
  so.push_back(text(500, 250, 420, 28, "What the script says", 16, true, 0x1F3A5F));
  so.push_back(text(500, 286, 420, 120,
                    "set n = n + 1        add one to a counter named n\n"
                    "text \"Counter\" = ...  change the text box whose Script name is Counter\n"
                    "if n >= 3 then ...   only do the rest of the line when n is 3 or more",
                    13, false, 0x33414F));
  so.push_back(text(40, 410, 880, 28, "Everything a script can do", 16, true, 0x1F3A5F));
  so.push_back(text(40, 446, 880, 190,
                    "goto \"Page title\"   go to a page (also: goto next, prev, first, last, and back)\n"
                    "set x = ...          remember a number or text (+ joins text)\n"
                    "text \"Name\" = ...   change a text box (give it a Script name in its Properties)\n"
                    "play \"Name\"  /  stop   start or stop an audio clip\n"
                    "open \"address\"      open a web address or file\n"
                    "say ...              show a message box\n\n"
                    "A script can also belong to the page: Edit > Add On-Open Script. It runs every time the page "
                    "is opened in Run mode (handy for resetting a counter or jumping somewhere).\n\n"
                    "Scripts are deliberately small and safe: no loops, no changing files, no network "
                    "access beyond opening what a normal link button would open.",
                    13, false, 0x222222));
  so.push_back(button(40, 660, 200, 44, "Back to the tour", tour));
  addObjects(scripts, std::move(so), "Starter");
  clearHistory();
}

Document::ReplaceResult Document::replaceAll(const std::string& find, const std::string& repl, bool matchCase,
                                             int64_t onlyPage, bool dryRun) {
  ReplaceResult r;
  if (find.empty()) return r;
  std::vector<Object> changed;
  for (const Page& p : pages_) {
    if (onlyPage && p.id != onlyPage) continue;
    bool pageHit = false;
    for (const Object& o : p.objects) {
      const char* key = o.type == ObjType::Text ? "text" : o.type == ObjType::Button ? "label" : nullptr;
      if (!key) continue;
      std::string text = o.prop(key);
      Runs runs = o.type == ObjType::Text ? decodeRuns(o.prop("runs"), text.size()) : Runs{};
      int n = replaceInText(text, runs, find, repl, matchCase);
      if (!n) continue;
      r.matches += n;
      ++r.objects;
      pageHit = true;
      if (dryRun) continue;
      Object a = o;
      a.setProp(key, text);
      if (o.type == ObjType::Text) a.setProp("runs", encodeRuns(runs));
      changed.push_back(a);
    }
    if (pageHit) ++r.pages;
  }
  if (!dryRun && !changed.empty()) updateObjects(changed, "Replace All");
  return r;
}

}  // namespace cairn

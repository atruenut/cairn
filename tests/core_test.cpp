// Command-line tests for the UI-free core. Builds and runs on Linux or Windows.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "document.h"
#include "rich.h"
#include "export.h"
#include "markdown.h"
#include "table.h"
#include "media.h"
#include "urls.h"
#include "sha256.h"

using namespace cairn;

static int g_fail = 0, g_run = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_run;                                                              \
    if (!(c)) {                                                           \
      ++g_fail;                                                           \
      std::printf("  FAIL line %d: %s\n", __LINE__, #c);                  \
    }                                                                     \
  } while (0)

static Object textObj(int x, int y, const std::string& t) {
  Object o;
  o.type = ObjType::Text;
  o.x = x; o.y = y; o.w = 100; o.h = 30;
  o.setProp("text", t);
  return o;
}

static std::string tmpPath(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p.string();
}

static void testProps() {
  std::printf("props round-trip\n");
  PropMap m;
  m["text"] = "line1\nline2\\with\rslash";
  m["size"] = "14";
  PropMap back = decodeProps(encodeProps(m));
  CHECK(back == m);
}

static void testBasicsAndPersistence() {
  std::printf("create / reopen\n");
  std::string path = tmpPath("cairn_t1.cairn");
  int64_t pid;
  {
    auto d = Document::create(path, true);
    CHECK(d->pages().size() == 2);
    CHECK(!d->canUndo());  // seeding must not be undoable
    pid = d->addPage("Notes");
    int64_t oid = d->addObject(pid, textObj(10, 10, "hello world"));
    CHECK(oid > 0);
    CHECK(d->canUndo());
  }
  {
    auto d = Document::open(path);
    CHECK(d->pages().size() == 3);
    const Page* p = d->page(pid);
    CHECK(p && p->title == "Notes");
    CHECK(p && p->objects.size() == 1);
    CHECK(p && p->objects[0].prop("text") == "hello world");
    CHECK(!d->canUndo());  // history is per-session
  }
  std::filesystem::remove(path);
}

static void testUndoRedo() {
  std::printf("undo / redo\n");
  std::string path = tmpPath("cairn_t2.cairn");
  auto d = Document::create(path, false);
  int64_t p1 = d->addPage("One");
  Object a = textObj(0, 0, "a");
  int64_t ida = d->addObject(p1, a);
  int64_t idb = d->addObject(p1, textObj(5, 5, "b"));

  // update
  Object moved = *d->object(ida);
  moved.x = 77;
  CHECK(d->updateObject(moved, "Move"));
  CHECK(d->object(ida)->x == 77);
  CHECK(!d->updateObject(moved, "Move"));  // no-op must not push history
  CHECK(d->undo());
  CHECK(d->object(ida)->x == 0);
  CHECK(d->redo());
  CHECK(d->object(ida)->x == 77);

  // delete then undo restores
  CHECK(d->deleteObjects(p1, {ida}));
  CHECK(!d->object(ida));
  CHECK(d->undo());
  CHECK(d->object(ida) && d->object(ida)->x == 77);

  // redo is discarded by a new action
  CHECK(d->undo());  // undo the update-of-x? (stack: add p, add a, add b, move, delete)
  CHECK(d->canRedo());
  Object other = *d->object(idb);
  other.y = 99;
  CHECK(d->updateObject(other, "Move"));
  CHECK(!d->canRedo());

  // delete a whole page with objects, undo restores objects too
  size_t objsBefore = d->page(p1)->objects.size();
  int64_t p2 = d->addPage("Two");
  CHECK(d->deletePage(p1));
  CHECK(!d->page(p1));
  CHECK(d->undo());
  CHECK(d->page(p1) && d->page(p1)->objects.size() == objsBefore);
  CHECK(d->redo());
  CHECK(!d->page(p1));
  CHECK(d->page(p2));
  CHECK(!d->deletePage(p2));  // last page cannot be deleted

  // everything must match what a fresh load sees
  d->undo();  // restore p1
  std::string path2 = path;
  size_t n = d->pages().size();
  size_t o1 = d->page(p1)->objects.size();
  d.reset();
  auto d2 = Document::open(path2);
  CHECK(d2->pages().size() == n);
  CHECK(d2->page(p1) && d2->page(p1)->objects.size() == o1);
  std::filesystem::remove(path);
}

static void testRedoDiscardedProperly() {
  std::printf("redo history is discarded, not buried\n");
  std::string path = tmpPath("cairn_t2b.cairn");
  auto d = Document::create(path, false);
  int64_t p = d->addPage("R");
  int64_t id = d->addObject(p, textObj(0, 0, "a"));
  auto setX = [&](int x) {
    Object o = *d->object(id);
    o.x = x;
    d->updateObject(o, "Move");
  };
  setX(10);
  setX(20);
  d->undo();  // x back to 10; the x=20 step is now redo-able
  setX(50);   // must throw away the x=20 step
  CHECK(!d->canRedo());
  d->undo();  // reverts the 50
  CHECK(d->object(id)->x == 10);
  d->undo();  // must revert the 10, NOT a stale entry for the discarded 20
  CHECK(d->object(id)->x == 0);
  d->undo();  // reverts the object add
  CHECK(!d->object(id));
  std::filesystem::remove(path);
}

static void testZOrder() {
  std::printf("z-order\n");
  std::string path = tmpPath("cairn_t3.cairn");
  auto d = Document::create(path, false);
  int64_t p = d->addPage("Z");
  int64_t a = d->addObject(p, textObj(0, 0, "a"));
  int64_t b = d->addObject(p, textObj(0, 0, "b"));
  int64_t c = d->addObject(p, textObj(0, 0, "c"));
  CHECK(d->page(p)->objects[0].id == a && d->page(p)->objects[2].id == c);
  CHECK(d->reorder(p, {a}, true));
  CHECK(d->page(p)->objects.back().id == a);
  CHECK(d->reorder(p, {a}, false));
  CHECK(d->page(p)->objects.front().id == a);
  CHECK(!d->reorder(p, {a, b, c}, true));  // everything selected: nothing to do
  d->undo();
  d->undo();
  CHECK(d->page(p)->objects.front().id == a && d->page(p)->objects.back().id == c);
  std::filesystem::remove(path);
}

static void testSearchAndLinks() {
  std::printf("search / backlinks\n");
  std::string path = tmpPath("cairn_t4.cairn");
  auto d = Document::create(path, false);
  int64_t p1 = d->addPage("Recipes");
  int64_t p2 = d->addPage("Garden");
  d->addObject(p1, textObj(0, 0, "Sourdough starter and flour"));
  d->addObject(p2, textObj(0, 0, "Tomatoes need staking"));

  auto r = d->search("flour");
  CHECK(r.size() == 1 && r[0] == p1);
  r = d->search("tomat");  // prefix
  CHECK(r.size() == 1 && r[0] == p2);
  r = d->search("gard");  // title substring
  CHECK(r.size() == 1 && r[0] == p2);
  r = d->search("\"unbalanced");  // must not throw
  CHECK(r.empty());
  CHECK(d->search("").size() == 2);

  Object btn;
  btn.type = ObjType::Button;
  btn.w = 100; btn.h = 30;
  btn.setProp("label", "to garden");
  btn.setPropInt("target", (long)p2);
  int64_t bid = d->addObject(p1, btn);
  auto bl = d->backlinks(p2);
  CHECK(bl.size() == 1 && bl[0].first == p1);
  CHECK(d->search("garden").size() == 2);  // label text is indexed too

  d->deleteObjects(p1, {bid});
  CHECK(d->backlinks(p2).empty());
  d->undo();
  CHECK(d->backlinks(p2).size() == 1);

  // editing text must refresh the index
  Object t = d->page(p2)->objects[0];
  t.setProp("text", "Peppers instead");
  d->updateObject(t, "Edit");
  CHECK(d->search("tomat").empty());
  CHECK(d->search("pepper").size() == 1);
  std::filesystem::remove(path);
}

static void testSafety() {
  std::printf("error handling\n");
  std::string bad = tmpPath("cairn_bad.cairn");
  FILE* f = std::fopen(bad.c_str(), "wb");
  std::fputs("this is definitely not a database, just some text padding padding padding", f);
  std::fclose(f);
  bool threw = false;
  try { Document::open(bad); } catch (const DbError&) { threw = true; }
  CHECK(threw);
  std::filesystem::remove(bad);

  threw = false;
  try { Document::open(tmpPath("cairn_missing.cairn")); } catch (const DbError&) { threw = true; }
  CHECK(threw);

  // A plain, valid SQLite file that is not a Cairn must be refused too.
  std::string other = tmpPath("cairn_other.sqlite");
  {
    Db db;
    db.open(other, true);
    db.exec("CREATE TABLE x(a)");
  }
  threw = false;
  try { Document::open(other); } catch (const DbError&) { threw = true; }
  CHECK(threw);
  std::filesystem::remove(other);

  // saveCopy produces an openable file and refuses to overwrite.
  std::string p1 = tmpPath("cairn_src.cairn"), p2 = tmpPath("cairn_copy.cairn");
  auto d = Document::create(p1, true);
  d->saveCopy(p2);
  auto c = Document::open(p2);
  CHECK(c->pages().size() == d->pages().size());
  threw = false;
  try { d->saveCopy(p2); } catch (const DbError&) { threw = true; }
  CHECK(threw);
  c.reset(); d.reset();
  std::filesystem::remove(p1);
  std::filesystem::remove(p2);
}


static Object imageObj(int64_t blobId, const char* name) {
  Object o;
  o.type = ObjType::Image;
  o.w = 200; o.h = 100;
  o.setPropInt("blob", (long)blobId);
  o.setProp("name", name);
  return o;
}

static void testSha256() {
  std::printf("sha256 official vectors\n");
  CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");  // two-block case
  CHECK(sha256Hex(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");  // long input
}

static void testBlobs() {
  std::printf("blobs: dedupe, binary safety, orphan sweep\n");
  std::string path = tmpPath("cairn_blob.cairn");
  std::string bin("\x00\x01\xff\xfe PNG\x00data\n\r\x00", 17);  // NULs and newlines must survive
  int64_t keep, page;
  {
    auto d = Document::create(path, false);
    page = d->addPage("Pics");
    keep = d->addBlob(bin, "image/png", "a.png");
    int64_t again = d->addBlob(bin, "image/png", "copy-of-a.png");
    CHECK(keep == again);  // same content, one row
    int64_t other = d->addBlob("different bytes", "image/png", "b.png");
    CHECK(other != keep);
    d->addObject(page, imageObj(keep, "a.png"));
    // `other` is never referenced by any object
    std::string got, mime, name;
    CHECK(d->blob(keep, got, &mime, &name));
    CHECK(got == bin && mime == "image/png" && name == "a.png");
    CHECK(!d->blob(9999, got));
    bool threw = false;
    try { d->addBlob("", "x", "y"); } catch (const DbError&) { threw = true; }
    CHECK(threw);
  }
  {
    auto d = Document::open(path);  // sweep runs on open
    std::string got;
    CHECK(d->blob(keep, got) && got == bin);  // referenced: kept
    int64_t reAdded = d->addBlob("different bytes", "image/png", "b.png");
    CHECK(reAdded != keep);
    CHECK(d->search("a.png").size() == 1);  // image file names are searchable
  }
  {
    Document::open(path);  // `reAdded` was never referenced either, so this open sweeps it
    Db raw;
    raw.open(path, false);
    Stmt s(raw, "SELECT COUNT(*) FROM blob");
    s.step();
    CHECK(s.colInt(0) == 1);  // only the referenced blob is left
  }
  std::filesystem::remove(path);
}

static void testTransfer() {
  std::printf("export / import across documents\n");
  std::string pa = tmpPath("cairn_xa.cairn"), pb = tmpPath("cairn_xb.cairn");
  auto a = Document::create(pa, false);
  auto b = Document::create(pb, false);
  int64_t pageA = a->addPage("A"), pageB = b->addPage("B");
  std::string img("\x89PNG\r\n\x1a\n binary \x00\x00 tail", 24);
  int64_t blobA = a->addBlob(img, "image/png", "pic.png");
  int64_t t = a->addObject(pageA, textObj(10, 20, "multi\nline \\ text"));
  int64_t i = a->addObject(pageA, imageObj(blobA, "pic.png"));

  std::string payload = a->exportObjects({t, i});
  auto ids = b->importObjects(pageB, payload, 24, 24);
  CHECK(ids.size() == 2);
  const Object* nt = b->object(ids[0]);
  const Object* ni = b->object(ids[1]);
  CHECK(nt && nt->prop("text") == "multi\nline \\ text" && nt->x == 34 && nt->y == 44);
  CHECK(ni && ni->type == ObjType::Image);
  std::string got;
  CHECK(ni && b->blob(ni->propInt("blob"), got) && got == img);  // blob travelled with it
  CHECK(b->canUndo() && b->undoLabel() == "Paste");
  b->undo();
  CHECK(b->page(pageB)->objects.empty());  // one undo removes the whole paste
  b->redo();
  CHECK(b->page(pageB)->objects.size() == 2);
  Object fo;
  fo.type = ObjType::Folder;
  fo.w = 200; fo.h = 100;
  fo.setProp("path", "docs\\sub");
  int64_t fid = a->addObject(pageA, fo);
  auto fids = b->importObjects(pageB, a->exportObjects({fid}), 0, 0);
  CHECK(fids.size() == 1 && b->object(fids[0]) && b->object(fids[0])->type == ObjType::Folder &&
        b->object(fids[0])->prop("path") == "docs\\sub");

  // Hostile / damaged payloads must import nothing and never crash.
  size_t before = b->page(pageB)->objects.size();
  std::vector<std::string> bad = {
      "", "garbage", "CAIRNOBJ1\n", "CAIRNOBJ1\nO 1 0 0 10 10 5 0 0 0\nab",  // truncated body
      "CAIRNOBJ1\nO 9 0 0 10 10 0 0 0 0\n",                                   // unknown type
      "CAIRNOBJ1\nO 1 0 0 10 10 18446744073709551615 1 0 0\n",                // length overflow
      "CAIRNOBJ1\nO 1 0 0 10 10 0 0 0\n",                                     // short header
      std::string("CAIRNOBJ1\nO 1 0 0 10 10 0 0 0 0\n") + std::string(300, 'x') + "\n",  // junk after
      payload.substr(0, payload.size() / 2),                                   // cut in half
  };
  for (size_t k = 0; k < bad.size(); ++k) {
    auto r = b->importObjects(pageB, bad[k], 0, 0);
    if (k != 7) CHECK(r.empty());
    else (void)r;  // trailing junk after a valid empty-ish record: just must not crash
  }
  CHECK(b->page(pageB)->objects.size() == before || b->page(pageB)->objects.size() == before + 1);
  CHECK(b->importObjects(99999, payload, 0, 0).empty());  // no such page

  // Every strict prefix of a valid payload must be rejected whole, never half-imported.
  size_t n0 = b->page(pageB)->objects.size();
  for (size_t cut = 0; cut + 1 < payload.size(); cut += 7) {
    auto r = b->importObjects(pageB, payload.substr(0, cut), 0, 0);
    if (!r.empty()) {  // a prefix that lands exactly on a record boundary is a valid 1-object payload
      CHECK(r.size() == 1);
      b->undo();
    }
  }
  CHECK(b->page(pageB)->objects.size() == n0);
  a.reset(); b.reset();
  std::filesystem::remove(pa);
  std::filesystem::remove(pb);
}

static void testMigrationFromV1() {
  std::printf("migrating a genuine v1 file\n");
  std::string path = tmpPath("cairn_v1.cairn");
  {
    // Exactly the schema v0.1 shipped (no blob table), user_version 1.
    Db db;
    db.open(path, true);
    db.exec("PRAGMA application_id=1128354382");
    db.exec("PRAGMA user_version=1");
    db.exec(R"SQL(BEGIN;
      CREATE TABLE page(id INTEGER PRIMARY KEY, title TEXT NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,
        bg INTEGER NOT NULL, sort INTEGER NOT NULL, tags TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, modified INTEGER NOT NULL);
      CREATE TABLE object(id INTEGER PRIMARY KEY, page_id INTEGER NOT NULL, type INTEGER NOT NULL,
        x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL, z INTEGER NOT NULL, props TEXT NOT NULL DEFAULT '');
      CREATE TABLE link(src_page INTEGER NOT NULL, src_object INTEGER NOT NULL, target_page INTEGER NOT NULL);
      CREATE VIRTUAL TABLE fts USING fts5(title, body, page_id UNINDEXED);
      INSERT INTO page VALUES(1,'Old page',960,640,16777215,0,'',1,1);
      INSERT INTO object VALUES(1,1,1,10,10,100,30,1,'text=legacy words
');
      COMMIT;)SQL");
  }
  {
    auto d = Document::open(path);
    CHECK(d->pages().size() == 1 && d->pages()[0].title == "Old page");
    CHECK(d->pages()[0].objects.size() == 1 && d->pages()[0].objects[0].prop("text") == "legacy words");
    int64_t id = d->addBlob("bytes", "image/png", "n.png");  // blob table now exists
    CHECK(id > 0);
    d->addObject(d->pages()[0].id, imageObj(id, "n.png"));
  }
  {
    Db raw;
    raw.open(path, false);
    Stmt s(raw, "PRAGMA user_version");
    s.step();
    CHECK(s.colInt(0) == 2);
  }
  auto d2 = Document::open(path);  // reopens cleanly at v2
  CHECK(d2->pages()[0].objects.size() == 2);

  // A file from the future is refused, not mangled.
  {
    Db raw;
    raw.open(path, false);
    raw.exec("PRAGMA user_version=99");
  }
  d2.reset();
  bool threw = false;
  try { Document::open(path); } catch (const DbError&) { threw = true; }
  CHECK(threw);
  std::filesystem::remove(path);
}

static void testSingleFile() {
  std::printf("single file on disk\n");
  std::string path = tmpPath("cairn_single.cairn");
  {
    auto d = Document::create(path, true);
    d->addPage("x");
    CHECK(!std::filesystem::exists(path + "-wal"));
    CHECK(!std::filesystem::exists(path + "-shm"));
    CHECK(!std::filesystem::exists(path + "-journal"));  // no stray journal between commits
  }
  std::filesystem::remove(path);
}

static std::vector<std::string> links(const std::string& t) {
  std::vector<std::string> v;
  for (auto& r : findUrls(t)) v.push_back(t.substr(r.first, r.second - r.first));
  return v;
}

static void testUrls() {
  std::printf("url detection\n");
  using V = std::vector<std::string>;
  CHECK(links("see https://example.com/a?b=1 now") == V{"https://example.com/a?b=1"});
  CHECK(links("Go to https://example.com.") == V{"https://example.com"});          // sentence period
  CHECK(links("(https://example.com/x)") == V{"https://example.com/x"});           // wrapping parens
  CHECK(links("https://en.wikipedia.org/wiki/Foo_(bar)") == V{"https://en.wikipedia.org/wiki/Foo_(bar)"});
  CHECK(links("HTTP://EXAMPLE.COM and www.example.org, ok") == (V{"HTTP://EXAMPLE.COM", "www.example.org"}));
  CHECK(links("mail mailto:a@b.co!") == V{"mailto:a@b.co"});
  CHECK(links("two http://a.io\nhttp://b.io").size() == 2);                        // newline ends a link
  CHECK(links("https://").empty() && links("www.").empty() && links("nowww.example.com").empty());
  CHECK(links("ftp://x.org javascript:alert(1) file:///c:/x").empty());            // never linkified
  CHECK(links("").empty());
  CHECK(urlTarget(std::string("www.x.org")) == "https://www.x.org");
  CHECK(urlTarget(std::string("http://x.org")) == "http://x.org");
  std::wstring w = L"a https://x.org b";
  auto r = findUrls(w);
  CHECK(r.size() == 1 && r[0].first == 2 && r[0].second == 15);                    // UTF-16 works too
}

static std::vector<std::string> titles(const Document& d) {
  std::vector<std::string> v;
  for (const Page& p : d.pages()) v.push_back(p.title);
  return v;
}

static void testPageOrder() {
  std::printf("page reordering\n");
  std::string path = "reorder_test.cairn";
  std::filesystem::remove(path);
  using V = std::vector<std::string>;
  {
    auto d = Document::create(path, false);
    int64_t a = d->addPage("A"), b = d->addPage("B"), c = d->addPage("C"), e = d->addPage("D");
    d->clearHistory();
    CHECK(titles(*d) == (V{"A", "B", "C", "D"}));
    CHECK(d->movePage(a, 2));
    CHECK(titles(*d) == (V{"B", "C", "A", "D"}));
    CHECK(!d->movePage(a, 2));                   // already there: no-op, no undo entry
    CHECK(d->movePage(e, -5));                   // clamped to the front
    CHECK(titles(*d) == (V{"D", "B", "C", "A"}));
    CHECK(d->movePage(b, 99));                   // clamped to the end
    CHECK(titles(*d) == (V{"D", "C", "A", "B"}));
    CHECK(!d->movePage(9999, 0));                // unknown page
    CHECK(d->undo() && titles(*d) == (V{"D", "B", "C", "A"}));
    CHECK(d->undo() && titles(*d) == (V{"B", "C", "A", "D"}));
    CHECK(d->undo() && titles(*d) == (V{"A", "B", "C", "D"}));
    CHECK(!d->canUndo());
    CHECK(d->redo() && d->redo() && titles(*d) == (V{"D", "B", "C", "A"}));
    int64_t f = d->addPage("E");                 // new pages still go last after reordering
    CHECK(titles(*d) == (V{"D", "B", "C", "A", "E"}));
    CHECK(d->deletePage(b));
    CHECK(d->movePage(f, 0) && titles(*d) == (V{"E", "D", "C", "A"}));
    (void)c;
  }
  {
    auto d = Document::open(path);               // the order survives closing and reopening
    CHECK(titles(*d) == (V{"E", "D", "C", "A"}));
  }
  std::filesystem::remove(path);
}

static std::string slice(const std::string& t, const Run& r) { return t.substr(r.start, r.len); }

static void testRichRuns() {
  std::printf("rich text runs\n");
  std::string t = "hello bold world";
  Runs r;
  toggleFlag(r, 6, 10, t.size(), kBold);
  CHECK(r.size() == 1 && slice(t, r[0]) == "bold" && r[0].flags == kBold);
  toggleFlag(r, 6, 10, t.size(), kBold);                       // toggling again clears it
  CHECK(r.empty());
  toggleFlag(r, 0, 5, t.size(), kBold);
  toggleFlag(r, 3, 9, t.size(), kItalic);                      // overlaps the bold run
  CHECK(r.size() == 3 && slice(t, r[0]) == "hel" && r[0].flags == kBold);
  CHECK(slice(t, r[1]) == "lo" && r[1].flags == (kBold | kItalic) && slice(t, r[2]) == " bol");
  // round trip, including a url with separators in it
  Runs u;
  restyleRange(u, 0, 5, t.size(), [](Run& x) { x.url = "https://x.org/a,b|c%20d"; x.color = 0x123456; x.size = 140; });
  Runs back = decodeRuns(encodeRuns(u), t.size());
  CHECK(back.size() == 1 && back[0].url == "https://x.org/a,b|c%20d" && back[0].color == 0x123456 && back[0].size == 140);
  // hostile storage: garbage, overlaps, out of range, huge numbers
  CHECK(decodeRuns("garbage||1,2|99999,5,1,,,|x,y,z,,,", 10).empty());
  Runs h = decodeRuns("0,6,1,,,|3,6,2,,,|8,100,4,,,|0,0,1,,,|1,1,999,,,|2,3,1,99999999,,|-1,2,1,,,", 10);
  size_t cursor = 0; bool sane = true;
  for (const Run& x : h) { if (x.start < cursor || x.end() > 10 || x.len == 0) sane = false; cursor = x.end(); }
  CHECK(sane && !h.empty());
  CHECK(decodeRuns("", 5).empty());

  // runs follow the text through edits
  std::string a = "one two three";
  Runs e;
  toggleFlag(e, 4, 7, a.size(), kBold);                         // "two"
  std::string b = "ONE one two three";                          // insert before: shifts
  shiftRunsForEdit(e, a, b);
  CHECK(e.size() == 1 && slice(b, e[0]) == "two");
  std::string c = "ONE one twoo three";                         // typing at the end of bold continues it
  shiftRunsForEdit(e, b, c);
  CHECK(e.size() == 1 && slice(c, e[0]) == "twoo");
  std::string d = "ONE one tXwoo three";                        // typing inside stays bold
  shiftRunsForEdit(e, c, d);
  CHECK(e.size() == 1 && slice(d, e[0]) == "tXwoo");
  std::string f = "ONE one  three";                             // deleting the whole run removes it
  shiftRunsForEdit(e, d, f);
  CHECK(e.empty());
  Runs lk;
  restyleRange(lk, 0, 4, 10, [](Run& x) { x.url = "https://a.b"; });
  shiftRunsForEdit(lk, "link text!", "linkX text!");            // typing after a link does not extend it
  CHECK(lk.size() == 1 && lk[0].len == 4);
  Runs mb;
  toggleFlag(mb, 0, 3, 6, kBold);
  shiftRunsForEdit(mb, "\xC3\xA9" "abc", "\xC3\xA9" "abXc");  // multibyte text never splits a character
  CHECK(mb.size() == 1 && mb[0].start == 0);
  // overlay (rich paste) replaces styling in the pasted range only
  Runs base;
  toggleFlag(base, 0, 20, 20, kItalic);
  Runs over;
  toggleFlag(over, 2, 5, 8, kBold);
  overlayRuns(base, over, 4, 8, 20);
  CHECK(base.size() >= 3 && base[0].start == 0 && base[0].len == 4 && base[0].flags == kItalic);
}

static std::string plainOf(const std::string& h, Runs* out = nullptr, const std::string& src = "") {
  RichText r = htmlToRich(h, src);
  if (out) *out = r.runs;
  return r.text;
}

static void testLinkTarget() {
  CHECK(sanitizeLinkTarget("  https://a.b/c  ") == "https://a.b/c");
  CHECK(sanitizeLinkTarget("example.com/x") == "https://example.com/x");
  CHECK(sanitizeLinkTarget("www.example.org") == "https://www.example.org");
  CHECK(sanitizeLinkTarget("mailto:me@x.org") == "mailto:me@x.org");
  CHECK(sanitizeLinkTarget("javascript:alert(1)").empty() && sanitizeLinkTarget("file:///c:/x").empty());
  CHECK(sanitizeLinkTarget("data:text/html,x").empty() && sanitizeLinkTarget("ftp://x.org").empty());
  CHECK(sanitizeLinkTarget("").empty() && sanitizeLinkTarget("https://").empty() && sanitizeLinkTarget("a b.com").empty());
  CHECK(sanitizeLinkTarget("localhost").empty() && sanitizeLinkTarget("/etc/passwd").empty());
}



static void testTypingStyle() {
  using namespace cairn;
  // "ab" with "b" bold. After "b" the style is bold; at the start it is plain.
  Runs r = decodeRuns("1,1,1,,,", 2);
  CHECK(styleBefore(r, 0) == 0);
  CHECK(styleBefore(r, 1) == 0);
  CHECK(styleBefore(r, 2) == kBold);
  CHECK(styleBefore(r, 99) == 0);
  // Typing "X" at the end with bold turned OFF: the character is plain although the run before is bold.
  Runs a = r;
  std::string o = "ab", n = "abX";
  shiftRunsForEdit(a, o, n);  // the edit tracker would extend the bold run
  CHECK(rangeAllHave(a, 2, 3, kBold));
  CHECK(applyTypingStyle(a, o, n, 2, 0));
  CHECK(!rangeAllHave(a, 2, 3, kBold) && rangeAllHave(a, 1, 2, kBold));
  // Typing "Y" in the middle with italic turned ON; others untouched, bold kept where it was.
  Runs b = r;
  o = "ab"; n = "aYb";
  shiftRunsForEdit(b, o, n);
  CHECK(applyTypingStyle(b, o, n, 1, kItalic));
  CHECK(rangeAllHave(b, 1, 2, kItalic) && !rangeAllHave(b, 0, 1, kItalic) && !rangeAllHave(b, 2, 3, kItalic));
  CHECK(rangeAllHave(b, 2, 3, kBold) && !rangeAllHave(b, 1, 2, kBold));
  // Multi-character insert (a paste of two characters).
  Runs c;
  o = "ab"; n = "abQR";
  CHECK(applyTypingStyle(c, o, n, 2, kBold | kUnderline));
  CHECK(rangeAllHave(c, 2, 4, kBold) && rangeAllHave(c, 2, 4, kUnderline));
  // Colour and size of an overlapping run survive.
  Runs d = decodeRuns("0,2,0,255,150,", 2);
  o = "ab"; n = "abZ";
  shiftRunsForEdit(d, o, n);
  CHECK(applyTypingStyle(d, o, n, 2, kBold));
  bool kept = false;
  for (const Run& x : d) if (x.start <= 2 && x.end() >= 3 && x.color == 255 && x.size == 150 && (x.flags & kBold)) kept = true;
  CHECK(kept);
  // Refusals: caret elsewhere, deletion, replacement, no change.
  Runs e;
  CHECK(!applyTypingStyle(e, "ab", "Xab", 2, kBold));   // inserted at 0, caret said 2
  CHECK(!applyTypingStyle(e, "abc", "ab", 2, kBold));   // deletion
  CHECK(!applyTypingStyle(e, "abc", "aXc", 1, kBold));  // replacement is not an insertion
  CHECK(!applyTypingStyle(e, "abc", "aXYZ", 1, kBold)); // replaced and grew: not a pure insertion
  CHECK(!applyTypingStyle(e, "ab", "ab", 1, kBold));
  CHECK(!applyTypingStyle(e, "ab", "abX", 7, kBold));   // caret out of range
  CHECK(e.empty());
}

static void testUtf16Units() {
  using namespace cairn;
  std::string s = "a\xC3\xA9\xF0\x9F\x98\x80z\nq";  // a, e-acute (2 bytes), emoji (4 bytes), z, newline, q
  CHECK(utf16UnitsBefore(s, 0) == 0);
  CHECK(utf16UnitsBefore(s, 1) == 1);      // after 'a'
  CHECK(utf16UnitsBefore(s, 3) == 2);      // after e-acute
  CHECK(utf16UnitsBefore(s, 7) == 4);      // after the emoji: two UTF-16 units
  CHECK(utf16UnitsBefore(s, s.size()) == 7);
  CHECK(utf16UnitsBefore(s, 9999) == 7);   // out of range clamps
  CHECK(utf16UnitsBefore("", 5) == 0);
  CHECK(utf16UnitsBefore("\x80\x80", 2) == 0);  // stray continuation bytes don't count
}

static void testSizeSteps() {
  using namespace cairn;
  CHECK(stepFontPt(14, 1) == 16);
  CHECK(stepFontPt(14, -1) == 13);
  CHECK(stepFontPt(15, 1) == 16);   // off the ladder: next rung up
  CHECK(stepFontPt(15, -1) == 14);  // ... and next rung down
  CHECK(stepFontPt(96, 1) == 96);   // clamps at the ends
  CHECK(stepFontPt(6, -1) == 6);
  CHECK(stepFontPt(-5, 1) == 6);
  CHECK(stepFontPt(100000, -1) == 96);
  CHECK(stepSizePercent(0, 1) == 125);    // 0 means "100"
  CHECK(stepSizePercent(0, -1) == 85);
  CHECK(stepSizePercent(400, 1) == 400);
  CHECK(stepSizePercent(50, -1) == 50);
  int v = 0;
  for (int i = 0; i < 20; ++i) v = stepSizePercent(v, 1);
  CHECK(v == 400);
}


static std::string fileOf(const std::vector<ExportFile>& fs, const std::string& path) {
  for (const auto& f : fs) if (f.path == path) return f.data;
  return "\x01MISSING";
}


static void testReplace() {
  std::printf("find & replace\n");
  using namespace cairn;
  std::string t = "Cat cat CAT concatenate";
  Runs none;
  CHECK(replaceInText(t, none, "cat", "dog", false) == 4 && t == "dog dog dog condogenate");
  t = "Cat cat CAT";
  CHECK(replaceInText(t, none, "cat", "dog", true) == 1 && t == "Cat dog CAT");
  t = "aaa";
  CHECK(replaceInText(t, none, "aa", "b", true) == 1 && t == "ba");  // non-overlapping
  t = "x";
  CHECK(replaceInText(t, none, "", "y", true) == 0 && t == "x");
  t = "a.b";
  CHECK(replaceInText(t, none, ".", "", true) == 1 && t == "ab");    // literal, not a pattern
  t = "h\xC3\xA9llo \xC3\xA9";
  CHECK(replaceInText(t, none, "\xC3\xA9", "e", true) == 2 && t == "hello e");
  t = "abc";
  CHECK(replaceInText(t, none, "abc", std::string(1100000, 'z'), true) == 0 && t == "abc");  // size cap
  // Styling follows the text: "ab CD ef" with CD bold; replacing "ab" by "xyz!" shifts it.
  t = "ab CD ef";
  Runs r = decodeRuns("3,2,1,,,", t.size());
  CHECK(replaceInText(t, r, "ab", "xyz!", true) == 1 && t == "xyz! CD ef");
  CHECK(rangeAllHave(r, 5, 7, kBold) && !rangeAllHave(r, 0, 5, kBold));
  // Replacing the styled word itself keeps its style; shrinking and growing both work.
  t = "ab CD ef"; r = decodeRuns("3,2,1,,,", t.size());
  CHECK(replaceInText(t, r, "CD", "QRS", true) == 1 && t == "ab QRS ef");
  CHECK(rangeAllHave(r, 3, 6, kBold) && !rangeAllHave(r, 6, 9, kBold));

  std::string path = tmpPath("cairn_replace.cairn");
  auto d = Document::create(path, false);
  int64_t p1 = d->addPage("One"), p2 = d->addPage("Two");
  Object a = textObj(0, 0, "Red fish, red hat");
  a.setProp("runs", "0,3,1,,,");
  int64_t a1 = d->addObject(p1, a);
  d->addObject(p2, textObj(0, 0, "no match here"));
  Object b = textObj(0, 40, "RED door");
  int64_t b2 = d->addObject(p2, b);
  Object btn; btn.type = ObjType::Button; btn.w = 80; btn.h = 24; btn.setProp("label", "Red button");
  int64_t b3 = d->addObject(p2, btn);

  auto dry = d->replaceAll("red", "blue", false, 0, true);
  CHECK(dry.matches == 4 && dry.objects == 3 && dry.pages == 2);
  CHECK(d->object(a1)->prop("text") == "Red fish, red hat");  // dry run changes nothing
  auto one = d->replaceAll("red", "blue", false, p1, true);
  CHECK(one.matches == 2 && one.pages == 1);

  auto done = d->replaceAll("red", "blue", false, 0, false);
  CHECK(done.matches == 4);
  CHECK(d->object(a1)->prop("text") == "blue fish, blue hat");
  CHECK(d->object(b2)->prop("text") == "blue door");
  CHECK(d->object(b3)->prop("label") == "blue button");
  Runs ra = decodeRuns(d->object(a1)->prop("runs"), d->object(a1)->prop("text").size());
  CHECK(rangeAllHave(ra, 0, 4, kBold) && !rangeAllHave(ra, 4, 10, kBold));  // "blue" kept the bold
  CHECK(d->search("blue").size() == 2);   // the search index followed
  CHECK(d->search("red").empty());
  CHECK(d->undo());                        // the whole replace is one undo step
  CHECK(d->object(a1)->prop("text") == "Red fish, red hat" && d->object(b3)->prop("label") == "Red button");
  CHECK(d->search("red").size() == 2);
  CHECK(d->replaceAll("zzz", "y", false, 0, false).matches == 0);
}

static void testExportSite() {
  std::printf("export as website\n");
  std::string path = tmpPath("cairn_export.cairn");
  auto d = Document::create(path, false);
  int64_t p1 = d->addPage("Home <b>&\"x\"");
  int64_t p2 = d->addPage("Second");
  Object t = textObj(10, 20, "Hello <script>alert(1)</script> & goodbye");
  t.setProp("runs", "0,5,1,,,");  // "Hello" bold
  d->addObject(p1, t);
  Object b; b.type = ObjType::Button; b.x = 5; b.y = 6; b.w = 100; b.h = 30;
  b.setProp("label", "go <next>"); b.setPropInt("target", (long)p2);
  d->addObject(p1, b);
  Object web = b; web.props.clear(); web.setProp("label", "web"); web.setProp("open", "https://example.org/a?b=1&c=2");
  d->addObject(p1, web);
  Object evil = b; evil.props.clear(); evil.setProp("label", "evil"); evil.setProp("open", "javascript:alert(1)");
  d->addObject(p1, evil);
  Object file = b; file.props.clear(); file.setProp("label", "local"); file.setProp("open", "C:\\Windows\\evil.exe");
  d->addObject(p1, file);
  int64_t blob = d->addBlob("PNGDATA", "image/png", "pic\".png");
  d->addObject(p2, imageObj(blob, "pic\".png"));
  int64_t odd = d->addBlob("x", "application/x-evil", "odd.bin");
  d->addObject(p2, imageObj(odd, "odd.bin"));
  d->setPageProps(p2, 800, 600, 0x112233, nullptr);

  auto files = exportSite(*d, "My <Site>");
  CHECK(files.size() >= 4 && files[0].path == "index.html");
  std::string idx = fileOf(files, "index.html");
  CHECK(idx.find("My &lt;Site&gt;") != std::string::npos);
  CHECK(idx.find(exportPageFile(p1)) != std::string::npos && idx.find(exportPageFile(p2)) != std::string::npos);
  CHECK(idx.find("<b>&\"x\"") == std::string::npos);  // titles are escaped

  std::string h1 = fileOf(files, exportPageFile(p1));
  CHECK(h1.find("<script>") == std::string::npos);
  CHECK(h1.find("&lt;script&gt;") != std::string::npos);
  CHECK(h1.find("<b>Hello</b>") != std::string::npos);
  CHECK(h1.find("left:10px;top:20px") != std::string::npos);
  CHECK(h1.find("href=\"" + exportPageFile(p2) + "\"") != std::string::npos);  // button -> page
  CHECK(h1.find("go &lt;next&gt;") != std::string::npos);
  CHECK(h1.find("href=\"https://example.org/a?b=1&amp;c=2\"") != std::string::npos);
  CHECK(h1.find("javascript:") == std::string::npos);
  CHECK(h1.find("evil.exe") == std::string::npos && h1.find("C:\\") == std::string::npos);
  CHECK(h1.find("<a class=\"b\" href=\"javascript") == std::string::npos);
  CHECK(h1.find("Second &rarr;") != std::string::npos);  // next-page nav

  std::string h2 = fileOf(files, exportPageFile(p2));
  CHECK(h2.find("width:800px;height:600px;background:#112233") != std::string::npos);
  CHECK(h2.find("src=\"images/b" + std::to_string(blob) + ".png\"") != std::string::npos);
  CHECK(h2.find("alt=\"pic&quot;.png\"") != std::string::npos);
  CHECK(fileOf(files, "images/b" + std::to_string(blob) + ".png") == "PNGDATA");
  CHECK(h2.find("images/b" + std::to_string(odd)) == std::string::npos);  // unknown type: not exported
  CHECK(h2.find("odd.bin") != std::string::npos);                         // placeholder keeps its name
  CHECK(h2.find("&larr; Home") != std::string::npos);

  // A button pointing at a page that no longer exists must not produce a dangling link.
  Object gone = b; gone.props.clear(); gone.setProp("label", "gone"); gone.setPropInt("target", 99999);
  d->addObject(p2, gone);
  std::string h2b = fileOf(exportSite(*d, "s"), exportPageFile(p2));
  CHECK(h2b.find("page-99999") == std::string::npos);
  CHECK(exportSite(*Document::create(tmpPath("cairn_export2.cairn"), false), "empty").size() == 1);
}

static void testHtmlCopy() {
  using namespace cairn;
  // Plain text is escaped and newlines become <br>.
  CHECK(richToHtml("a<b>&\"x\nz", {}) == "a&lt;b&gt;&amp;&quot;x<br>z");
  // Styles and links.
  Runs r = decodeRuns("0,2,3,,,|3,3,0,16711680,150,https%3A%2F%2Fx.org", 6);
  std::string h = richToHtml("ab cde", r);
  CHECK(h.find("<b><i>ab</i></b>") != std::string::npos || h.find("<i><b>ab</b></i>") != std::string::npos);
  CHECK(h.find("href=\"https://x.org\"") != std::string::npos);
  CHECK(h.find("color:#FF0000;") != std::string::npos);
  CHECK(h.find("font-size:150%;") != std::string::npos);
  // A hostile link never reaches the output.
  Run bad; bad.start = 0; bad.len = 2; bad.url = "javascript:alert(1)";
  CHECK(richToHtml("hi", {bad}).find("javascript") == std::string::npos);
  CHECK(richToHtml("hi", {bad}).find("<a") == std::string::npos);
  // Round trip through the paste reader.
  RichText back = htmlToRich(richToHtml("ab cde", r));
  CHECK(back.text == "ab cde");
  CHECK(rangeAllHave(back.runs, 0, 2, kBold) && rangeAllHave(back.runs, 0, 2, kItalic));
  bool link = false;
  for (const Run& x : back.runs) if (x.url == "https://x.org") link = true;
  CHECK(link);
  // Clipboard envelope offsets point at the fragment.
  std::string w = wrapClipboardHtml("<b>hi</b>");
  size_t s = std::stoul(w.substr(w.find("StartFragment:") + 14, 10));
  size_t e = std::stoul(w.substr(w.find("EndFragment:") + 12, 10));
  CHECK(w.substr(s, e - s) == "<b>hi</b>");
  CHECK(std::stoul(w.substr(w.find("EndHTML:") + 8, 10)) == w.size());
  CHECK(htmlFragmentFromClipboard(w) == "<b>hi</b>");
}

static void testHtmlPaste() {
  std::printf("html paste\n");
  Runs r;
  std::string t = plainOf("<p>Hello <b>bold</b> and <i>it</i> <a href=\"https://x.org/a?b=1&amp;c=2\">link</a>.</p>", &r);
  CHECK(t == "Hello bold and it link.");
  CHECK(r.size() == 3 && slice(t, r[0]) == "bold" && r[0].flags == kBold);
  CHECK(slice(t, r[1]) == "it" && r[1].flags == kItalic);
  CHECK(slice(t, r[2]) == "link" && r[2].url == "https://x.org/a?b=1&c=2");
  CHECK(plainOf("<div>a</div><div>b</div>") == "a\nb");
  CHECK(plainOf("<p>one</p><p>two</p>") == "one\n\ntwo");
  CHECK(plainOf("x<br>y<br><br>z") == "x\ny\n\nz");
  CHECK(plainOf("  lots   of\n\t space  ") == "lots of space");
  CHECK(plainOf("<ul><li>a</li><li>b</li></ul>") == "\xE2\x80\xA2 a\n\xE2\x80\xA2 b");
  CHECK(plainOf("<ol><li>a</li><li>b</li></ol>") == "1. a\n2. b");
  CHECK(plainOf("5 &lt; 6 &amp; &#65;&#x42; &copy; &bogus;") == "5 < 6 & AB \xC2\xA9 &bogus;");
  CHECK(plainOf("<script>alert(1)</script>ok<style>p{}</style>") == "ok");
  CHECK(plainOf("a<span style=\"display:none\">SECRET</span>b") == "ab");
  CHECK(plainOf("<!-- c -->t<!--x-->") == "t");
  CHECK(plainOf("<pre>a\n  b</pre>") == "a\n  b");
  std::string h = plainOf("<h1>Title</h1><p>body</p>", &r);
  CHECK(h == "Title\n\nbody" && r.size() == 1 && r[0].flags == kBold && r[0].size == 160);
  plainOf("<span style=\"color: rgb(200, 30, 30); font-weight:700; text-decoration: underline line-through\">x</span>", &r);
  CHECK(r.size() == 1 && r[0].color == 0xC81E1E && r[0].flags == (kBold | kUnderline | kStrike));
  plainOf("<span style=\"color:#222\">plain</span><span style=\"color:#fff\">white</span>", &r);
  CHECK(r.empty());                                              // neutral body colours are not kept
  plainOf("<span style=\"font-size:32px\">big</span><span style=\"font-size:16px\">same</span>", &r);
  CHECK(r.size() == 1 && r[0].size == 200);
  // links: only safe schemes; relative links resolve against the page
  std::string t2 = plainOf("<a href=\"javascript:alert(1)\">a</a><a href=\"/wiki/X\">b</a><a href=\"data:text/html,x\">c</a><a href=\"#top\">d</a>", &r, "https://en.wikipedia.org/wiki/Main");
  CHECK(t2 == "abcd" && r.size() == 1 && r[0].url == "https://en.wikipedia.org/wiki/X");
  // malformed input must not crash or hang
  plainOf("<<<>>><a href=<b>unclosed <i>x</u></b></i><", &r);
  plainOf("<div><div><div>" + std::string(5000, '<'), &r);
  std::string deep;
  for (int i = 0; i < 3000; ++i) deep += "<b>";
  CHECK(htmlToRich(deep + "x").text == "x");
  // CF_HTML envelope
  std::string clip = "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000200\r\nStartFragment:0000000130\r\nEndFragment:0000000140\r\nSourceURL:https://a.b/c\r\n<html><body><!--StartFragment--><b>X</b><!--EndFragment--></body></html>";
  std::string src;
  std::string frag = htmlFragmentFromClipboard(clip, &src);
  CHECK(src == "https://a.b/c");
  std::string noHdr = htmlFragmentFromClipboard("<html><!--StartFragment-->hi<!--EndFragment--></html>");
  CHECK(noHdr == "hi");
  (void)frag;
}

static void testTags() {
  std::printf("tags\n");
  CHECK(normalizeTags("  Alpha ,beta;  #Gamma\n alpha,,  ") == "Alpha, beta, Gamma");  // dedupe ignores case
  CHECK(normalizeTags("a   b ,\t c") == "a-b, c");
  CHECK(normalizeTags("").empty() && normalizeTags(" , ;\n").empty() && normalizeTags("#").empty());
  CHECK(normalizeTags(std::string(100, 'x')).size() == 40);
  std::string many;
  for (int i = 0; i < 60; ++i) many += "t" + std::to_string(i) + ",";
  CHECK(splitTags(normalizeTags(many)).size() == 24);
  std::string path = "tags_test.cairn";
  std::filesystem::remove(path);
  int64_t a, b, c;
  {
    auto d = Document::create(path, false);
    a = d->addPage("Garden notes"); b = d->addPage("Tax stuff"); c = d->addPage("Untagged");
    d->clearHistory();
    std::string t1 = "home, Money", t2 = "money; taxes", none = "";
    CHECK(d->setPageProps(a, 960, 640, 0xFFFFFF, &t1));
    CHECK(d->setPageProps(b, 960, 640, 0xFFFFFF, &t2));
    CHECK(d->page(a)->tags == "home, Money");
    CHECK(!d->setPageProps(a, 960, 640, 0xFFFFFF, &t1));                    // no change, no undo entry
    CHECK(d->search("#money").size() == 2);                                // exact tag filter, any case
    CHECK(d->search("#mon").size() == 2 && d->search("#tax").size() == 1);  // prefix
    CHECK(d->search("#home #money").size() == 1);                           // all terms must match
    CHECK(d->search("#nope").empty());
    CHECK(d->search("#money garden").size() == 1);                          // tag filter plus text
    CHECK(d->search("taxes").size() == 1);                                  // plain words find tags too
    auto tags = d->allTags();
    CHECK(tags.size() == 3 && tags[0].first == "home" && tags[1].first == "Money" && tags[1].second == 2);
    CHECK(d->undo() && d->page(b)->tags.empty() && d->search("#taxes").empty());   // undo takes it out of search
    CHECK(d->redo() && d->search("#taxes").size() == 1);
    (void)c; (void)none;
  }
  {
    auto d = Document::open(path);                                           // persists
    CHECK(d->page(a)->tags == "home, Money" && d->search("#home").size() == 1);
  }
  std::filesystem::remove(path);
}

static void testMarkdown() {
  std::printf("markdown\n");
  using namespace cairn;
  MdInline a = parseMarkdownInline("a **b** *c* ~~d~~ `e` [f](https://x.org/y) g");
  CHECK(a.text == "a b c d e f g");
  CHECK(rangeAllHave(a.runs, 2, 3, kBold) && rangeAllHave(a.runs, 4, 5, kItalic));
  CHECK(rangeAllHave(a.runs, 6, 7, kStrike) && rangeAllHave(a.runs, 8, 9, kMono));
  CHECK(!rangeAllHave(a.runs, 0, 2, kBold));
  bool hasUrl = false;
  for (const Run& r : a.runs) if (r.url == "https://x.org/y") hasUrl = true;
  CHECK(hasUrl);
  CHECK(parseMarkdownInline("2 * 3 * 4").text == "2 * 3 * 4");          // spaces: not emphasis
  CHECK(parseMarkdownInline("snake_case_name").runs.empty());            // intraword underscores
  CHECK(parseMarkdownInline("**open").text == "**open");                 // unmatched stays literal
  CHECK(parseMarkdownInline("\\*not\\*").text == "*not*");
  CHECK(parseMarkdownInline("![pic](http://x/y.png)").text == "[image: pic]");
  MdInline bad = parseMarkdownInline("[click](javascript:alert(1)) done");
  bool anyUrl = false;
  for (const Run& r : bad.runs) if (!r.url.empty()) anyUrl = true;
  CHECK(!anyUrl && bad.text == "click done");                            // unsafe scheme dropped
  CHECK(rangeAllHave(parseMarkdownInline("***x***").runs, 0, 1, kBold | kItalic));
  // Hostile: deep nesting and long unmatched runs terminate.
  std::string deep(5000, '*');
  CHECK(parseMarkdownInline(deep).text.size() <= 5000);  // terminates quickly
  std::string br;
  for (int i = 0; i < 3000; ++i) br += "[a](";
  CHECK(!parseMarkdownInline(br).text.empty());

  // Round trip through export syntax.
  std::string t = "plain bold it\nsecond *star*";
  Runs r = decodeRuns("6,4,1,,,|11,2,2,,,", t.size());
  MdInline rt = parseMarkdownInline(richToMarkdown(t, r));
  CHECK(rt.text == t);
  CHECK(rangeAllHave(rt.runs, 6, 10, kBold) && rangeAllHave(rt.runs, 11, 13, kItalic));
  CHECK(richToMarkdown("# not a heading", Runs()) == "\\# not a heading");
  CHECK(richToMarkdown("1. no list", Runs()) == "1\\. no list");

  // Import / export on a real document.
  std::string path = tmpPath("md.cairn");
  auto doc = Document::create(path, false);
  std::string src = "intro line\n\n# First\nTags: alpha, beta\n\nSome **bold** text.\n\n- one\n- two\n\n## Sub\n\n```\ncode here\n```\n\n# Second\n\nhi\n";
  size_t before = doc->pages().size();
  MdImportResult res = importMarkdown(*doc, src, "Imported");
  CHECK(res.pages == 3 && doc->pages().size() == before + 3);
  const Page* p1 = doc->page(res.firstPage + 1);
  CHECK(p1 && p1->title == "First" && p1->tags == "alpha, beta");
  CHECK(p1 && p1->objects.size() == 4);
  CHECK(doc->undo());  // one undo removes only the last imported page
  CHECK(doc->pages().size() == before + 2);
  doc->redo();
  std::string md = exportMarkdown(*doc);
  CHECK(md.find("# First") != std::string::npos && md.find("**bold**") != std::string::npos);
  CHECK(md.find("Tags: alpha, beta") != std::string::npos);
  CHECK(importMarkdown(*doc, std::string(3u << 20, 'x'), "Big").pages == 0);  // oversize ignored
  CHECK(importMarkdown(*doc, "", "Empty").pages == 0);
  std::remove(path.c_str());
}

static void testTables() {
  std::printf("tables\n");
  using namespace cairn;
  Table a = parseTable("| Name | Qty |\n| apple | 3 |\n| pear |\n");
  CHECK(a.rows.size() == 3 && a.cols() == 2 && a.rows[1][0] == "apple" && a.rows[2][1].empty());  // short row padded
  Table b = parseTable("x | y\n  1 |  2  \n");  // outer bars optional, cells trimmed
  CHECK(b.rows.size() == 2 && b.rows[0][0] == "x" && b.rows[1][1] == "2");
  Table e = parseTable("|  | b |\n| a | |\n");  // empty cells keep their place
  CHECK(e.cols() == 2 && e.rows[0][0].empty() && e.rows[0][1] == "b" && e.rows[1][1].empty());
  Table esc = parseTable("| a\\|b | c\\\\d |");
  CHECK(esc.rows.size() == 1 && esc.rows[0][0] == "a|b" && esc.rows[0][1] == "c\\d");
  CHECK(parseTable(formatTable(esc)).rows == esc.rows);  // round trip
  CHECK(parseTable(formatTable(e)).rows == e.rows);
  CHECK(parseTable("").rows.empty() && parseTable("\n\n  \n").rows.empty());
  Table md = parseTable("| h1 | h2 |\n|:---|---:|\n| a | b |\n", true);
  CHECK(md.rows.size() == 2 && md.rows[1][1] == "b");  // separator skipped
  CHECK(parseTable("|---|\n", false).rows.size() == 1);  // only skipped when asked
  std::string big;
  for (int i = 0; i < 500; ++i) { for (int j = 0; j < 60; ++j) big += "| c "; big += "|\n"; }
  Table cap = parseTable(big);
  CHECK(cap.rows.size() == kTableMaxRows && cap.cols() == kTableMaxCols);
  CHECK(parseTable("| " + std::string(9000, 'z') + " |").rows[0][0].size() == kTableMaxCell);
  CHECK(tableToMarkdown(a, true) == "| Name | Qty |\n| --- | --- |\n| apple | 3 |\n| pear |  |\n");
  std::string h = tableToHtml(parseTable("| <b>x</b> | \"q\" & |"), true);
  CHECK(h.find("<b>") == std::string::npos && h.find("&lt;b&gt;") != std::string::npos &&
        h.find("<th>") != std::string::npos && h.find("&amp;") != std::string::npos);
  CHECK(tableToHtml(a, false).find("<th>") == std::string::npos);

  {  // structure edits
    Table t = parseTable("| a | b |\n| c | d |\n");
    CHECK(tableInsertRow(t, 1) && t.rows.size() == 3 && t.rows[1].size() == 2 && t.rows[1][0].empty() && t.rows[2][0] == "c");
    CHECK(tableInsertRow(t, 3) && t.rows.size() == 4);  // append
    CHECK(!tableInsertRow(t, 9) && t.rows.size() == 4);
    CHECK(tableDeleteRow(t, 3) && tableDeleteRow(t, 1) && t.rows.size() == 2 && t.rows[1][0] == "c");
    CHECK(tableInsertCol(t, 1) && t.cols() == 3 && t.rows[0][1].empty() && t.rows[0][2] == "b" && t.rows[1][2] == "d");
    CHECK(tableInsertCol(t, 3) && t.cols() == 4 && !tableInsertCol(t, 9));
    CHECK(tableDeleteCol(t, 3) && tableDeleteCol(t, 1) && t.cols() == 2 && t.rows[1][1] == "d");
    CHECK(tableDeleteRow(t, 0) && !tableDeleteRow(t, 0) && t.rows.size() == 1);  // keeps one row
    CHECK(tableDeleteCol(t, 0) && !tableDeleteCol(t, 0) && t.cols() == 1);        // and one column
    Table big;
    for (size_t i = 0; i < kTableMaxRows; ++i) tableInsertRow(big, big.rows.size());
    CHECK(big.rows.size() == kTableMaxRows && !tableInsertRow(big, 0));  // row cap
    Table wide = parseTable("x");
    while (tableInsertCol(wide, wide.cols())) {}
    CHECK(wide.cols() == kTableMaxCols);  // column cap
    Table e;  // an empty table can still grow
    CHECK(tableInsertCol(e, 0) && e.cols() == 1);
    Table sc = parseTable("| a | b |\n");
    CHECK(tableSetCell(sc, 0, 1, "  x\ny | z  ") && sc.rows[0][1] == "x y | z");
    CHECK(!tableSetCell(sc, 5, 0, "q") && !tableSetCell(sc, 0, 5, "q"));
    CHECK(parseTable(formatTable(sc)).rows == sc.rows);  // a bar inside a cell survives
    std::string long_ = std::string(kTableMaxCell - 1, 'a') + "\xC3\xA9";  // 'é' would straddle the cap
    tableSetCell(sc, 0, 0, long_);
    CHECK(sc.rows[0][0].size() == kTableMaxCell - 1);  // cut before the character, not through it
  }

  // Through a document: Markdown tables become table objects and survive an export.
  std::string path = tmpPath("tbl.cairn");
  auto doc = Document::create(path, false);
  MdImportResult r = importMarkdown(*doc, "# T\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\nafter\n", "x");
  const Page* pg = doc->page(r.firstPage);
  CHECK(pg && pg->objects.size() == 2 && pg->objects[0].type == ObjType::Table);
  CHECK(pg && parseTable(pg->objects[0].prop("data")).rows.size() == 2);
  std::string md2 = exportMarkdown(*doc);
  CHECK(md2.find("| A | B |\n| --- | --- |\n| 1 | 2 |") != std::string::npos);
  CHECK(!doc->search("1 | 2").empty());  // table text is searchable
  std::remove(path.c_str());
}

static void testWikiLinks() {
  std::printf("wiki links\n");
  using namespace cairn;
  auto w = findWikiLinks("see [[Alpha]] and [[ Beta page ]] ok");
  CHECK(w.size() == 2 && w[0].title == "Alpha" && w[1].title == "Beta page");
  CHECK(w[0].start == 4 && w[0].len == 9);
  CHECK(findWikiLinks("[[]] [[  ]] [[a\nb]] [[open").empty());
  CHECK(findWikiLinks("[[a[b]]").size() == 0 && findWikiLinks("[[[x]]]").size() == 1);
  CHECK(findWikiLinks("[[x]]")[0].title == "x");
  CHECK(findWikiLinks("[[" + std::string(200, 'z') + "]]").empty());       // title too long
  std::string many;
  for (int i = 0; i < 2000; ++i) many += "[[a]]";
  CHECK(findWikiLinks(many).size() == 500);
  std::string unclosed;
  for (int i = 0; i < 5000; ++i) unclosed += "[[";
  CHECK(findWikiLinks(unclosed).empty());  // finishes, no quadratic blowup
  CHECK(findWikiLinks("[[h\xC3\xA9llo]]")[0].title == "h\xC3\xA9llo");

  std::string path = tmpPath("wiki.cairn");
  auto doc = Document::create(path, false);
  int64_t pa = doc->addPage("Alpha"), pb = doc->addPage("Beta Notes");
  doc->addObject(pa, textObj(10, 10, "go to [[beta notes]] now"));
  CHECK(doc->pageByTitle("BETA NOTES") == pb && doc->pageByTitle("nope") == 0);
  auto bl = doc->backlinks(pb);
  CHECK(bl.size() == 1 && bl[0].first == pa);
  CHECK(doc->backlinks(pa).empty());
  int64_t pc = doc->addPage("Gamma");
  doc->addObject(pc, textObj(10, 10, "[[Gamma]] self and [[Missing]]"));
  CHECK(doc->backlinks(pc).empty());  // a page linking to itself isn't its own backlink
  int upd = -1;
  doc->renamePage(pb, "Renamed", &upd);
  CHECK(upd == 1);
  CHECK(doc->backlinks(pb).size() == 1);  // the mention followed the rename
  {
    const Object& to = doc->page(pa)->objects[0];
    CHECK(to.prop("text") == "go to [[Renamed]] now");
  }
  CHECK(doc->undo());  // one step undoes the title and the text together
  CHECK(doc->page(pb)->title == "Beta Notes" && doc->page(pa)->objects[0].prop("text") == "go to [[beta notes]] now");
  CHECK(doc->redo());
  CHECK(doc->renamePage(pb, "Beta Notes"));
  {  // styled text: runs keep their place when the title changes length
    int64_t pd = doc->addPage("Delta");
    Object st = textObj(10, 10, "**x [[Beta Notes]] tail");
    st.setProp("runs", "0,3,1,,,");
    int64_t sid = doc->addObject(pd, st);
    doc->renamePage(pb, "B");
    const Object* so = doc->object(sid);
    CHECK(so && so->prop("text") == "**x [[B]] tail");
    Runs rr = decodeRuns(so->prop("runs"), so->prop("text").size());
    CHECK(rangeAllHave(rr, 0, 3, kBold) && !rangeAllHave(rr, 3, 6, kBold));
    doc->renamePage(pb, "Beta Notes");
    // a styled run AFTER the link must move with the text; a link to another page must stay as is
    Object m2 = textObj(10, 10, "[[Beta Notes]] and [[Alpha]] tail");
    m2.setProp("runs", "29,4,1,,,");  // "tail"
    int64_t mid = doc->addObject(pd, m2);
    doc->renamePage(pb, "Longer title here");
    const Object* mo = doc->object(mid);
    CHECK(mo && mo->prop("text") == "[[Longer title here]] and [[Alpha]] tail");
    Runs mr = decodeRuns(mo->prop("runs"), mo->prop("text").size());
    size_t tp = mo->prop("text").find("tail");
    CHECK(rangeAllHave(mr, tp, tp + 4, kBold) && !rangeAllHave(mr, 0, tp, kBold));
    doc->renamePage(pb, "Beta Notes");
  }
  int upd2 = -1;
  doc->renamePage(pb, "Bad [title]", &upd2);  // cannot be written in brackets: links are left alone
  CHECK(upd2 == 0 && doc->page(pa)->objects[0].prop("text") == "go to [[Beta Notes]] now");
  doc->renamePage(pb, "Beta Notes");
  upd2 = -1;
  doc->renamePage(pa, "Alpha2", &upd2);
  CHECK(upd2 == 1);  // exactly one object (the mixed one) mentions Alpha

  // Overlay: found -> page address, missing -> create address; stored runs are untouched.
  std::string txt = "x [[Beta notes]] y [[Nope]]";
  std::string enc = wikiOverlay(txt, "", [&](const std::string& t) { return (long long)doc->pageByTitle(t); });
  Runs ov = decodeRuns(enc, txt.size());
  bool sawPage = false, sawNew = false;
  for (const Run& r : ov) {
    if (r.url == "cairn-page:" + std::to_string(pb)) sawPage = true;
    if (r.url == "cairn-new:Nope") sawNew = true;
  }
  CHECK(sawPage && sawNew);
  CHECK(wikiOverlay("no links", "", [](const std::string&) { return 0LL; }).empty());
  CHECK(rangeAllHave(ov, 2, 14, kUnderline));
  std::string html = richToHtml(txt, ov, true);
  CHECK(html.find("href=\"page-" + std::to_string(pb) + ".html\"") != std::string::npos);
  CHECK(html.find("cairn-new") == std::string::npos);  // the red one is not a link in HTML
  CHECK(richToHtml(txt, ov, false).find("page-") == std::string::npos);  // clipboard copy never gets it
  Runs hostile = decodeRuns("0,2,0,,,cairn-page:12x", 2);
  CHECK(richToHtml("ab", hostile, true).find("page-") == std::string::npos);  // digits only

  bool exported = false;
  for (const ExportFile& f : exportSite(*doc, "t"))
    if (f.path == "page-" + std::to_string(pa) + ".html" &&
        f.data.find("href=\"page-" + std::to_string(pb) + ".html\"") != std::string::npos) exported = true;
  CHECK(exported);
  std::string mdw = exportMarkdown(*doc);
  CHECK(mdw.find("go to [[Beta Notes]] now") != std::string::npos);  // kept in wiki form, not escaped
  MdInline back = parseMarkdownInline("go to [[beta notes]] now");
  CHECK(back.text == "go to [[beta notes]] now");
  std::remove(path.c_str());
}

static void testAudioSniff() {
  std::printf("audio sniff\n");
  using namespace cairn;
  AudioKind k;
  std::string wav = std::string("RIFF\x24\x00\x00\x00WAVEfmt ", 16);
  CHECK(sniffAudio(wav, k) && k.ext == "wav" && k.mime == "audio/wav");
  CHECK(sniffAudio(std::string("ID3\x03\x00\x00\x00\x00\x00\x00\x00\x00", 12), k) && k.ext == "mp3");
  CHECK(sniffAudio(std::string("\xFF\xFB\x90\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12), k) && k.ext == "mp3");  // bare frame
  CHECK(sniffAudio(std::string("\x00\x00\x00\x20" "ftypM4A \x00\x00\x00\x00", 16), k) && k.ext == "m4a");
  CHECK(!sniffAudio(std::string("\x00\x00\x00\x20" "ftypisom\x00\x00\x00\x00", 16), k));  // video mp4 is not audio
  CHECK(sniffAudio(std::string("MThd\x00\x00\x00\x06\x00\x01\x00\x01", 12), k) && k.ext == "mid");
  CHECK(sniffAudio(std::string("\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA", 12), k) && k.ext == "wma");
  CHECK(!sniffAudio("MZ\x90\x00 this is an exe renamed .mp3", k));
  CHECK(!sniffAudio("", k) && !sniffAudio("RIFF", k));
  CHECK(!sniffAudio(std::string("RIFF\x24\x00\x00\x00" "AVI LIST", 16), k));  // RIFF but not WAVE
  CHECK(!sniffAudio(std::string("\xFF\xE1\x90\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12), k));  // reserved MPEG version... layer 0
  CHECK(!sniffAudio(std::string("\xFF\xFB\xF0\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12), k));  // bitrate index 15 is invalid
  CHECK(!sniffAudio(std::string("\xFF\xFB\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12), k));  // free bitrate: not accepted
  std::string path = tmpPath("aud.cairn");
  auto doc = Document::create(path, false);
  int64_t pg = doc->addPage("A");
  Object o;
  o.type = ObjType::Audio;
  o.w = 240; o.h = 48;
  o.setProp("name", "clip.wav");
  CHECK(doc->addObject(pg, o) > 0);
  auto b = Document::create(tmpPath("aud2.cairn"), false);
  int64_t pb = b->addPage("B");
  auto ids = b->importObjects(pb, doc->exportObjects({doc->page(pg)->objects[0].id}), 0, 0);
  CHECK(ids.empty());  // an audio object with no audio bytes is unusable, so a paste drops it
  // The clip itself: stored, kept across a reopen (blob sweep), copied between files, exported as a player.
  std::string wavBytes = std::string("RIFF\x24\x00\x00\x00WAVEfmt \x10\x00\x00\x00", 20) + std::string(40, '\x01');
  int64_t bid = doc->addBlob(wavBytes, "audio/wav", "clip.wav");
  Object ao;
  ao.type = ObjType::Audio;
  ao.w = 240; ao.h = 48;
  ao.setPropInt("blob", (long)bid);
  ao.setProp("name", "clip.wav");
  int64_t aid = doc->addObject(pg, ao);
  doc.reset();
  doc = Document::open(path);
  std::string back;
  CHECK(doc->blob(bid, back) && back == wavBytes);  // not swept as unreferenced
  auto ids2 = b->importObjects(pb, doc->exportObjects({aid}), 0, 0);
  CHECK(ids2.size() == 1);
  std::string got;
  CHECK(b->blob((int64_t)b->object(ids2[0])->propInt("blob", 0), got) && got == wavBytes);
  bool player = false;
  for (const ExportFile& f : exportSite(*doc, "t"))
    if (f.data.find("<audio controls") != std::string::npos) player = true;
  CHECK(player);
  bool audioFile = false;
  for (const ExportFile& f : exportSite(*doc, "t"))
    if (f.path == "audio/b" + std::to_string(bid) + ".wav" && f.data == wavBytes) audioFile = true;
  CHECK(audioFile);
  // A hostile paste: an "audio" object whose bytes are not audio is dropped.
  std::string evil = b->exportObjects({ids2[0]});
  size_t at = evil.find(wavBytes);
  CHECK(at != std::string::npos);
  evil.replace(at, 12, std::string(12, 'M'));  // break the RIFF/WAVE header, same length
  CHECK(b->importObjects(pb, evil, 0, 0).empty());
  std::remove(path.c_str());
  std::remove(tmpPath("aud2.cairn").c_str());
}

int main() {
  testTags();
  testRichRuns();
  testLinkTarget();
  testHtmlPaste();
  testHtmlCopy();
  testSizeSteps();
  testUtf16Units();
  testTypingStyle();
  testExportSite();
  testReplace();
  testMarkdown();
  testTables();
  testWikiLinks();
  testAudioSniff();
  testPageOrder();
  testUrls();
  testProps();
  testBasicsAndPersistence();
  testUndoRedo();
  testRedoDiscardedProperly();
  testZOrder();
  testSearchAndLinks();
  testSafety();
  testSha256();
  testBlobs();
  testTransfer();
  testMigrationFromV1();
  testSingleFile();
  std::printf("\n%d checks, %d failed\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}

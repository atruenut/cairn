// A Cairn document: one SQLite file holding pages, objects, links and a search
// index, plus an in-memory mirror and an undo stack.
//
// Storage is write-through: every public mutator runs as a single SQLite
// transaction, so there is never an "unsaved" state to lose.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db.h"
#include "model.h"

namespace cairn {

class Document {
 public:
  // Creates a new file (fails if it exists). With seedStarter, adds two
  // explanatory pages that can't be undone away.
  static std::unique_ptr<Document> create(const std::string& path, bool seedStarter = true);
  // Opens an existing Cairn file; refuses anything that isn't one.
  static std::unique_ptr<Document> open(const std::string& path);
  ~Document();

  const std::string& path() const { return path_; }
  const std::vector<Page>& pages() const { return pages_; }
  const Page* page(int64_t id) const;
  const Object* object(int64_t id) const;
  uint64_t revision() const { return revision_; }

  // Called after every change, including undo/redo.
  std::function<void()> onChange;

  // ---- pages ----
  int64_t addPage(const std::string& title);
  bool renamePage(int64_t id, const std::string& title);
  // `tags` (optional) is cleaned with normalizeTags; it changes in the same undo step.
  bool setPageProps(int64_t id, int w, int h, int bg, const std::string* tags = nullptr);
  // Every tag in use with its page count, sorted by name (case-insensitive).
  std::vector<std::pair<std::string, int>> allTags() const;
  bool deletePage(int64_t id);  // refuses to delete the last page
  // A new page with its objects, as ONE undo step ("Import"). Returns the page id.
  int64_t addPageWithObjects(const std::string& title, int w, int h, const std::string& tags,
                             std::vector<Object> protos, const char* label = "Add Page");
  // Moves a page to position `newIndex` (0-based, clamped) in the page order. One undo step.
  // Returns false if the page is unknown or already there.
  bool movePage(int64_t id, int newIndex);

  // ---- objects ----
  int64_t addObject(int64_t pageId, Object proto, const char* label = "Add Object");
  std::vector<int64_t> addObjects(int64_t pageId, std::vector<Object> protos, const char* label);
  bool updateObjects(const std::vector<Object>& afters, const char* label);
  bool updateObject(const Object& after, const char* label) {
    return updateObjects(std::vector<Object>{after}, label);
  }
  bool deleteObjects(int64_t pageId, const std::vector<int64_t>& ids, const char* label = "Delete");
  bool reorder(int64_t pageId, const std::vector<int64_t>& ids, bool toFront);

  // ---- undo ----
  bool canUndo() const { return cursor_ > 0; }
  bool canRedo() const { return cursor_ < stack_.size(); }
  std::string undoLabel() const { return cursor_ > 0 ? stack_[cursor_ - 1].label : std::string(); }
  std::string redoLabel() const { return cursor_ < stack_.size() ? stack_[cursor_].label : std::string(); }
  bool undo();
  bool redo();
  void clearHistory();

  // ---- queries ----
  // Page ids matching the text (prefix match on every word, plus title substring).
  // Empty text returns every page in order.
  std::vector<int64_t> search(const std::string& text) const;

  // Find & replace in the text of text objects and the labels of buttons (not titles).
  // onlyPage = 0 means every page. dryRun counts without changing anything. One undo step.
  struct ReplaceResult { int matches = 0, objects = 0, pages = 0; };
  ReplaceResult replaceAll(const std::string& find, const std::string& repl, bool matchCase,
                           int64_t onlyPage, bool dryRun);
  // Pages whose buttons point at `pageId`.
  std::vector<std::pair<int64_t, std::string>> backlinks(int64_t pageId) const;  // buttons and [[wiki links]]
  int64_t pageByTitle(const std::string& title) const;  // ASCII case-insensitive, 0 if none

  // Writes a consistent copy of the whole file to `path` (fails if it exists).
  void saveCopy(const std::string& path) const;

  // ---- blobs (images and other attachments) ----
  // Stores data once per unique content (SHA-256) and returns its id. Not part of
  // undo: an object that references the blob is what undo adds and removes.
  // Unreferenced blobs are swept the next time the file is opened.
  int64_t addBlob(const std::string& data, const std::string& mime, const std::string& name);
  bool blob(int64_t id, std::string& data, std::string* mime = nullptr,
            std::string* name = nullptr) const;

  // ---- clipboard / cross-file transfer ----
  // Serializes objects (with the blobs they use) into a self-contained payload.
  std::string exportObjects(const std::vector<int64_t>& ids) const;
  // Pastes such a payload onto a page, offset by (dx,dy), as one undo step.
  // A malformed or truncated payload imports nothing and returns an empty list.
  std::vector<int64_t> importObjects(int64_t pageId, const std::string& payload, int dx, int dy,
                                     const char* label = "Paste");

 private:
  Document() = default;

  struct PageChange {
    std::optional<Page> before, after;
  };
  struct ObjChange {
    std::optional<Object> before, after;
  };
  struct UndoEntry {
    std::string label;
    std::vector<PageChange> pages;
    std::vector<ObjChange> objs;
  };

  void initSchema();
  int checkHeader();  // returns the file's schema version
  void migrate(int fromVersion);
  size_t purgeUnusedBlobs();
  void loadAll();
  void seedStarter();

  void commit(UndoEntry e);
  void runEntry(const UndoEntry& e, bool forward);
  void applyPageChange(const PageChange& c, bool forward);
  void applyObjChange(const ObjChange& c, bool forward);
  void applyPageState(const std::optional<Page>& state, int64_t id);
  void putObjectRaw(const Object& o);
  void eraseObjectRaw(int64_t id, int64_t pageId);
  void finalizeDirty();
  void reindexFts(const Page& p);
  void rebuildLinks(const Page& p);

  Page* findPageMut(int64_t id);
  static int maxZ(const Page& p);
  static int minZ(const Page& p);
  void fireChanged();

  std::string path_;
  mutable Db db_;
  std::vector<Page> pages_;
  int64_t nextPageId_ = 1;
  int64_t nextObjId_ = 1;
  std::set<int64_t> dirtyPages_;
  std::vector<UndoEntry> stack_;
  size_t cursor_ = 0;
  uint64_t revision_ = 0;
};

}  // namespace cairn

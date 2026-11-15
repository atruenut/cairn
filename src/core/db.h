// Thin RAII wrapper over the SQLite C API. No Windows headers; pure C++17.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "sqlite3.h"

namespace cairn {

struct DbError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Db {
 public:
  Db() = default;
  ~Db() { close(); }
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // `path` is UTF-8. With create=false the file must already exist.
  void open(const std::string& path, bool create) {
    close();
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    int rc = sqlite3_open_v2(path.c_str(), &h_, flags, nullptr);
    if (rc != SQLITE_OK) {
      std::string msg = h_ ? sqlite3_errmsg(h_) : "out of memory";
      close();
      throw DbError("Cannot open file: " + msg);
    }
    sqlite3_busy_timeout(h_, 2000);
  }

  void close() {
    if (h_) {
      sqlite3_close_v2(h_);
      h_ = nullptr;
    }
  }

  bool isOpen() const { return h_ != nullptr; }
  sqlite3* handle() const { return h_; }

  void exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(h_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
      std::string msg = err ? err : "unknown error";
      sqlite3_free(err);
      throw DbError("SQL error: " + msg);
    }
  }

 private:
  sqlite3* h_ = nullptr;
};

class Stmt {
 public:
  Stmt(Db& db, const char* sql) : db_(db.handle()) {
    if (sqlite3_prepare_v2(db_, sql, -1, &s_, nullptr) != SQLITE_OK)
      throw DbError(std::string("SQL error: ") + sqlite3_errmsg(db_) + " in: " + sql);
  }
  ~Stmt() { sqlite3_finalize(s_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bindInt(int i, int64_t v) { return check(sqlite3_bind_int64(s_, i, v)); }
  Stmt& bindText(int i, const std::string& v) {
    return check(sqlite3_bind_text(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT));
  }
  Stmt& bindNull(int i) { return check(sqlite3_bind_null(s_, i)); }
  Stmt& bindBlob(int i, const std::string& v) {
    return check(sqlite3_bind_blob64(s_, i, v.data(), (sqlite3_uint64)v.size(), SQLITE_TRANSIENT));
  }

  // true while a row is available
  bool step() {
    int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(std::string("SQL error: ") + sqlite3_errmsg(db_));
  }
  void run() {
    while (step()) {
    }
  }

  int64_t colInt(int c) const { return sqlite3_column_int64(s_, c); }
  std::string colText(int c) const {
    const unsigned char* t = sqlite3_column_text(s_, c);
    int n = sqlite3_column_bytes(s_, c);
    return t ? std::string(reinterpret_cast<const char*>(t), (size_t)n) : std::string();
  }

  std::string colBlob(int c) const {
    const void* p = sqlite3_column_blob(s_, c);
    int n = sqlite3_column_bytes(s_, c);
    return p ? std::string(static_cast<const char*>(p), (size_t)n) : std::string();
  }

 private:
  Stmt& check(int rc) {
    if (rc != SQLITE_OK) throw DbError(std::string("SQL bind error: ") + sqlite3_errmsg(db_));
    return *this;
  }
  sqlite3* db_;
  sqlite3_stmt* s_ = nullptr;
};

}  // namespace cairn

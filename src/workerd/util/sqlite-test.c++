// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite.h"
#include <kj/test.h>
#include <errno.h>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace workerd {
namespace {

void setupSql(SqliteDatabase& db) {
  // Initialize the database with some data.

  // TODO(sqlite): Do this automatically and don't permit it via run().
  db.run("PRAGMA journal_mode=WAL;");

  {
    auto query = db.run(R"(
      CREATE TABLE people (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        email TEXT NOT NULL UNIQUE
      );

      INSERT INTO people (id, name, email)
      VALUES (?, ?, ?),
            (?, ?, ?);
    )", 123, "Bob"_kj, "bob@example.com"_kj,
        321, "Alice"_kj, "alice@example.com"_kj);

    KJ_EXPECT(query.changeCount() == 2);
  }
}

void checkSql(SqliteDatabase& db) {
  // Do some read-only queries on `db` to check that it's in the state that `setupSql()` ought to
  // have left it in.

  {
    auto query = db.run("SELECT * FROM people ORDER BY name");

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 321);
    KJ_EXPECT(query.getText(1) == "Alice");
    KJ_EXPECT(query.getText(2) == "alice@example.com");

    query.nextRow();
    KJ_ASSERT(!query.isDone());
    KJ_EXPECT(query.getInt(0) == 123);
    KJ_EXPECT(query.getText(1) == "Bob");
    KJ_EXPECT(query.getText(2) == "bob@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }

  {
    auto query = db.run("SELECT * FROM people WHERE people.id = ?", 123l);

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 123);
    KJ_EXPECT(query.getText(1) == "Bob");
    KJ_EXPECT(query.getText(2) == "bob@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }

  {
    auto query = db.run("SELECT * FROM people WHERE people.name = ?", "Alice"_kj);

    KJ_ASSERT(!query.isDone());
    KJ_ASSERT(query.columnCount() == 3);
    KJ_EXPECT(query.getInt(0) == 321);
    KJ_EXPECT(query.getText(1) == "Alice");
    KJ_EXPECT(query.getText(2) == "alice@example.com");

    query.nextRow();
    KJ_EXPECT(query.isDone());
  }
}

KJ_TEST("SQLite backed by in-memory directory") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    setupSql(db);
    checkSql(db);

    {
      auto files = dir->listNames();
      KJ_ASSERT(files.size() == 2);
      KJ_EXPECT(files[0] == "foo");
      KJ_EXPECT(files[1] == "foo-wal");
    }
  }

  {
    auto files = dir->listNames();
    KJ_ASSERT(files.size() == 1);
    KJ_EXPECT(files[0] == "foo");
  }

  // Open it again and make sure tha data is still there!
  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);

    checkSql(db);
  }
}

class TempDirOnDisk {
public:
  TempDirOnDisk() {}
  ~TempDirOnDisk() noexcept(false) {
    dir = nullptr;
    disk->getRoot().remove(path);
  }

  const kj::Directory* operator->() {
    return dir;
  }
  const kj::Directory& operator*() {
    return *dir;
  }

private:
  kj::Own<kj::Filesystem> disk = kj::newDiskFilesystem();
  kj::Path path = makeTmpPath();
  kj::Own<const kj::Directory> dir = disk->getRoot().openSubdir(path, kj::WriteMode::MODIFY);

  kj::Path makeTmpPath() {
    const char* tmpDir = getenv("TEST_TMPDIR");
    kj::String pathStr = kj::str(
        tmpDir != nullptr ? tmpDir : "/var/tmp", "/workerd-sqlite-test.XXXXXX");
#ifdef _WIN32
    if (_mktemp(pathStr.begin()) == nullptr) {
      KJ_FAIL_SYSCALL("_mktemp", errno, pathStr);
    }
    auto path = disk->getCurrentPath().evalNative(pathStr);
    disk->getRoot().openSubdir(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY |
                                     kj::WriteMode::CREATE_PARENT);
    return path;
#else
    if (mkdtemp(pathStr.begin()) == nullptr) {
      KJ_FAIL_SYSCALL("mkdtemp", errno, pathStr);
    }
    return disk->getCurrentPath().evalNative(pathStr);
#endif
  }
};

KJ_TEST("SQLite backed by real disk") {
  // Well, I made it possible to use an in-memory directory so that unit tests wouldn't have to
  // use real disk. But now I have to test that it does actually work on real disk. So here we are,
  // in a unit test, using real disk.

  TempDirOnDisk dir;
  SqliteDatabase::Vfs vfs(*dir);

  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    setupSql(db);
    checkSql(db);

    {
      auto files = dir->listNames();
#ifdef _WIN32
      KJ_ASSERT(files.size() == 2);
      KJ_EXPECT(files[0] == "foo");
      KJ_EXPECT(files[1] == "foo-wal");
#else
      KJ_ASSERT(files.size() == 3);
      KJ_EXPECT(files[0] == "foo");
      KJ_EXPECT(files[1] == "foo-shm");
      KJ_EXPECT(files[2] == "foo-wal");
#endif
    }
  }

  {
    auto files = dir->listNames();
    KJ_ASSERT(files.size() == 1);
    KJ_EXPECT(files[0] == "foo");
  }

  // Open it again and make sure tha data is still there!
  {
    SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::MODIFY);

    checkSql(db);
  }
}

}  // namespace
}  // namespace workerd

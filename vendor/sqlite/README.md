# Vendored SQLite

SQLite 3.54.1, source id `2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76`.

Provenance (checked 2026-11-17): rebuilt from the official `sqlite/sqlite` repository at tag `version-3.54.1`
(commit `57520227c95396ce39ba6ecaac8d475b78dd3964`) and compared byte for byte with the files here.
`sqlite3.h` and `sqlite3ext.h` are identical; `sqlite3.c` is identical when built with one extra option,
which is baked into the parser tables:

    ./configure
    make sqlite3.c OPT_FEATURE_FLAGS="-DSQLITE_ENABLE_UPDATE_DELETE_LIMIT"

(needs `tclsh`). Without that option the only differences are the generated parser constants and the
`SQLITE_UDL_CAPABLE_PARSER` define. Cairn does not rely on `UPDATE ... LIMIT`, so either build works.

The other compile-time options Cairn uses are in the `Makefile` and `build.bat`.

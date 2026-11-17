# Cairn

**A software experiment, made for fun.** Cairn is not a product and has no goal, market
or roadmap beyond "what would it be like to build this?" It exists because it is enjoyable to
make, and it will go wherever curiosity goes next. Use it, break it, fork it, ignore it.

The idea: a local-first multimedia knowledge environment for Windows, inspired by IBM
LinkWay. Pages are freeform canvases of text, images, buttons and shapes you can link
together. Buttons can open other pages, files, folders or web links, and web addresses typed
into text become clickable links in Run mode. Animated GIFs play. Text keeps its formatting
(bold, italic, underline, strikethrough, code, colours, sizes, links) and pasting from a web page
brings its styling along. Everything lives in one `.cairn` file (SQLite) and every change is
written immediately: there is no Save step.

It is written in collaboration with Claude (an AI assistant). `DEVLOG.md` is Claude's
first-person diary of the work: what was tried, what broke, what was and was not verified.

Native Win32, C++17, plain Makefile, builds with MinGW-w64 gcc. No runtime
dependencies; one static exe (about 4 MB).

## Build

    build.bat       # Windows, no make needed (see below)
    make            # build/cairn.exe  (cross: x86_64-w64-mingw32-g++ on Linux)
    make CROSS=     # on Windows with MinGW-w64 on PATH
    make test       # UI-free core tests, run natively (303 checks)

`build.bat` needs MinGW-w64 on PATH. Easiest install:
`winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT`, then open a new
terminal. `build.bat test` runs the core tests; `build.bat clean` removes build\.

Run `cairn.exe`, or `cairn.exe path\to\file.cairn` (created if missing).
With no argument it reopens your last file, or creates
`Documents\Cairn\My Cairn.cairn`. Settings live in `%APPDATA%\Cairn\cairn.ini`.

## Keys

Ctrl+E Edit/Run mode · Ctrl+Z / Ctrl+Y undo/redo · Ctrl+Shift+T/B/R/I add text/
button/rectangle/image · Ctrl+C/X/V copy/cut/paste (objects, images, text, files) ·
Ctrl+P command palette · Alt+Up/Down (or drag in the list) reorder pages ·
Ctrl+B/I/U, Ctrl+Shift+X, Ctrl+K format the selection (in a text box being edited) or the whole
selected text object · Ctrl+Shift+V paste as plain text · drag files from Explorer onto a page · F2 edit text · Alt+Enter properties · Ctrl+D duplicate ·
Ctrl+F find page · Alt+Left/Right history · Ctrl+wheel zoom.

**Toolbar and right-click.** The toolbar has the common actions (new page, back/forward,
move page, tags, undo/redo, clipboard, add text/button/shape/image, text formatting, zoom,
Edit and Snap toggles); hover for tooltips. Right-click an object, empty page space, or
a link for the matching menu (cut/copy/paste, add here, open/copy link, format, order, properties).

**Function-key bar.** The strip along the bottom is F1 Help, F2 Edit Text, F3 Run/Edit, F4 Properties,
F5 Duplicate, F6 Rename Page, F7 New Page, F8 Delete, F9 Palette, F10 Find. Click the cells or press the keys;
View > Function Key Bar hides it.

**Text size.** Ctrl+] / Ctrl+[ (or the toolbar's A buttons) make the selected text objects and buttons one step
bigger or smaller; inside a text box being edited they resize just the selected characters.

**Command palette.** Ctrl+P. Drag it by its top strip; it reopens where you left it.

**Live styled editing.** While you edit a text box, bold, italic, underline, strikethrough, code, colours, sizes
and links show as you work (it uses Windows' RichEdit control, falling back to a plain box if that's unavailable).
Ctrl+Z / Ctrl+Y inside the box undo and redo typing and formatting as separate steps. Pressing Ctrl+B / I / U
(or Ctrl+Shift+X for strikethrough) with nothing selected sets the style of the next characters you type; the
toolbar's B / I / U buttons show the style at the caret.

**Find and Replace.** Ctrl+H (Edit menu) replaces text across this page or all pages, in text objects and button
labels. It tells you how many matches it found before changing anything, keeps bold/colour/links attached to the
right words, and the whole replace is a single undo step.

**Export as Website.** File > Export as Website writes a new folder of plain HTML (an index, one page per
Cairn page with objects in their places, the images, and working page-to-page buttons). No scripts; web links
are kept, buttons that open local files are not. It never writes into an existing folder.

**Wiki links.** Type `[[Page title]]` in any text object. In Run mode it is a blue underlined link to the page
with that title (case-insensitive). If no page has that title it shows in muted red, and clicking it creates the
page (one undo step) and goes there. The target page's "Linked from this page" list includes pages that mention
it this way. The brackets stay in the text. Renaming a page rewrites `[[links]]` to it on every page, in the same undo step
(unless the new title contains brackets). The website
export turns them into real links between pages; Markdown export keeps them as `[[Title]]`, which other
note tools understand.

**Button scripts.** Think of a script as a short recipe attached to a button: when you click the button in Run mode,
Cairn does the recipe's steps in order. The "Buttons and scripts" page in the starter file has a working counter you can
copy from. A button's Properties has a Script box. When it is not empty, clicking the button in Run mode
runs the script instead of its Go-to / Open settings. It is a deliberately tiny language: no loops, no files, no
network, and each line runs at most once, so a script always ends. One statement per line, `#` starts a comment:

    goto "Page title"     goto next | prev | first | last      back
    set n = n + 1         numbers and "text"; an unset variable is 0; + joins text
    text "Counter" = "Clicked " + n + " times"     set the text of the text object whose Script name is Counter
    play "Clip"           stop                     a clip whose Script name or file name is Clip, on this page
    open "https://example.org"                     only what a link button could open, with the same checks
    say "hello"                                    a message box
    if n >= 3 then say "Three!"                    = != <> < > <= >=; one statement after then; no nesting

Give an object a Script name in its Properties so scripts can find it. A script is checked when you press OK and is
not saved if it has an error (the old one is kept); at run time an error stops the script and says which line.
Variables last until the document is closed. Changes a script makes to text are one undo step each. Scripts do not
run on an exported website, where a scripted button is shown as plain text.

**Split view.** View > Split View (Ctrl+\) shows two pages side by side. Each pane has its own page, its own
Edit/Run mode and its own zoom, and it is a full pane: you can edit in either. Commands, the toolbar, the page
list and the status bar follow the pane you last clicked, which is marked with a blue bar along its top. Click a
page in the list to load it into the active pane. Handy for reading one page while writing another, or for
copying objects between pages with Ctrl+C / Ctrl+V. The second pane starts in Run mode. Drag the divider
between the panes to resize them; whether the split was on, and the divider position, are remembered between
sessions (the page shown in the second pane is not).

**Text boxes** grow taller while you type, so the text is never cut off (they never shrink on their own).

**Audio.** Edit > Add Audio (or drop a file on the page) embeds a WAV, MP3, M4A, WMA or MIDI clip in the file.
In Run mode the player has a play/pause button, a stop button, a seek bar (click to jump), elapsed / total
time and a volume slider (the volume carries over to later clips). Only one clip plays at a time. Properties has Loop and "Play
when the page opens". Playback goes through Windows' own media codecs, so a format your Windows can't decode
will say so instead of playing. Files are recognised by their contents, not their name, and clips are written
to a temporary folder only while in use (removed on exit). The website export includes a player.

**Tables.** Edit > Add Table. A table is a grid of plain-text cells. In Edit mode, double-click a cell to type in
it: Enter finishes, Esc cancels, Tab moves to the next cell (Shift+Tab the previous), and Tab in the last cell adds
a row. Right-click a cell for Table > insert or delete a row or column. F2 / Properties edit the whole table as
text instead: one row per line, cells between `|` bars (a literal bar is `\|`). The first row can be a header.
Tables are searchable, export to the website as real HTML tables and to Markdown as GitHub-style tables, and a
Markdown table in an imported file becomes a table object. Cells are plain text (no styling), equal-width,
and capped at 100 rows by 20 columns.

**Folder listing.** Edit > Add Folder Listing puts a live view of a folder on the page: folders first, then
files, hidden dot-files left out. In Run mode, click an entry to open it (the same safety checks as any link
button apply) and use the wheel over it to scroll. The path can be relative to the .cairn file, so a library
can move as a unit. It rereads the folder every couple of seconds, so it is never stale.

**Markdown.** File > Export as Markdown writes the text of every page (one `# heading` per page, bold/italic/
strikethrough/code/links kept). File > Import Markdown turns a `.md` file into pages: each `# heading` starts a
page, paragraphs, lists, quotes and code blocks become text objects, and it is one undo step per page. Text only;
images are not exported and imports never fetch anything. Unsafe link schemes are dropped.

**Copy out.** Copying styled text puts HTML on the clipboard too, so bold, colours, sizes and links
survive pasting into Word, mail clients and web editors.

**Tags.** Give a page tags in Page Properties (comma separated; spaces become hyphens).
They show in the page list, are searchable (`#tag` in the find box filters exactly), and the
toolbar tag button lists every tag with a count.

## Downloads

Built by GitHub Actions on every push to `main`. When the version in `src/win/main.cpp` is new, a Release is
published automatically: see the Releases page for the exe (zipped, and bare). Each run's build is also kept as
an artifact on its Actions page.

## Layout

    src/core   UI-free: SQLite wrapper, data model, Document (undo, search, links)
    src/win    Win32: canvas, property form, application shell
    tests      core tests    vendor/sqlite  amalgamation    res  icon, manifest, rc
    DEVLOG.md  the build diary (read this for the why)

Safety: a button will not run .exe/.bat/script files from a document; it offers
to show them in Explorer. Only http/https/mailto links are opened.

Status: early (v0.22.0). See DEVLOG.md for what is verified and what is not.

## License

MIT, see `LICENSE`.

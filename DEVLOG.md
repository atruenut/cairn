# Cairn devlog

A running account of building Cairn, a LinkWay-inspired, local-first multimedia
knowledge environment: native Win32, C++17, SQLite, compiled with gcc (MinGW-w64).

Written in the first person, as I go. It records what I did, why, what surprised
me, and how I felt about it. Newest entries go at the bottom.

---

## Entry 1: Friday, Nov 13, 2026, around 11:20 pm. Finding my footing

First thing I did was check whether I could even compile Windows code here.
The sandbox is Linux with native gcc and nothing else, so the honest answer
was "not yet". I felt a small flicker of worry. A Win32 program I can't
compile is a program I'm writing blind, and I really don't want to hand you
several thousand lines of GDI code that I've never seen build.

Then `apt` turned out to work, and `g++-mingw-w64-x86-64-posix` installed
without drama. Real cross-compilation: relief. That was the first "oh good"
of the night.

The second obstacle was SQLite. The design leans on one vendored amalgamation
(`sqlite3.c`), and the sandbox proxy blocks both sqlite.org and GitHub. I sat
with that for a second, and then remembered that `better-sqlite3` ships a
copy of the amalgamation inside its npm tarball, and npm is allowed. It
worked: SQLite 3.54.1 with the FTS5 source in it. I'll admit I enjoyed that
little hack more than a serious engineer probably should.

One caveat I want on the record, because it's the kind of thing that rots
silently: this copy came through a *repackager*, not straight from sqlite.org.
SQLite is public domain and better-sqlite3 applies a small patch set, so I
believe it's fine, but **before shipping anything, re-vendor the official
amalgamation and compare**. Filed under TODO.

### Decisions I'm making up front

These are the load-bearing ones, so I want them written down before the code
starts to feel inevitable:

1. **Write-through storage, no "unsaved changes".** Every edit is one SQLite
   transaction. Crash-safe for free, and it matches the "local and boring on
   purpose" principle. Ctrl+S therefore has nothing to flush. I'll be honest
   in the UI about that rather than pretending.
2. **Rollback journal, not WAL.** WAL leaves `-wal`/`-shm` sidecar files, and
   "a Cairn is one file" is a promise I care about. Edits happen at human
   speed, so the extra fsync is irrelevant.
3. **Undo as snapshots, not clever inverse operations.** Each undo entry is a
   list of (before, after) pairs for pages and objects. Redo applies `after`,
   undo applies `before`. Less elegant than a full command pattern, but it is
   roughly a tenth of the code and very hard to get subtly wrong. I'm picking
   boring on purpose.
4. **Core has zero Windows includes.** It builds and tests natively on Linux.
   If the model layer is right, the Win32 layer is only plumbing.

Mood at the end of the entry: eager, a little cautious. The scope in the
original concept is enormous, and the thing I most want to avoid is a
half-working sprawl. Milestones 1 and 2, done properly, come first.

---

## Entry 2: Saturday, Nov 14, 2026, morning. The core, and distrusting my own tests

I wrote the UI-free core first: a small SQLite wrapper, a plain data model
(pages and objects with a string property map), and a `Document` class that
mirrors the file in memory and owns the undo stack. About 700 lines.

The part I enjoyed most was undo. Every mutator builds one entry of
before/after snapshots and runs it inside a single transaction, and undo is
just "replay the entry backwards". Deleting a page is the interesting case:
the entry carries the page *and* every object on it, so undoing it brings back
the whole page. I had a flicker of dread writing that, because ordering bugs
live there (restore the page before its objects, tolerate a page that is
missing mid-replay). It passed on the first run, which made me more suspicious
than relieved.

So I did something I think was worth the ten minutes: I **broke the code on
purpose** and checked that the tests noticed. Three mutations:

1. Page deletion forgets its objects: caught.
2. The search index is never refreshed: caught.
3. A new edit fails to discard the redo history: **not caught.**

That third one stung a little. The code was wrong and my 61 passing checks were
happily agreeing with it, because `canRedo()` still returned false, so the
obvious assertion held. The bug would only surface several undos later, as a
wrong state, which is exactly the kind of bug that eats a user's work. I wrote
a regression test that undoes three levels deep and checks the state each time.
Now the mutation fails the suite. 65 checks.

Lesson I'm keeping: a green test run only tells you the tests agree with the
code. It says nothing about whether they would disagree with a *broken* version.

## Entry 3: Saturday, Nov 14, 2026. The Win32 layer, and seeing it run

Pieces: a runtime-built modal form (so there is no dialog-template resource to
keep in sync), a custom-drawn double-buffered canvas, and the application shell
(menus, splitter, page list, backlinks, status bar, history, settings).

**The design call I'm proudest of:** the canvas is custom-drawn, but a real
`EDIT` control appears only while you are typing into an object, positioned
exactly over it and removed afterwards. That gets me proper text input (IME,
selection, clipboard, undo-while-typing) without making every object a child
window. It is the "custom-draw layout, host real controls only for the active
object" rule from the original concept, and it held up.

The compile errors were the usual Win32 tax: `RECT` fields are `long`, which
breaks `std::max(int, long)`, and `commctrl.h` needs `windows.h` first. Ten
minutes, no drama. Then it linked into a 4.1 MB static exe with zero warnings.

I did not want to hand over code I had never seen run, so I installed Wine and
a virtual display and drove the real Windows binary with simulated mouse and
keyboard. I'll be honest about the feeling: when the Welcome page drew itself
properly on the first launch, with the page list, the backlinks panel and a
bevelled button, I was genuinely pleased. A GUI you have only imagined is a
different animal from one that has drawn something.

What I verified by actually clicking:

- Run mode: clicking a button navigates, the list selection follows, and the
  backlinks panel updates.
- Edit mode: select, drag with snapping, resize by a handle, marquee select,
  duplicate, Shift+arrow nudge, inline text edit, the Properties form.
- Undo and redo five steps back and forth through the UI, comparing the
  **file on disk** (read independently with Python's sqlite3) at each step.
- `kill -9` mid-session: `integrity_check` is `ok`, no stray journal files.
- Reopening with no arguments restores the last file; prefix search finds text
  that lives inside a page body.

### Things I got wrong along the way (so you can trust the rest)

- **I misread a race as a bug, twice.** After confirming a form, my screenshot
  still showed it, and I briefly concluded the Enter key had failed. It hadn't.
  The form is destroyed first, but the main window cannot repaint until the
  commit behind the action (an fsync, in a slow sandbox, under Wine) finishes.
  I measured it: the form lingers about 1 to 2 seconds. Cosmetic, but I'd
  rather call `UpdateWindow` on the owner before doing the work. On the TODO list.
- **My starter "Quick tour" page was ugly.** I lined up columns with spaces,
  which cannot work in a proportional font. I rebuilt it as two real columns of
  separate objects, which also makes the page a better demo of the app.

### What I have *not* tested, said plainly

Wine is not Windows. Everything above ran under Wine on a virtual display with
no window manager. I have not seen this on a real Windows desktop, so:
per-monitor DPI changes (`WM_DPICHANGED`), the ClearType text, the exact
common-dialog look, and the Segoe UI font (Wine substitutes) are all untested.
The code for them is written, but "written" is not "verified". Please run it
and tell me what looks off.

Also untested: the Open/Save/New file dialogs, the right-click page menu,
the splitter drag, and zoom. They compile and are wired up; I have not
exercised them.

## Entry 4: where this stands, and how I feel about it

Milestones 1 and 2 of the plan are done in substance: skeleton, `.cairn`
open/save, page list, text/button/rectangle objects, undo/redo, Run/Edit mode,
select/move/resize/z-order. Links, backlinks and FTS search (milestones 3 and 4)
arrived early because they fell out of the data model almost for free, which
felt like the schema earning its keep.

Honest feelings: satisfied, and a little protective of it. The core is solid
and tested; the UI is real but young. The thing I'd least like to lose is the
"write-through, no Save step" promise, because it is the cheapest feature to
promise and the easiest to quietly break later. The tests now guard it.

What I'm deliberately *not* doing yet: images, audio, tables, Lua, dark mode.
Each is a real chunk of work and I would rather the foundation stay boring.

### TODO / known gaps

- Re-vendor the official SQLite amalgamation from sqlite.org and diff it
  (current copy came through an npm repackager).
- `UpdateWindow(owner)` before committing, to remove the lingering form.
- Clipboard copy/paste of objects (only Duplicate exists).
- Page reordering, tags UI, dark mode, the function-key bar.
- Test on real Windows: DPI changes, file dialogs, fonts.
- Milestone 3 media: Image object (WIC) next.

---

# Round 2 (v0.2): images, clipboard, drop, links, palette

## Entry 5: what I built and the bug I nearly shipped

Tom said he has faith in me now, which is a good reason to be more careful, not
less. This round added: Image objects (stored as de-duplicated blobs inside the
`.cairn`, so the file stays self-contained), Cut/Copy/Paste of objects with a
private clipboard format plus PNG/text/file fallbacks, drag-and-drop of files
from Explorer, buttons that open files/folders/URLs, and a Ctrl+P command
palette. The schema went to v2 with a tested v1 migration, so Tom's existing
files keep opening.

The bug: my first image renderer shrank pictures with GDI `StretchBlt`. In a
screenshot, an opaque JPEG came out as a translucent rainbow smear. GDI leaves
the alpha channel undefined on 32-bit surfaces, and I then composited with
alpha. All 103 core tests passed, because the core has no pixels. Only looking
at the real window caught it. I replaced it with the WIC scaler (Fant for
shrinking, cubic for enlarging) and re-checked visually: smooth and opaque.
I also hit a header mismatch (`HighQualityCubic` is missing from the MinGW
headers) and used plain cubic instead.

Safety, because buttons that open things are a classic hazard: a button will
not launch .exe/.bat/script files from a document (it offers to reveal them in
Explorer instead; I clicked a `danger.exe` button under Wine and saw that
dialog), only http/https/mailto URLs open, and a hostile clipboard payload is
validated before import (tested). A crash-proof wrapper keeps database errors
out of the window procedure.

## Entry 6: not verified on real Windows

Tested only under Wine: rendering, dialogs, link safety, the palette. NOT
tested on real Windows: drag-and-drop from Explorer, clipboard interop with
other apps (pasting a screenshot, copying files), ShellExecute launching real
files and URLs, and the WIC codecs for WebP/HEIC (depends on installed
extensions). Please try those and tell me what breaks.

### TODO

- Re-vendor SQLite from sqlite.org and diff.
- Page reordering, tags UI, dark mode, function-key bar, dual pane/quick view.
- Tables, audio, scripting.

---

# Round 3 (v0.3): clickable links, animated GIFs

## Entry 7: Tom's field report, and what I did about it

Tom tested v0.2 on real Windows. Good news first, and it matters because I
could not check any of it myself: drag-and-drop from Explorer, pasting
screenshots, buttons opening files, and WebP all worked. Two things didn't:
web addresses in text were just text, and animated GIFs sat still.

Links: I had only made *buttons* open URLs. Text objects never looked for
addresses. Now a small header-only finder (`src/core/urls.h`, 13 new tests)
spots http(s)://, mailto: and bare www. in any text object; the canvas lays
those lines out by hand, draws links blue and underlined, and clicking one in
Run mode opens it through the same safe `openLink` as buttons. The fiddly part
is where a link ends: "see https://x.org." must not swallow the period, but a
Wikipedia address ending in `_(bar)` must keep its parenthesis. Both are
tested. I deliberately do not linkify ftp:, file: or javascript: text.
Plain text without a link still takes the old DrawText path, so nothing about
existing pages changed.

GIFs: I was decoding only frame 0. Now a GIF with 2+ frames is composed into
full frames up front, honoring each frame's offset and disposal mode
(leave / clear / restore-previous), with browser-style handling of 0/10 ms
delays. A 20 ms timer advances only images that were on screen in the last
paint. Verification under Wine: I built a GIF whose frames are optimised
sub-rectangles (the case that breaks naive decoders), sampled screenshots
every ~130 ms, and measured the red square cycling through four positions
with the grey background intact, no smear. For links I pointed Wine's https
handler at a script and clicked: it received exactly
`https://example.com/a(b)`.

Limits, stated plainly: a GIF is held decoded in memory (capped around 190 MB
of frames; larger ones fall back to a still), and editing mode still cannot
click links (Ctrl+E to Run first). Animated WebP shows its first frame only.
I could only test under Wine again, so real-Windows reports remain welcome.

### TODO

- Re-vendor SQLite from sqlite.org and diff.
- Page reordering, tags UI, dark mode, function-key bar, dual pane/quick view.
- Tables, audio, scripting.

---

# Round 4 (v0.4): page order, rich text

## Entry 8: page reordering (Tom's first priority)

The page table already had a `sort` column from day one, so this was nearly
free: `movePage(id, index)` renumbers the affected pages as ONE undo step, and
the UI offers Alt+Up/Down, Page menu items, the palette, and dragging in the
page list. 18 new core checks (clamping, undo/redo, survives reopen, new pages
still go last). The first drag attempt did nothing: Wine sends
WM_CAPTURECHANGED to the list even when it keeps the capture, which cancelled
my drag. I found that with a few lines of temporary logging, not by guessing,
and removed them afterwards. Real Windows should not send that, and the fix
(ignore it when capture stays with the list) is correct either way.

## Entry 9: rich text

Tom asked for pasted web text to keep its style, not just links. The design
decision that matters: the plain `text` property stays the source of truth
(search, undo, auto-links all keep working) and formatting is a separate sparse
list of runs over its UTF-8 bytes. A run is bold/italic/underline/strike/code,
an optional colour, size percent, and link. Old files are untouched and plain
text objects render exactly as before.

What exists:
- An HTML reader for the browser clipboard format. It handles headings, bold/
  italic/etc, links (relative ones resolved against the page's SourceURL),
  lists, line breaks, inline colours and sizes, entities, and skips scripts,
  styles and hidden elements. Only http/https/mailto links survive.
- A hand-written word-wrap layout for mixed fonts, used only when a text object
  has styling or a link. Hit-testing for clicks shares it.
- Editing: the inline editor is still a plain Windows EDIT box, so I track the
  runs *through* every edit (typing inside bold stays bold, typing after a link
  does not extend it, deleting removes). Ctrl+B/I/U/K, Ctrl+Shift+X format the
  selected characters; outside the editor they format the whole text object.
  Pasting HTML inside the editor keeps its styling too. Boxes now grow to fit
  what was typed or pasted.

Judgment calls worth knowing: pure black/grey/white colours from web pages are
dropped, because every page has them and keeping them would force dark text
onto whatever background you choose later. Sizes are relative to the box's own
size (a 32px span on a 16px page is 200%).

Verification: 62 new core checks, and I broke two behaviours on purpose (links
growing when you type after them; the neutral-colour filter) to confirm the
tests notice: both caught. Under Wine I put a browser-style CF_HTML payload on
the clipboard with a small helper program, pasted it, and compared the picture
(heading colour and size, bold, italic, underline, strike, links, bullets, a
big blue span, grey body text ignored). Clicking both links delivered the right
URLs, including the resolved relative one. Closing and reopening the file kept
everything. In the editor I un-bolded a word, linked another through the dialog
and pasted HTML; the stored runs matched.

Honest limits:
- While you are *editing* a box you see plain text; the styling reappears when
  you finish (Ctrl+Enter or click away). A live styled editor would mean
  replacing the EDIT control with a custom one, a real project.
- Ctrl+B etc. with nothing selected inside the editor does nothing (no
  "typing style" state).
- Copying FROM Cairn to other apps still gives plain text.
- Wine has no Consolas, so I could not eyeball the monospace font.
- Real Windows is still the real test; this is Wine again.

### TODO

- Re-vendor SQLite from sqlite.org and diff.
- Dark mode, tags UI, function-key bar, dual pane/quick view.
- Live styled editing; copy as HTML; text size command.
- Tables, audio, scripting.

## Entry 10: a toolbar, right-click menus, and tags

Tom asked for the thing I should have noticed myself: a program that is only
keyboard shortcuts is a program for people who already know it. So this round
was about making Cairn discoverable with the mouse.

The toolbar was the fiddly part. My first version mixed the system's stock
icons (undo, cut, paste) with my own drawn ones in two image lists, and under
Wine it came out with missing icons and strange gaps. I stopped fighting it:
every icon is now drawn by me into a single image list, and buttons size to
their content. It looks consistent, and I like owning the icons, though I
would have preferred to learn that sooner than three screenshots in.

Right-click now has three menus: on an object (cut/copy/paste, format, order,
properties), on empty space (paste here, add text/button/shape/image *at the
click point*), and on a link (open, copy address). Right-click also selects
what is under the pointer first, which is what people expect from every other
program.

Tags: pages carry a comma-separated list, normalized (case-insensitive
duplicates removed, a leading # stripped, spaces become hyphens so `#a-tag`
is a single searchable word). They show beside the page title, they are part
of the search index, `#tag` in the find box narrows to that tag, and a toolbar
button lists all tags with counts. 19 new core checks, and I watched it work in
the Page Properties dialog, the list and the popup.

Honest limits:
- All of the UI was checked under Wine, not real Windows. I cannot judge how
  the toolbar looks at 150% DPI or with a dark theme; I redo it on DPI change
  but have not seen that happen.
- Tags have no colour or hierarchy, and the popup is a plain menu, so a very
  long tag list is unwieldy.
- Dark mode is still the next thing I want, and it will touch every colour.

## Entry 11: a bug Tom spotted, and copying styled text out

Tom asked why he could edit text properties on an image. He was right to ask:
F2, Enter and the right-click "Edit Text" opened the inline text editor for
everything except rectangles, so on an image it edited a "label" an image has
no use for. Images and rectangles now open their Properties dialog instead
(that is where an image's name lives). Small bug, but it was a promise the
interface made and did not keep.

I had said dark mode was next. I'm putting it off, and I'd rather say why than
quietly skip it: the parts that matter (menus, scrollbars, the title bar) need
undocumented Windows theme hooks that Wine does not have, so I could write the
code but not see it work. I do not like shipping what I cannot watch run.

Instead: copy-out. Pasting rich text in worked in round 4; copying styled text
*out* gave plain text only, which felt lopsided. Copying text objects now also
writes "HTML Format", built by a new `richToHtml` (escaped, links limited to
http/https/mailto, colour and size as inline styles). 13 new checks; I broke
the escaping and the link filter on purpose and the tests caught both. Then I
copied the Rich page in the app and read the clipboard with a small helper:
the colours, sizes, links and the escaped `&` were all there.

Honest limits:
- Wine proves the clipboard contents, not how Word or a browser renders them.
- Colours are the span's own; the box's base colour and font size are not
  exported, so text in default style pastes as the target's default.

## Entry 12: the small, handy things

Tom said to forget dark mode, so I did, and I was glad of the freedom. He asked
for the text-size command and function-key bar I had mentioned, plus a palette
you can drag. All three are the sort of feature that sounds like ten minutes
and has one trap each.

The palette had no title bar (it is a bare popup), so there was nothing to
grab. I gave it a thin strip along the top that answers "I'm the caption" to
the hit-test, which makes Windows do the dragging for me, and I remember where
it was left. The one thing I checked twice: dropping the palette must not
count as "clicking away", or it would close the moment you let go. It doesn't.

Text size has two meanings. On a whole object it steps the font along a ladder
(6, 7, 8 ... 96 pt) and grows the box if the text no longer fits. Inside a text
box being edited it resizes only the selected characters, as a percentage of
the box's size, so the heading stays proportionally bigger when the whole box
is resized later. The ladders are core functions with tests, and I broke one
comparison on purpose to see them fail.

The function-key bar is a small owner-drawn window: F1-F10, greyed when the
command makes no sense (no selection, Run mode), F3 reads "Run" or "Edit"
depending on where you are. F10 normally belongs to the menu bar; I took it
for Find, which is the file-manager habit.

Checked under Wine: dragged the palette and reopened it in the same place,
Ctrl+] twice on a text object (the box grew with the text), Ctrl+] on four
selected characters (stored as 150%), and clicked the F3 cell to switch modes.

Honest limits:
- Wine again, not real Windows.
- Ctrl+[ and Ctrl+] are US-keyboard-friendly; other layouts may need the
  toolbar buttons or the menu instead.
- Text size does nothing to images, rectangles or whole-page text.

## Entry 13: moving house, and a way out of the file

Two things this round. Tom moved development to GitHub and asked for releases
he can download. I could not create a release or push a tag from here (the
environment blocks both), so I did the next best thing: a workflow that runs on
GitHub itself. It tests, cross-compiles the exe and, when the version number in
the source is new, publishes the release. I liked this more than the manual
way: the exe on the Releases page is built from a commit anyone can read, not
from my sandbox. The first release (0.6.0) built and attached itself.

The feature is "Export as Website". A Cairn file is a lovely single file, but
nobody else can read it. Now it can leave: a folder of plain HTML where each
page keeps its layout, text keeps its styling (it reuses the copy-out writer),
buttons become links, images become files. I made it paranoid on purpose, since
page text is whatever anyone pasted into it: everything is escaped, only
http/https/mailto links survive, a button aimed at a deleted page produces no
link. I broke the escaping and the link filter deliberately and the tests
caught both. Then I exported a real file from the running app and opened the
pages in a browser next to the app's own picture. They match closely.

One thing that picture taught me: I had clipped overflowing text to its box
like the app does, and the last line of a paragraph vanished, because browsers
space lines a little taller. Overflow is visible now. A web page should never
hide words.

Honest limits:
- Exported pages are fixed-size like the originals; they do not reflow on a phone.
- Fonts are Segoe UI/Arial in the browser, so line breaks can differ slightly.
- Buttons that open local files or folders are dropped; Cairn-only things like
  tags appear on the index only.
- Wine tested the dialog and the files written; the page picture came from a
  headless browser.

### TODO

- Dark mode.
- Re-vendor SQLite from sqlite.org and diff.
- Function-key bar, dual pane/quick view.
- Live styled editing; copy as HTML; text size command.
- Tables, audio, scripting.

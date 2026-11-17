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

## Entry 27: three notes from Tom

Tom's feedback was three separate things, and the third was the most useful: "I don't
understand what the scripting is about, you need explanations of that." Fair. I had
documented the commands but never said what scripting is for, and a feature that can't be
discovered doesn't exist. So: a new starter page, "Buttons and scripts", reached from the
Quick tour. It explains in two paragraphs what a script is (a recipe a button follows when
clicked), shows a live counter whose button runs the very script printed beside it, and lists
every command. A unit test runs that exact script, so the page cannot rot into a lie.

Text boxes now grow while you type. Before, a box only grew when you finished editing, so a
new box with five lines of text spilled out of its frame until you clicked away. The change is
small: on every edit notification I measure the rich text at the box's width and, if it needs
more height, store a transient taller copy (the same override map used for dragging) and move
the editor window to match. Finishing the edit stores the real height in the undo step;
Escape throws the transient one away. Checked under Wine: a new box typed to five lines grew
live and saved at 115 units tall.

Audio got a real player: play/pause, stop, a seek bar, elapsed and total time, and a volume
slider. While doing it I found a latent bug in my own earlier design: MCI posts an "aborted"
notification when you stop a clip, and if the next clip had already started, that late message
would have stopped the new clip. Notifications are now matched to the device id of the clip that
is playing, and aborted/superseded notices (which my own seek and pause calls cause) are
ignored. Wine has no sound device, so I verified drawing and clicking only; I could not hear
anything, and seek/pause/volume on real codecs are unverified until Tom tries them.

## Entry 28: a divider you can drag

Small round, two things from my own list. The divider between the two panes in split view
can now be dragged (15% to 85%), and the split view, with the divider position, comes back the
next time Cairn starts. Both live in the settings file, not the document, because a split is
how you are looking at a file rather than part of it.

Wine caught one real bug: I first restored the split before the first page was shown, so the
right pane opened on "No page selected". Restoring after the page is navigated fixed it. Checked
under Wine by dragging, closing and reopening. Still not persisted: which page the second pane
shows, and per-pane history.

## Entry 29: scripts that belong to the page

I had planned a page-level "on open" script as a page property, which would have meant a
schema change. Then I noticed an object can do the job: a new object type, "On-open script",
that is a dashed yellow note in Edit mode and invisible in Run mode. It gets undo, copy and
paste, duplication and cross-file transfer for free, and old files and old versions of Cairn
are unaffected (the importer's type range just grows to 8; a file with one opened in an older
build would show an "Object" it cannot draw, nothing worse).

Danger worth designing for: page A's script says goto B, page B's says goto A. Automatic opens
now stop after four hops. Under Wine, an on-open script of `goto "Rich"` on Welcome sent me
straight to Rich when I opened Welcome from the list. The starter scripting page mentions it.

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

## Entry 14: replace, and a test that was lying to me

Find and Replace across pages. The interesting part was not finding text (that
is a loop) but keeping rich text intact: replacing a word must not smear its
bold over its neighbours or silently drop it. My first version lost the style
whenever the replaced text *was* a whole styled word, because the edit tracker
(built for typing) reads "everything in this run changed" as "the run is gone".
The tests caught it immediately; the fix is that a replacement inherits the
style of the first character it replaces.

The humbling bit was the mutation testing. I deleted the dry-run guard to see
if the tests would notice, and they did not: 0 failures. I had written the
guard twice (once when collecting changes, once before applying them), so
removing either one alone is invisible; only removing both is caught. That is
a harmless kind of redundancy, but it was a good reminder that a passing
mutant is a question, not always a bug. I checked each one.

The UI is deliberately cautious: it counts first and asks "Replace 4 matches in
3 objects on 2 pages?" before touching anything, and the whole thing is one
undo. I watched it in the app: a lowercase search matched "BOLDPASTE" and the
stored text and its bold run came out right.

Honest limits:
- Matching is plain text, case-insensitive only for ASCII letters (so "e" and
  "E" match, but "é" and "É" do not). No regular expressions, no whole-word option.
- Page titles, tags and image names are not replaced.
- Wine only.

## Entry 15: seeing the bold while you type

Until now, formatting a word showed up only after you finished editing, because
the inline editor was a plain EDIT box that can't draw styles. It always felt
slightly wrong to press Ctrl+B and see nothing. Now the editor is a RichEdit
control that *displays* the styling live.

The design choice that made this tractable: my runs list is still the single
source of truth, and the RichEdit is only a view of it. I never read formatting
back out of the control. After every change I repaint the runs onto it. That
meant none of the careful storage, copy/paste and replace logic had to change.

Two things bit me, and I'm glad I tested rather than trusted:

1. **Undo broke.** RichEdit records my repainting as undoable edits, so Ctrl+Z
   undid my own repaint, which triggered another repaint, forever: nothing ever
   undid. I turned its native undo off and gave the editor a small history of
   its own (text, styles, caret; typing within a second is one step, a format
   command or a paste is its own step).
2. **Worse: Ctrl+Z was undoing the document.** The main loop decides "the user is
   typing, leave the shortcuts alone" by checking that the focused window's class
   is "Edit". A RichEdit has another class, so Ctrl+Z, Ctrl+A, Delete and the
   F-keys were all firing on the *page* while someone typed. It would have been a
   miserable bug on real Windows. The guard now knows both classes.

I also moved the byte-offset-to-UTF-16 mapping into the tested core, since an
emoji is two UTF-16 units and four UTF-8 bytes. In the app I confirmed bold
lands on exactly the right character after accented letters, in both the
RichEdit editor and the plain fallback (which I forced by hiding msftedit).

Honest limits:
- Wine's RichEdit, not Microsoft's: the real one may differ in line spacing and
  caret details. The undo and shortcut logic is mine and shouldn't.
- Long text boxes (over 20,000 bytes) fall back to showing no live styling.
- Ctrl+B with nothing selected still does nothing; "typing style" is still open.
- Line breaks in the editor can differ a little from how the finished box lays out.

## Entry 16: pressing Ctrl+B and having it mean something

With styling visible, the obvious gap was that Ctrl+B with nothing selected did
nothing. Every word processor lets you press it and then type. So: a "typing
style". Toggle bold with the caret in an empty spot, and the next characters
you type are bold; toggle it again and they are not.

I kept the decision logic in the tested core: given the old text, the new text
and where the caret was, either the change is a pure insertion exactly at the
caret (apply the pending style to precisely those characters) or it isn't
(do nothing). Anything else, like the caret moving, a deletion or a paste that
replaces something, simply cancels the pending style rather than guessing. My
mutation test found a hole there first: a replacement that also grew the text
slipped through, so I added that case.

The toolbar's B, I and U buttons now show as pressed when the text at the caret
(or the selection, or the pending style) has that style. That small thing made
the feature feel finished; without it you press Ctrl+B and have no idea what
state you are in.

Honest limits:
- Only the five simple styles (bold, italic, underline, strikethrough, code).
  Colour and size are not "typing styles" yet.
- The pending style is spent by the next edit: if you toggle bold, then click
  elsewhere and type, the styling is dropped rather than carried along.
- Wine only.

## Entry 17: a way in and out for plain text

Cairn files are SQLite, which is fine for me and useless for anything else, so I
added Markdown in both directions. It is text only, on purpose.

Export writes one `# Page title` per page, then its text objects in reading order
(top to bottom, then left to right). Styled runs become `**bold**`, `*italic*`,
`~~strike~~`, `` `code` `` and `[text](url)`. Whitespace at the edge of a styled run
is moved outside the markers, since `** bold**` is not bold in Markdown. Characters
that would turn into formatting are escaped, so text survives a round trip.
Underline has no Markdown form and is dropped. Images are written as a placeholder.

Import is the part with decisions in it. Each `# heading` starts a page; text before
the first one goes on a page named after the file. Paragraphs, list runs, quotes and
tables become text objects stacked down the page, code fences become monospace, and
`##` headings become larger bold text. Heights are estimated from line counts, so
the layout is approximate. Hostile input is handled: a 2 MB cap, bounded search
windows so an unmatched `[a](` repeated thousands of times cannot go quadratic, a
nesting limit, and the same link-scheme filter as everywhere else.

I added `Document::addPageWithObjects` so a page and its contents are one undo step.

Two of my own mistakes in the tests are worth noting: I used the wrong separator
in the run encoding (the test silently had no runs and so checked nothing useful),
and my first link parser stopped at the first `)`, which cut
`[x](javascript:alert(1))` in half. Targets now allow balanced parentheses.

Verified: 330 core checks natively. The menu commands compile but I have not driven
the file dialogs under Wine, so the UI side of this is untested.

## Entry 18: a window onto a folder

LinkWay's pages could point at files; mine could only point at one file at a
time with a button. A Folder Listing object shows what is actually in a folder
and stays current, so a page can be the front door to a directory of papers or
photos without me maintaining a button per file.

It is a new object type (5) with a `path` and a font size. Relative paths resolve
against the .cairn file's folder, the same rule buttons use. It reads at most every
two seconds, caps at 2000 entries, sorts folders first then by name, and hides
dot-files. In Run mode a click opens the entry through the existing `openLink`, so
the refusal of odd URL schemes and the other safety checks apply unchanged; the
wheel scrolls the list when the pointer is over it.

Mistakes: the transfer importer (copy/paste between documents) rejected object
types above 4, so a pasted listing would have been silently dropped. Caught it
by writing the paste test first, and now it is in the suite. My first draft also
used a small triangle glyph for folders that rendered as an empty box in the font
I use; folders now just end in a backslash.

Verified: 331 core checks natively; under Wine the listing draws correctly
(header, folders first, hidden file omitted). I did not test clicking or the wheel
there, and it is not exported to the website or Markdown.

## Entry 19: tables, without a spreadsheet

Tables were on my TODO list for a long time because the honest version (click a
cell, type in it, Tab to the next) is a second text editor inside the canvas, and I
already had to build one of those. I took the cheap route first: a table is one
text prop, a row per line with cells between bars, edited in the Properties box.

That decision pays off elsewhere. The format is its own serialisation, so search,
copy/paste between documents and undo work unchanged. Export is easy because the
logic is a few pure functions in the tested core (`parseTable`, `formatTable`,
`tableToMarkdown`, `tableToHtml`). It is forgiving on input: outer bars are
optional, short rows are padded, caps of 100 rows by 20 columns by 2000 characters
per cell apply, and Markdown separator rows can be skipped. Escaped bars and
backslashes survive a round trip, which a test checks. Markdown import used to turn tables into
a monospace blob; it now creates real table objects, and export writes them back
out as GitHub tables.

What it cannot do: cells have no styling, columns are equal width, and you cannot
click a cell to edit it. Those are the things a proper grid editor would add.

Also in this release, the website export now skips Folder Listings on purpose (they
show the viewer's own disk).

Verified: 349 core checks natively; under Wine a table draws with its header
shading, grid and an escaped bar. I did not drive the edit dialog there.

## Entry 20: [[wiki links]]

Cairn's links so far were buttons, which suits a layout tool but not a note-taking
one. In a knowledge environment you want to be mid-sentence, type a page's name
in double brackets and have it be a link.

The design question was where the link lives. I decided it does not live
anywhere: the text keeps its literal `[[Title]]`, and the link is derived whenever
the page is drawn. `findWikiLinks` (pure, tested) finds the spans; the canvas looks
each title up (`Document::pageByTitle`, case-insensitive) and overlays a
display-only run: underlined blue with an internal `cairn-page:<id>` address if the
page exists, muted red if not. Nothing is written back, so there is no link table to
get stale. If you create the missing page later, the red link turns blue by itself.
Renaming a page breaks references to the old title, which I accept for now.

Backlinks needed the same thing: `backlinks()` now also scans text for mentions
instead of relying on the stored link table, which only knew about buttons. That is a
scan over all text per page change, fine at this scale and worth revisiting if a
document ever gets enormous.

The new internal address cannot come from outside: people can only type
http/https/mailto links, so `cairn-page:` is only ever produced by the overlay (a hand-edited
file could carry one, and all that does is navigate to a page that exists).
Hostile input: 500 links per text, titles capped at 120 bytes, and 5000 unclosed `[[`
finishes immediately (tested).

I also hit a compile error (`_wtoll` is not in MinGW's headers) that the native
test build cannot see, since the Windows code is not part of it. It is a reminder to
always run the full `make`, not just `make test`.

Verified: 363 core checks; under Wine, the existing page's link draws blue, the
missing one red, and a click navigates and shows up in the target's "Linked from"
panel.

## Entry 21: red links make pages, and exports follow links

Two follow-ups to wiki links. A red link is now a "create this page" button:
clicking it adds the page (one undo step, so a mis-click is Ctrl+Z) and goes there,
which is how wikis grow. For that, the display overlay moved from the canvas into
the core (`wikiOverlay`), so the website exporter can use the same code: it resolves
`[[Title]]` and writes a real link to `page-<id>.html`. Unresolved titles export as
muted red text with no link.

`richToHtml` only turns the internal address into a link when told to
(`pageLinks`, which only the exporter sets), and only for an address that is
`cairn-page:` followed by digits. Clipboard copy never gets it, and a hostile
file carrying `cairn-page:12x` does not.

Markdown export keeps `[[Title]]` as typed rather than escaping the brackets,
since that is how other note tools write it, and import already leaves it as text,
so it round-trips.

I got the red-link prefix length wrong (9 instead of 10) and the first page it
created was titled ":Nowhere"; the native tests cannot see the Windows
handler, so only the Wine run caught it. Fixed and re-run: the page is created
with the right name, appears in the list, and the source page shows in its "Linked
from" panel.

Verified: 373 core checks natively; the red-link click under Wine as above. The
exported site's links are covered by a core test but I did not open the result
in a browser this time.

## Entry 22: renames keep their links

The one thing I called out as a known hole in wiki links was that renaming a page
orphaned every `[[old title]]`. `renamePage` now rewrites those mentions in the
same undo step as the title change, so Ctrl+Z puts everything back at once.

Details that mattered: the rewrite goes back to front within a text so earlier
offsets stay valid, and each replacement goes through `shiftRunsForEdit`, so a bold
word after the link stays bold and in the right place. Only mentions that resolve to
the page being renamed are touched; a `[[Other]]` in the same text is left alone. If
the new title cannot be written inside brackets (it has brackets or a line break, or
is over 120 bytes) links are left as they are rather than half-rewritten, and the
caller can see how many were updated.

My first version of the tests passed while two deliberately broken versions of the
code also passed: neither the run shifting nor the "only this page" check was
exercised, because my styled run sat before the link and my text had only one
link. I added a case with a run after the link and a second link to a different
page, and both broken versions are now caught. This is the second time a green
suite taught me less than a red mutant did.

Verified: 385 core checks; under Wine, renaming "Quick tour" to "Tour" via F6
rewrote `[[quick tour]]` on the front page to `[[Tour]]` and left `[[Nowhere]]` alone.

## Entry 23: typing into table cells

Tables could only be edited as a block of bars and text, which is honest but
tedious. Now in Edit mode you double-click a cell and type. I did not build a
grid editor; the cell editor is the same plain EDIT control the canvas uses for
button labels, placed over the cell, committing on Enter or when focus leaves.
Tab commits and opens the neighbour, and Tab off the last cell appends a row and
grows the object by one row height, which is the single spreadsheet habit I
wanted most. Row and column insert/delete live on the right-click menu and are
pure functions in the tested core (`tableInsertRow` and friends, with the row and
column caps and the "never fewer than one row/column" rule).

A bug the Wine run found, worth recording: the first Tab committed the cell but
the next editor opened and was immediately committed away. Destroying an EDIT
posts a WM_KILLFOCUS-driven "commit" message to the canvas, and by the time that
message arrived the *new* editor existed, so the stale message committed and closed
it. The commit message now carries the window that sent it, and the canvas ignores
messages from an editor that is no longer current. The text editor had the same
latent race (it was just never reopened that quickly). A window handle can in
principle be reused, so the guard is good but not airtight.

Also fixed while in there: capping a cell at 2000 bytes could split a multi-byte
character; it now cuts before the character.

Verified: 401 core checks; under Wine I double-clicked a cell and typed, tabbed
to the end to append a row, and inserted a column from the right-click menu; the
saved file shows each change. I did not check undo of these by hand, but each is
one `updateObject` call like every other edit.

## Entry 24: sound

LinkWay pages could talk; mine were silent. Audio is a new object type holding a clip
as a blob inside the .cairn, so a document stays one file and travels with its sounds.

The core part is small and tested: `sniffAudio` identifies WAV, MP3, M4A, WMA and MIDI
from their first bytes, and refuses everything else, including a video MP4 or an .exe
renamed .mp3. The same check guards pasting objects between files, so a crafted
clipboard cannot smuggle non-audio bytes in as a "clip".

Playback is MCI (winmm): the clip is written to a per-run temp folder and opened
by path, since MCI cannot play from memory. It plays through Windows' own codecs,
one clip at a time, with a finished/stopped notification back to the canvas for loop
and for resetting the button. Autoplay starts when you enter a page in Run mode and
sound stops when you leave it.

The important bug was not in the audio at all. The "delete blobs nothing references"
sweep on open, and the copy/paste and export paths, only knew about images, so an audio
clip would have been silently deleted the next time the file was opened. I found it by
reading every place that mentions `ObjType::Image` before writing the UI, and wrote the
reopen test first; disabling the sweep fix makes the suite crash, as it should.

One more guard: a looping clip that "ends" in under 300 ms is not restarted, so a
broken or zero-length clip cannot spin the app in a loop.

Verified: 423 core checks; under Wine the object draws, a click extracts a byte-identical
temp copy and opens it, and looping does not spin the CPU. Wine has no sound device, so I
have not heard anything play. That needs a real Windows machine, and that is the first
thing to try when you test this release.

## Entry 25: two panes

The "dual pane" item on my list is really "see two pages at once". I considered a
cheap read-only second view, but the useful case is copying between pages and
editing one while reading the other, so I went for a second full canvas. That sounded
like a rewrite, and mostly was not. The canvas was already a self-contained class
over a shared `Document` (all state like selection and zoom is per canvas, all content
and undo is in the document), so a second instance just works. The main window had
103 direct uses of `g.canvas`; those now go through `cv()`, which returns the active
pane, and a handful of places that mean "every pane" (document change, document swap,
DPI change) were handled by hand. The active pane is whichever was last clicked or
focused, marked with a thin blue bar. The page list, backlinks, title and status bar
follow it.

Things that had to change because there were suddenly two of everything:
- The table-cell editor's "Tab means navigate" flag was a global; it is now a property
  of the editor window, so one pane's editor closing cannot switch off the other's.
- Audio state is per process (MCI has one device alias), so it is shared by both panes
  and both repaint when a clip starts or stops; before this, the second pane could never
  have started a clip while the first one was playing.
- If the page shown in the other pane is deleted, that pane falls back to the first page
  instead of showing nothing.

Not done: the split is not remembered between sessions, the divider is fixed at
half and cannot be dragged, and back/forward history is shared rather than per pane.

Verified: 423 core checks (nothing in the core changed); under Wine, Ctrl+\ splits the view,
clicking the right pane makes it active, choosing a page in the list loaded it into the right
pane only, and Ctrl+E switched only the right pane to Edit mode. I did not test editing the
same page in both panes at once, or dragging objects between panes.

## Entry 26: scripting, on a leash

The last item on Tom's list is the one I was most cautious about: a document that
runs code. A file someone sends you must never be able to do harm just by being
clicked, and "a scripting language" usually means the opposite of that. So the
design question came before any code: what is the smallest language that is
still useful, and where can it not reach?

The answer is a closed command set, not a general language. There are no loops, so a
script runs each line at most once and always terminates; I do not need a timeout
because there is nothing to time out. There is no file or network access and no way
to name an arbitrary program. Every effect goes through a `ScriptHost` interface that
the application implements: go to a page, set the text of a named text object, play a
named clip, open a link (through the same `openLink` checks as a link button, so it
still refuses programs and odd schemes), show a message. A hostile script can at worst
annoy: navigate, change text on the current page (undoable), or pop up message boxes.
The interpreter lives in the OS-free core, so it is covered by the native tests
without a Windows machine.

Parsing happens fully before anything runs, so a syntax error on line 5 does not
leave lines 1 to 4 half-executed. Errors carry line numbers, nesting depth and string
length have caps, and `if` cannot nest, which removes a whole category of recursion
questions. Variables are session-only and per document; an unset variable reads as 0
so counters are one line.

The interface side: a button's Properties gains a Script box, checked on OK (an
invalid script is not saved and the old one is kept, with the line and reason shown),
and every object gains an optional Script name so scripts can find things. A scripted
button replaces its Go-to / Open settings; the website export shows it as plain text
since a static page cannot run it. Pasting a button carries its script along, which
is fine, because the only thing a script can do is the above.

I checked the three caps with mutation tests (disable the line cap, the nesting cap,
and the string cap in turn: each makes a test fail).

Verified: 455 core checks; under Wine, a button with `set n = n + 1`, a `text`
update and `if n >= 2 then say "Twice!"` updated a named text object ("Clicked 1
times", "Clicked 2 times") and showed the message box on the second click; undo became
available afterwards. I did not exercise goto/back/play from a script by hand; those
paths are covered by the mock-host tests only.

That finishes the list Tom gave me (cell editing, audio, split view, scripting). What
is left on my own TODO: a page-level "on open" script, remembering the split, a
draggable pane divider, and per-pane history.

### TODO

- Dark mode.
- Re-vendor SQLite from sqlite.org and diff.
- Function-key bar, dual pane/quick view.
- Live styled editing; copy as HTML; text size command.
- Tables, audio, scripting.

// Rich text for Text objects: the plain text stays the source of truth (search, links and
// undo all work on it), and formatting is a sparse list of "runs" over its UTF-8 bytes.
// Gaps between runs are default style. Also: a tolerant HTML-fragment reader used for
// pasting from web pages (CF_HTML on the Windows clipboard).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cairn {

enum RunFlags : unsigned { kBold = 1, kItalic = 2, kUnderline = 4, kStrike = 8, kMono = 16 };

struct Run {
  size_t start = 0, len = 0;  // byte range in the UTF-8 text
  unsigned flags = 0;
  int color = -1;             // 0xRRGGBB, or -1 = the object's own colour
  int size = 0;               // percent of the object's font size; 0 = 100
  std::string url;            // non-empty: this range is a link
  size_t end() const { return start + len; }
  bool isDefault() const { return !flags && color < 0 && !size && url.empty(); }
  bool sameStyle(const Run& o) const {
    return flags == o.flags && color == o.color && size == o.size && url == o.url;
  }
};

using Runs = std::vector<Run>;

// ---- storage -------------------------------------------------------------------
std::string encodeRuns(const Runs& runs);
// Never trusts its input: bad fields, overlaps and out-of-range runs are dropped/clamped.
Runs decodeRuns(const std::string& s, size_t textLen);
void normalizeRuns(Runs& runs, size_t textLen);  // sort, clamp, drop defaults, merge neighbours

// ---- editing -------------------------------------------------------------------
// Keeps runs attached to the right characters when text changes from oldT to newT
// (finds the single changed region by common prefix/suffix; snaps to UTF-8 boundaries).
// Typing inside or at the end of a styled run continues the style; links don't grow.
void shiftRunsForEdit(Runs& runs, const std::string& oldT, const std::string& newT);

// Applies fn to every run overlapping [a,b), creating runs over unstyled gaps.
void restyleRange(Runs& runs, size_t a, size_t b, size_t textLen, const std::function<void(Run&)>& fn);
bool rangeAllHave(const Runs& runs, size_t a, size_t b, unsigned flag);
void toggleFlag(Runs& runs, size_t a, size_t b, size_t textLen, unsigned flag);
// Replaces the styling of [offset, offset+overLen) with `over` (positions relative to 0).
void overlayRuns(Runs& base, const Runs& over, size_t offset, size_t overLen, size_t textLen);

// A link address typed by a person -> something safe to store, or "" if unacceptable.
// Accepts http(s)://, mailto:, and bare "www.x.org" / "x.org" (made https). Rejects every
// other scheme (javascript:, file:, data:, ...), spaces and control characters.
std::string sanitizeLinkTarget(const std::string& typed);

// ---- HTML paste ----------------------------------------------------------------
struct RichText {
  std::string text;
  Runs runs;
};
// Pulls the fragment out of a Windows "HTML Format" clipboard payload (or returns the
// input unchanged when it has no header). `sourceUrl` receives the SourceURL header.
std::string htmlFragmentFromClipboard(const std::string& clip, std::string* sourceUrl = nullptr);
// Converts an HTML fragment to text + runs: bold/italic/underline/strike/code, links,
// headings, lists, line breaks, colours and sizes from inline styles. Scripts, styles and
// hidden elements are skipped. Only http/https/mailto links are kept; relative links are
// resolved against sourceUrl's origin when given.
RichText htmlToRich(const std::string& html, const std::string& sourceUrl = "");

// ---- typing style --------------------------------------------------------------
// Toggling bold/italic/... with nothing selected sets a "typing style" for the next characters.
// styleBefore: the style flags (kBold..kMono) of the character just before `pos` (0 at the start).
unsigned styleBefore(const Runs& runs, size_t pos);
// If newT is oldT with text inserted exactly at `caret`, gives the inserted characters exactly the
// style flags `flags` (replacing the five style bits, keeping colour/size) and returns true.
// Anything else (the caret moved, text was deleted or changed elsewhere) returns false, untouched.
bool applyTypingStyle(Runs& runs, const std::string& oldT, const std::string& newT, size_t caret, unsigned flags);

// ---- text offsets --------------------------------------------------------------
// UTF-16 units in the first `byteEnd` bytes of a UTF-8 string, as a Windows text control counts
// them (a 4-byte character is two units). Tolerates malformed input and out-of-range offsets.
size_t utf16UnitsBefore(const std::string& s, size_t byteEnd);

// ---- find & replace ------------------------------------------------------------
// Replaces every non-overlapping match of `find` (left to right) in `text`, keeping `runs`
// attached to the right characters. Matching ignores ASCII case unless matchCase. Returns the
// number of replacements; empty `find`, or a result over 1 MB, changes nothing and returns 0.
int replaceInText(std::string& text, Runs& runs, const std::string& find, const std::string& repl, bool matchCase);

// ---- size steps ----------------------------------------------------------------
// One step bigger (dir > 0) or smaller (dir < 0) along a fixed ladder; always lands on a rung
// and never leaves the ladder. Points are for a whole object, percent is for a styled span.
int stepFontPt(int currentPt, int dir);       // 6 .. 96
int stepSizePercent(int currentPct, int dir);  // 50 .. 400; 0 means 100

// ---- HTML copy -----------------------------------------------------------------
// The reverse of htmlToRich: a safe HTML fragment (escaped text, <b><i><u><s><code>, links
// restricted to http/https/mailto, colour and size as inline styles, newlines as <br>).
std::string richToHtml(const std::string& text, const Runs& runs);
// Wraps a fragment in the Windows "HTML Format" clipboard envelope (byte offsets filled in).
std::string wrapClipboardHtml(const std::string& fragment);

}  // namespace cairn

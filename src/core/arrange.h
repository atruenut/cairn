// Aligning and spacing objects: pure geometry, no document or window involved.
#pragma once
#include <vector>

namespace cairn {

struct Box {
  int x = 0, y = 0, w = 0, h = 0;
};

enum class Arrange { Left, Right, Top, Bottom, CenterH, CenterV, SpreadH, SpreadV };

// Returns the boxes moved (sizes never change). Align modes line every box up with the selection's
// outer edge or centre line. Spread modes keep the two outermost boxes where they are and space the
// rest so the gaps between neighbours are equal; they need at least three boxes (otherwise nothing moves).
std::vector<Box> arrangeBoxes(const std::vector<Box>& in, Arrange mode);

// Line and arrow shapes: a Shape object whose kind is 4..9 is a straight segment inside its box.
//   4 horizontal line, 5 vertical line, 6 diagonal top-left to bottom-right, 7 diagonal bottom-left to top-right,
//   8 arrow pointing right, 9 arrow pointing down.
struct Segment {
  int x1, y1, x2, y2;
  bool arrow;  // draw a head at (x2, y2)
};
bool shapeSegment(int kind, int x, int y, int w, int h, Segment& out);  // false when kind is not a line kind

struct Pt {
  int x, y;
};
// Corners of a filled arrowhead whose tip is the segment's end: tip, then the two back corners.
// `len` is the head length, `half` half its width, in the caller's units. Degenerate segments give three equal points.
void arrowHead(const Segment& s, int len, int half, Pt out[3]);

// Is point (px, py), measured from the box's top-left, on this shape? Used so clicking the empty corner of an
// ellipse, diamond or triangle, or beside a line, does not select it. `tol` widens the shape (same unit as the
// box), so thin lines stay clickable. Rectangles (kind 0 or unknown) are the whole box.
bool shapeContains(int kind, double w, double h, double px, double py, double tol);

}  // namespace cairn

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

}  // namespace cairn

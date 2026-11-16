// Tables: a grid of plain-text cells, stored as one text prop that people can also edit by hand.
// One row per line, "| a | b | c |". A literal bar is written \| and a backslash \\ .
#pragma once

#include <string>
#include <vector>

namespace cairn {

struct Table {
  std::vector<std::vector<std::string>> rows;  // always rectangular after parseTable
  size_t cols() const { return rows.empty() ? 0 : rows[0].size(); }
};

constexpr size_t kTableMaxRows = 100, kTableMaxCols = 20, kTableMaxCell = 2000;

// Parses the line format above. Leading/trailing bars are optional, cells are trimmed, short rows
// are padded, extra rows/columns beyond the caps are dropped. With skipRules, Markdown separator
// lines such as "|---|:--:|" are skipped. Never fails: bad input gives an empty or smaller table.
Table parseTable(const std::string& text, bool skipRules = false);
std::string formatTable(const Table& t);  // parseTable(formatTable(t)) == t for any t
std::string tableToMarkdown(const Table& t, bool header);  // GitHub-style; first row is the header row
std::string tableToHtml(const Table& t, bool header);      // escaped, no styles
std::string cellInline(const std::string& cell);           // cell text made safe for one table cell

}  // namespace cairn

// Markdown in and out. Export writes the text of a document (one "# heading" per page);
// import turns a Markdown file into pages of text objects. Text only: images and rectangles are
// not exported, and import never fetches or opens anything.
#pragma once

#include <string>

#include "rich.h"

namespace cairn {

class Document;

// Styled text <-> inline Markdown (**bold**, *italic*, ~~strike~~, `code`, [text](url)).
// Underline has no Markdown form and is dropped. Only safe link targets survive either way.
std::string richToMarkdown(const std::string& text, const Runs& runs);
struct MdInline {
  std::string text;
  Runs runs;
};
MdInline parseMarkdownInline(const std::string& markdown);

std::string exportMarkdown(const Document& doc);

struct MdImportResult {
  int pages = 0, objects = 0;
  long long firstPage = 0;  // id of the first page created (0 if none)
};
// Each "# heading" starts a page; text before the first one goes on a page called fallbackTitle.
// One undo step per page. Input over 2 MB is ignored.
MdImportResult importMarkdown(Document& doc, const std::string& markdown, const std::string& fallbackTitle);

}  // namespace cairn

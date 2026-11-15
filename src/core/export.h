// Export a whole document as a small static website: one HTML page per Cairn page (objects keep
// their position and size), an index, and the images as separate files. No scripts, no external
// resources. Everything user-written is escaped; only http/https/mailto links are emitted.
#pragma once

#include <string>
#include <vector>

namespace cairn {

class Document;

struct ExportFile {
  std::string path;  // relative, forward slashes: "index.html", "page-12.html", "images/b3.png"
  std::string data;
};

// `siteTitle` names the index page. The first file is always "index.html".
std::vector<ExportFile> exportSite(const Document& doc, const std::string& siteTitle);

// File name a page is exported under ("page-<id>.html"), so links can be built from outside.
std::string exportPageFile(long long pageId);

}  // namespace cairn

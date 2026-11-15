// Plain data model for pages and objects. No I/O, no Windows.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace cairn {

enum class ObjType : int { Text = 1, Button = 2, Rect = 3, Image = 4 };

// Object properties are a small string->string map, stored one "key=value" per line.
//
// Known keys (colors are 0xRRGGBB stored as decimal; -1 means "none"):
//   Text   : text, size (pt), bold (0/1), align (0 left,1 center,2 right), color, fill
//   Button : label, target (page id), open (file path or URL), size, bold, color, fill
//   Rect   : fill, line, lw (line width), round (corner radius)
//   Image  : blob (blob id), name (original file name), iw, ih (pixel size), fit (0 stretch, 1 fit)
using PropMap = std::map<std::string, std::string>;

std::string encodeProps(const PropMap& m);
PropMap decodeProps(const std::string& s);

struct Object {
  int64_t id = 0;
  int64_t pageId = 0;
  ObjType type = ObjType::Text;
  int x = 0, y = 0, w = 120, h = 40;  // page units (96-dpi pixels)
  int z = 0;                          // paint order; higher is on top
  PropMap props;

  std::string prop(const std::string& k, const std::string& def = "") const {
    auto it = props.find(k);
    return it == props.end() ? def : it->second;
  }
  int propInt(const std::string& k, int def = 0) const {
    auto it = props.find(k);
    if (it == props.end() || it->second.empty()) return def;
    return (int)std::strtol(it->second.c_str(), nullptr, 10);
  }
  void setProp(const std::string& k, const std::string& v) { props[k] = v; }
  void setPropInt(const std::string& k, long v) { props[k] = std::to_string(v); }

  bool operator==(const Object& o) const {
    return id == o.id && pageId == o.pageId && type == o.type && x == o.x && y == o.y &&
           w == o.w && h == o.h && z == o.z && props == o.props;
  }
  bool operator!=(const Object& o) const { return !(*this == o); }
};

struct Page {
  int64_t id = 0;
  std::string title;
  int w = 960, h = 640;
  int bg = 0xFFFFFF;
  int sort = 0;
  std::string tags;
  int64_t created = 0, modified = 0;
  std::vector<Object> objects;  // kept sorted by (z, id)
};

// Tags are stored as one comma-separated string. This cleans user input: splits on commas,
// semicolons and newlines, strips a leading '#', trims and collapses spaces, drops empties and
// case-insensitive duplicates, and caps the count and length. Spaces inside a tag become
// hyphens. Result is "a, b, c".
std::string normalizeTags(const std::string& typed);
std::vector<std::string> splitTags(const std::string& stored);  // of an already-normalized string

}  // namespace cairn

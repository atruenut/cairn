#include "export.h"
#include "arrange.h"
#include "table.h"
#include "media.h"
#include "script.h"
#include "lisp.h"

#include <cstdio>
#include <algorithm>
#include <map>

#include "document.h"
#include "rich.h"

namespace cairn {
namespace {

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string css(int rgb) {
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", rgb & 0xFFFFFF);
  return b;
}

// Only formats browsers show natively; anything else is not exported as an image.
const char* extFor(const std::string& mime) {
  if (mime == "image/png") return "png";
  if (mime == "image/jpeg") return "jpg";
  if (mime == "image/gif") return "gif";
  if (mime == "image/webp") return "webp";
  if (mime == "image/bmp") return "bmp";
  return nullptr;
}

const char* kStyle =
    "body{margin:0;background:#e9ecef;font-family:'Segoe UI',Arial,Helvetica,sans-serif;color:#222}"
    "nav{max-width:960px;margin:12px auto;padding:0 12px;font-size:14px;display:flex;gap:16px}"
    "nav a{color:#0b57d0;text-decoration:none}nav a:hover{text-decoration:underline}"
    ".page{position:relative;margin:0 auto 40px;box-shadow:0 1px 6px rgba(0,0,0,.25);overflow:hidden}"
    ".page>*{position:absolute;box-sizing:border-box}"
    ".t{padding:4px;white-space:pre-wrap;overflow:visible;overflow-wrap:break-word}"
    ".t a{color:#0b57d0}"
    ".b{display:flex;align-items:center;justify-content:center;text-align:center;border:1px solid #8a8a8a;"
    "border-radius:3px;text-decoration:none;overflow:hidden;white-space:nowrap}"
    "a.b:hover{filter:brightness(.95)}.b[data-s]{cursor:pointer}"
    ".i{display:block}"
    ".au audio{width:100%;height:100%}"
    ".tb{overflow:auto}.tb table{border-collapse:collapse;width:100%}"
    ".tb th,.tb td{border:1px solid #8a8a8a;padding:3px 6px;text-align:left;vertical-align:top}"
    ".tb th{background:#e8ecf2}"
    ".index{max-width:720px;margin:24px auto;padding:0 12px}"
    ".index li{margin:6px 0}.tag{font-size:12px;color:#555;margin-left:8px}";


std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '<': o += "\\u003c"; break;
      case '>': o += "\\u003e"; break;
      case '&': o += "\\u0026"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o + "\"";
}

std::string lowerAscii(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

// Adds attributes to the opening tag of an object's HTML (it always starts with "<tag ").
void addAttrs(std::string& chunk, const std::string& attrs) {
  size_t sp = chunk.find(' ');
  if (sp != std::string::npos) chunk.insert(sp, " " + attrs);
}

std::string head(const std::string& title) {
  return "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>" + esc(title) + "</title><style>" + kStyle + "</style></head><body>\n";
}

std::string pageHtml(const Document& doc, const Page& p, const Page* prev, const Page* next,
                     std::map<long long, std::string>& imageFiles, size_t pageIndex, bool& usesScripts) {
  std::string o = head(p.title);
  o += "<nav><a href=\"index.html\">Index</a>";
  if (prev) o += "<a href=\"" + exportPageFile(prev->id) + "\">&larr; " + esc(prev->title) + "</a>";
  if (next) o += "<a href=\"" + exportPageFile(next->id) + "\">" + esc(next->title) + " &rarr;</a>";
  o += "</nav>\n";
  o += "<div class=\"page\" style=\"width:" + std::to_string(p.w) + "px;height:" + std::to_string(p.h) +
       "px;background:" + css(p.bg) + "\">\n";
  int z = 0, objNo = 0;
  std::vector<std::string> btnScripts, openScripts, objJson;
  std::vector<std::pair<std::string, std::string>> audioIds;
  std::vector<std::string> legacyVars;
  auto asLisp = [&](const std::string& src) -> std::string {
    if (src.empty()) return "";
    if (looksLikeLisp(src)) return src;
    return legacyToLisp(src, legacyVars);  // "" if it does not parse
  };
  for (const Object& ob : p.objects) {
    size_t chunkStart = o.size();
    ++objNo;
    std::string pos = "left:" + std::to_string(ob.x) + "px;top:" + std::to_string(ob.y) +
                      "px;width:" + std::to_string(ob.w) + "px;height:" + std::to_string(ob.h) +
                      "px;z-index:" + std::to_string(++z) + ";";
    switch (ob.type) {
      case ObjType::Text: {
        std::string text = ob.prop("text");
        int al = ob.propInt("align", 0);
        std::string st = pos + "font-size:" + std::to_string(ob.propInt("size", 14)) + "pt;color:" +
                         css(ob.propInt("color", 0x222222)) + ";text-align:" +
                         (al == 1 ? "center" : al == 2 ? "right" : "left") + ";";
        if (ob.propInt("bold", 0)) st += "font-weight:bold;";
        if (ob.propInt("fill", -1) >= 0) st += "background:" + css(ob.propInt("fill", 0)) + ";";
        o += "<div class=\"t\" style=\"" + st + "\">" + richToHtml(text, decodeRuns(wikiOverlay(text, ob.prop("runs"), [&](const std::string& t) { return (long long)doc.pageByTitle(t); }), text.size()), true) +
             "</div>\n";
        break;
      }
      case ObjType::Button: {
        std::string st = pos + "font-size:" + std::to_string(ob.propInt("size", 13)) + "pt;color:" +
                         css(ob.propInt("color", 0)) + ";background:" +
                         (ob.propInt("fill", -1) >= 0 ? css(ob.propInt("fill", 0)) : std::string("#e1e1e1")) + ";";
        if (ob.propInt("bold", 0)) st += "font-weight:bold;";
        std::string label = esc(ob.prop("label"));
        std::string href;
        long long target = ob.propInt("target", 0);
        if (!ob.prop("script").empty()) {
          // a scripted button runs its script (cairn.js); it is not a link
        } else if (target > 0 && doc.page(target)) href = exportPageFile(target);
        else {
          std::string open = ob.prop("open");
          std::string safe = open.empty() ? "" : sanitizeLinkTarget(open);
          if (!safe.empty()) href = safe;  // web links only; local files are not exported
        }
        if (href.empty()) o += "<div class=\"b\" style=\"" + st + "\">" + label + "</div>\n";
        else o += "<a class=\"b\" href=\"" + esc(href) + "\" style=\"" + st + "\">" + label + "</a>\n";
        break;
      }
      case ObjType::Rect: {
        int kind = ob.propInt("kind", 0);
        Segment sg;
        if (shapeSegment(kind, 0, 0, ob.w, ob.h, sg)) {  // line or arrow: inline SVG
          int line = ob.propInt("line", 0x555555), lw = std::max(1, ob.propInt("lw", 1));
          std::string col = css(line >= 0 ? line : 0x555555);
          o += "<svg style=\"" + pos + "overflow:visible;\" viewBox=\"0 0 " + std::to_string(ob.w) + " " + std::to_string(ob.h) +
               "\"><line x1=\"" + std::to_string(sg.x1) + "\" y1=\"" + std::to_string(sg.y1) + "\" x2=\"" + std::to_string(sg.x2) +
               "\" y2=\"" + std::to_string(sg.y2) + "\" stroke=\"" + col + "\" stroke-width=\"" + std::to_string(lw) + "\"/>";
          if (sg.arrow) {
            Pt hp[3];
            arrowHead(sg, std::max(8, lw * 5), std::max(4, lw * 2), hp);
            o += "<polygon points=\"" + std::to_string(hp[0].x) + "," + std::to_string(hp[0].y) + " " + std::to_string(hp[1].x) +
                 "," + std::to_string(hp[1].y) + " " + std::to_string(hp[2].x) + "," + std::to_string(hp[2].y) + "\" fill=\"" + col +
                 "\"/>";
          }
          o += "</svg>\n";
          break;
        }
        if (kind == 2 || kind == 3) {  // diamond / triangle: an inline SVG polygon
          int w = ob.w, h = ob.h, line = ob.propInt("line", 0x555555), lw = ob.propInt("lw", 1);
          std::string pts = kind == 2 ? std::to_string(w / 2) + ",0 " + std::to_string(w) + "," + std::to_string(h / 2) + " " +
                                           std::to_string(w / 2) + "," + std::to_string(h) + " 0," + std::to_string(h / 2)
                                      : std::to_string(w / 2) + ",0 " + std::to_string(w) + "," + std::to_string(h) + " 0," +
                                           std::to_string(h);
          o += "<svg style=\"" + pos + "\" viewBox=\"0 0 " + std::to_string(w) + " " + std::to_string(h) +
               "\"><polygon points=\"" + pts + "\" fill=\"" +
               (ob.propInt("fill", -1) >= 0 ? css(ob.propInt("fill", 0)) : std::string("none")) + "\" stroke=\"" +
               (line >= 0 && lw > 0 ? css(line) : std::string("none")) + "\" stroke-width=\"" + std::to_string(lw) +
               "\"/></svg>\n";
          break;
        }
        std::string st = pos;
        if (kind == 1) st += "border-radius:50%;";
        if (ob.propInt("fill", -1) >= 0) st += "background:" + css(ob.propInt("fill", 0)) + ";";
        int line = ob.propInt("line", 0x555555), lw = ob.propInt("lw", 1);
        if (line >= 0 && lw > 0) st += "border:" + std::to_string(lw) + "px solid " + css(line) + ";";
        if (ob.propInt("round", 0) > 0) st += "border-radius:" + std::to_string(ob.propInt("round", 0)) + "px;";
        o += "<div style=\"" + st + "\"></div>\n";
        break;
      }
      case ObjType::Table: {
        std::string st = pos + "font-size:" + std::to_string(ob.propInt("size", 12)) + "pt;";
        o += "<div class=\"tb\" style=\"" + st + "\">" +
             tableToHtml(parseTable(ob.prop("data")), ob.propInt("header", 1) != 0) + "</div>\n";
        break;
      }
      case ObjType::Audio: {
        long long blob = ob.propInt("blob", 0);
        std::string data;
        AudioKind ak;
        if (blob > 0 && doc.blob(blob, data) && sniffAudio(data, ak)) {
          std::string file = "audio/b" + std::to_string(blob) + "." + ak.ext;
          imageFiles[blob] = file;
          o += "<div class=\"au\" style=\"" + pos + "\"><audio controls preload=\"none\" src=\"" + file + "\"></audio></div>\n";
        } else {
          o += "<div style=\"" + pos + "border:1px dashed #999;color:#777;font-size:12px;padding:4px\">" +
               esc(ob.prop("name", "audio")) + "</div>\n";
        }
        break;
      }
      case ObjType::Script:
        break;  // behaviour only; its script is collected below and run by cairn.js
      case ObjType::Folder:
        break;  // a live view of the viewer's own disk; nothing to publish
      case ObjType::Image: {
        long long blob = ob.propInt("blob", 0);
        std::string data, mime;
        const char* ext = (blob > 0 && doc.blob(blob, data, &mime)) ? extFor(mime) : nullptr;
        if (!ext) {
          o += "<div style=\"" + pos + "border:1px dashed #999;color:#777;font-size:12px;padding:4px\">" +
               esc(ob.prop("name", "image")) + "</div>\n";
          break;
        }
        std::string file = "images/b" + std::to_string(blob) + "." + ext;
        imageFiles[blob] = file;
        o += "<img class=\"i\" src=\"" + file + "\" alt=\"" + esc(ob.prop("name")) + "\" style=\"" + pos +
             "object-fit:" + (ob.propInt("fit", 1) ? "contain" : "fill") + "\">\n";
        break;
      }
    }
    // What makes an object scriptable on the exported page: an id for scripts to find it by, and click handlers.
    {
      std::string chunk = o.substr(chunkStart);
      const std::string sname = ob.prop("sname");
      if (ob.type == ObjType::Script) {
        std::string lisp = asLisp(ob.prop("script"));
        if (!lisp.empty()) openScripts.push_back(lisp);
      }
      if (ob.type == ObjType::Button && !ob.prop("script").empty()) {
        std::string lisp = asLisp(ob.prop("script"));
        if (!lisp.empty()) {
          addAttrs(chunk, "data-s=\"" + std::to_string(btnScripts.size()) + "\"");
          btnScripts.push_back(lisp);
        }
      }
      bool visible = ob.type == ObjType::Text || ob.type == ObjType::Button || ob.type == ObjType::Rect ||
                     ob.type == ObjType::Audio || ob.type == ObjType::Image || ob.type == ObjType::Table;
      if (visible && (!sname.empty() || ob.type == ObjType::Audio)) {
        std::string id = "o" + std::to_string(objNo);
        addAttrs(chunk, "id=\"" + id + "\"");
        if (!sname.empty()) {
          const char* t = ob.type == ObjType::Text ? "text" : ob.type == ObjType::Button ? "button" : ob.type == ObjType::Rect ? "shape" : "other";
          std::string text = ob.type == ObjType::Text ? ob.prop("text") : ob.type == ObjType::Button ? ob.prop("label") : "";
          objJson.push_back(jstr(lowerAscii(sname)) + ":{\"id\":" + jstr(id) + ",\"t\":\"" + t + "\",\"x\":" + std::to_string(ob.x) +
                            ",\"y\":" + std::to_string(ob.y) + ",\"w\":" + std::to_string(ob.w) + ",\"h\":" + std::to_string(ob.h) +
                            ",\"fill\":" + std::to_string(ob.propInt("fill", -1)) + ",\"color\":" + std::to_string(ob.propInt("color", -1)) +
                            ",\"line\":" + std::to_string(ob.propInt("line", -1)) + ",\"text\":" + jstr(text) + "}");
        }
        if (ob.type == ObjType::Audio) {
          if (!sname.empty()) audioIds.emplace_back(lowerAscii(sname), id);
          if (!ob.prop("name").empty()) audioIds.emplace_back(lowerAscii(ob.prop("name")), id);
        }
      }
      o.resize(chunkStart);
      o += chunk;
    }
  }
  o += "</div>\n";
  if (!btnScripts.empty() || !openScripts.empty()) {
    usesScripts = true;
    std::string init;
    for (const std::string& v : legacyVars) init += "(define v:" + v + " 0)\n";
    std::string cfg = "{\"title\":" + jstr(p.title) + ",\"cur\":" + std::to_string(pageIndex) + ",\"order\":[";
    const std::vector<Page>& all = doc.pages();
    for (size_t i = 0; i < all.size(); ++i) cfg += (i ? "," : "") + jstr(exportPageFile(all[i].id));
    cfg += "],\"pages\":{";
    for (size_t i = 0; i < all.size(); ++i) cfg += (i ? "," : "") + jstr(lowerAscii(all[i].title)) + ":" + jstr(exportPageFile(all[i].id));
    cfg += "},\"init\":" + jstr(init) + ",\"open\":[";
    for (size_t i = 0; i < openScripts.size(); ++i) cfg += (i ? "," : "") + jstr(openScripts[i]);
    cfg += "],\"btn\":[";
    for (size_t i = 0; i < btnScripts.size(); ++i) cfg += (i ? "," : "") + jstr(btnScripts[i]);
    cfg += "],\"objs\":{";
    for (size_t i = 0; i < objJson.size(); ++i) cfg += (i ? "," : "") + objJson[i];
    cfg += "},\"audio\":{";
    for (size_t i = 0; i < audioIds.size(); ++i) cfg += (i ? "," : "") + jstr(audioIds[i].first) + ":" + jstr(audioIds[i].second);
    cfg += "}}";
    o += "<script src=\"cairn.js\"></script>\n<script>Cairn.page(" + cfg + ");</script>\n";
  }
  o += "</body></html>\n";
  return o;
}

}  // namespace

std::string exportPageFile(long long pageId) { return "page-" + std::to_string(pageId) + ".html"; }

std::vector<ExportFile> exportSite(const Document& doc, const std::string& siteTitle) {
  std::vector<ExportFile> out;
  const std::vector<Page>& pages = doc.pages();

  std::string idx = head(siteTitle) + "<div class=\"index\"><h1>" + esc(siteTitle) + "</h1><ul>\n";
  for (const Page& p : pages) {
    idx += "<li><a href=\"" + exportPageFile(p.id) + "\">" + esc(p.title) + "</a>";
    for (const std::string& t : splitTags(p.tags)) idx += "<span class=\"tag\">#" + esc(t) + "</span>";
    idx += "</li>\n";
  }
  idx += "</ul></div></body></html>\n";
  out.push_back({"index.html", idx});

  std::map<long long, std::string> imageFiles;
  bool usesScripts = false;
  for (size_t i = 0; i < pages.size(); ++i)
    out.push_back({exportPageFile(pages[i].id),
                   pageHtml(doc, pages[i], i > 0 ? &pages[i - 1] : nullptr,
                            i + 1 < pages.size() ? &pages[i + 1] : nullptr, imageFiles, i, usesScripts)});
  if (usesScripts) out.push_back({"cairn.js", cairnJsSource()});
  for (const auto& kv : imageFiles) {
    std::string data;
    if (doc.blob(kv.first, data)) out.push_back({kv.second, data});
  }
  return out;
}

}  // namespace cairn

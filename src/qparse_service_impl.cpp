#include "qparse_service_impl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/Pl_DCT.hh>
#include <qpdf/Pl_Discard.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_RunLength.hh>

// The engine umbrella headers (header-only library over qpdf).
#include <parse.h>
#include <render.h>

#include "engine_tables.h"
#include "lzw_filter.h"
#include "qpdf_readers.h"
#include "sha256.h"
#include "valid_utf8.h"

namespace grpc_qparse {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

// CMake stamps this from the image tag or git describe; the fallback keeps
// the translation unit self-contained.
#ifndef GRPC_QPARSE_BUILD_VERSION
#define GRPC_QPARSE_BUILD_VERSION "dev"
#endif

namespace {

constexpr char kBackendName[] = "grpc-qparse";
constexpr char kEngineVersion[] = "qpdf-based cell parser (qpdf 12.2.0)";

// Families this backend fills. The engine's public surface does not expose
// the raw annotation list as typed data, encryption details, attachments,
// signatures, JavaScript, the structure tree, page thumbnails, or its
// internal resource dictionaries, so those are honest unsupported verdicts
// for now.
bool FamilySupported(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_PAGE_INVENTORY:
    case pdfv1::PDF_FAMILY_TEXT_CELLS:
    case pdfv1::PDF_FAMILY_PAGE_RASTER:
    case pdfv1::PDF_FAMILY_FONTS:
    case pdfv1::PDF_FAMILY_EMBEDDED_FONTS:
    case pdfv1::PDF_FAMILY_PLACED_IMAGES:
    case pdfv1::PDF_FAMILY_VECTOR_SHAPES:
    case pdfv1::PDF_FAMILY_HYPERLINKS:
    case pdfv1::PDF_FAMILY_FORM_FIELDS:
    case pdfv1::PDF_FAMILY_OUTLINE:
    case pdfv1::PDF_FAMILY_DOC_METADATA:
      return true;
    default:
      return false;
  }
}

// Parse never decodes image samples (ParseDecodeConfig), so a placed image
// is where it is and what its XObject declares, and nothing more.
constexpr char kPlacedImagesDetail[] =
    "placement, declared pixel size and color space only: no image samples, "
    "ParseOptions.include_image_data is not served";

const char* UnsupportedDetail(pdfv1::PdfFamily family) {
  switch (family) {
    case pdfv1::PDF_FAMILY_DEEP_RESOURCES:
      return "the engine decodes page resources internally without a public "
             "typed surface; needs an engine-side patch";
    default:
      return "not exposed by the engine's public surface";
  }
}

// Page geometry as PageInfo reports it, read from the page dictionary with
// inheritance through the page tree resolved: /MediaBox, /CropBox and
// /Rotate may sit on any ancestor /Pages node (ISO 32000-1, 7.7.3.4).
struct PageGeometry {
  // The MediaBox, lower-left corner first; US Letter when the page has no
  // usable one, as PDFium falls back, and media_box_guessed says so.
  std::array<double, 4> media_box = {0.0, 0.0, 612.0, 792.0};
  // The page and its ancestors store no /MediaBox of four finite numbers
  // spanning an area: media_box is the fallback, not the document's.
  bool media_box_guessed = false;
  // The CropBox clipped to the MediaBox: the part of the page a viewer
  // shows and the renderer draws. The MediaBox when the page sets none.
  std::array<double, 4> crop_box = {0.0, 0.0, 612.0, 792.0};
  // /Rotate as whole clockwise quarter turns: 0, 90, 180 or 270.
  int rotation = 0;

  // The displayed size: the CropBox, turned with the page.
  double DisplayWidth() const {
    return rotation % 180 == 0 ? crop_box[2] - crop_box[0]
                               : crop_box[3] - crop_box[1];
  }
  double DisplayHeight() const {
    return rotation % 180 == 0 ? crop_box[3] - crop_box[1]
                               : crop_box[2] - crop_box[0];
  }
};

// A page box: four finite numbers spanning a non-empty area, normalized so
// the lower-left corner comes first.
std::optional<std::array<double, 4>> RectangleOf(QPDFObjectHandle box) {
  if (!box.isArray() || box.getArrayNItems() != 4) return std::nullopt;
  std::array<double, 4> v{};
  for (int i = 0; i < 4; ++i) {
    QPDFObjectHandle item = box.getArrayItem(i);
    if (!item.isNumber()) return std::nullopt;
    v[static_cast<size_t>(i)] = item.getNumericValue();
    if (!std::isfinite(v[static_cast<size_t>(i)])) return std::nullopt;
  }
  const std::array<double, 4> rect = {
      std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]),
      std::max(v[1], v[3])};
  if (!(rect[0] < rect[2] && rect[1] < rect[3])) return std::nullopt;
  return rect;
}

// /Rotate read the way PDFium reads it: whole quarter turns clockwise, a
// negative value wrapping around.
int RotationOf(QPDFObjectHandle rotate) {
  long long degrees = 0;
  if (rotate.isInteger()) {
    degrees = rotate.getIntValue();
  } else if (rotate.isReal()) {
    const double value = rotate.getNumericValue();
    if (std::isfinite(value) && std::fabs(value) < 1e9) {
      degrees = static_cast<long long>(value);
    }
  }
  long long quarter_turns = (degrees / 90) % 4;
  if (quarter_turns < 0) quarter_turns += 4;
  return static_cast<int>(quarter_turns * 90);
}

PageGeometry ReadPageGeometry(QPDFObjectHandle page) {
  QPDFPageObjectHelper helper(page);
  PageGeometry geometry;
  if (auto media = RectangleOf(helper.getAttribute("/MediaBox", false))) {
    geometry.media_box = *media;
  } else {
    geometry.media_box_guessed = true;
  }
  geometry.crop_box = geometry.media_box;
  if (auto crop = RectangleOf(helper.getAttribute("/CropBox", false))) {
    const std::array<double, 4>& media = geometry.media_box;
    const std::array<double, 4> visible = {
        std::max((*crop)[0], media[0]), std::max((*crop)[1], media[1]),
        std::min((*crop)[2], media[2]), std::min((*crop)[3], media[3])};
    if (visible[0] < visible[2] && visible[1] < visible[3]) {
      geometry.crop_box = visible;
    }
  }
  geometry.rotation = RotationOf(helper.getAttribute("/Rotate", false));
  return geometry;
}

// The pages that neither store nor inherit a usable /MediaBox, found by
// walking the page tree before QPDF::getAllPages() does: qpdf writes US
// Letter into each page with no box at all, after which the page reads as
// if the document had stored it. The nearest node that has the key decides,
// as it does for ReadPageGeometry. A page that is not an indirect object
// cannot be named here; qpdf's own warning about it is still passed on.
std::set<QPDFObjGen> PagesWithoutMediaBox(QPDFObjectHandle root) {
  struct Node {
    QPDFObjectHandle node;
    bool inherits_box;
  };
  std::set<QPDFObjGen> found;
  std::set<QPDFObjGen> visited;
  std::vector<Node> pending{{root, false}};
  while (!pending.empty()) {
    Node current = pending.back();
    pending.pop_back();
    if (!current.node.isDictionary()) continue;
    if (current.node.isIndirect() &&
        !visited.insert(current.node.getObjGen()).second) {
      continue;
    }
    const bool has_box =
        current.node.hasKey("/MediaBox")
            ? RectangleOf(current.node.getKey("/MediaBox")).has_value()
            : current.inherits_box;
    QPDFObjectHandle kids = current.node.getKey("/Kids");
    if (kids.isArray()) {
      const int count = kids.getArrayNItems();
      for (int i = 0; i < count; ++i) {
        pending.push_back({kids.getArrayItem(i), has_box});
      }
    } else if (!has_box && current.node.isIndirect()) {
      found.insert(current.node.getObjGen());
    }
  }
  return found;
}

QPDFObjectHandle BoxArray(const std::array<double, 4>& box) {
  return QPDFObjectHandle::newArray(
      QPDFObjectHandle::Rectangle(box[0], box[1], box[2], box[3]));
}

// A font as the engine names it in cells and text instructions: its
// resource key together with the descriptor's /FontName, falling back to
// the descendant's descriptor, /Name and /BaseFont in the engine's order
// (page_font.h init_font_name and init_base_font).
using FontKey = std::pair<std::string, std::string>;

std::optional<std::string> NameOrString(QPDFObjectHandle object) {
  if (object.isName()) return object.getName();
  if (object.isString()) return object.getUTF8Value();
  return std::nullopt;
}

QPDFObjectHandle DictionaryKey(QPDFObjectHandle dictionary,
                               const char* key) {
  return dictionary.isDictionary() ? dictionary.getKey(key)
                                   : QPDFObjectHandle::newNull();
}

QPDFObjectHandle DescendantFont(QPDFObjectHandle font) {
  QPDFObjectHandle descendants = DictionaryKey(font, "/DescendantFonts");
  if (descendants.isArray() && descendants.getArrayNItems() > 0) {
    return descendants.getArrayItem(0);
  }
  return QPDFObjectHandle::newNull();
}

// A font's name as the document writes it, from the places the engine
// looks for one, in its order. A name is bytes, in any encoding.
std::string WrittenFontName(QPDFObjectHandle font) {
  const QPDFObjectHandle descendant = DescendantFont(font);
  for (QPDFObjectHandle candidate :
       {DictionaryKey(DictionaryKey(font, "/FontDescriptor"), "/FontName"),
        DictionaryKey(DictionaryKey(descendant, "/FontDescriptor"),
                      "/FontName"),
        DictionaryKey(font, "/Name"), DictionaryKey(font, "/BaseFont"),
        DictionaryKey(descendant, "/BaseFont")}) {
    if (auto name = NameOrString(candidate)) return *name;
  }
  return "null";
}

// The streams a font dictionary can take its program from: a /FontFile,
// /FontFile2 or /FontFile3 stream on its descriptor or its Type0
// descendant's, or on either dictionary itself, wherever the engine looks
// for one (page_font.h init_font_program).
std::vector<QPDFObjectHandle> ProgramStreamsOf(QPDFObjectHandle font) {
  std::vector<QPDFObjectHandle> programs;
  const QPDFObjectHandle descendant = DescendantFont(font);
  for (QPDFObjectHandle dictionary :
       {DictionaryKey(font, "/FontDescriptor"),
        DictionaryKey(descendant, "/FontDescriptor"), font, descendant}) {
    for (const char* key : {"/FontFile", "/FontFile2", "/FontFile3"}) {
      QPDFObjectHandle program = DictionaryKey(dictionary, key);
      if (program.isStream()) programs.push_back(program);
    }
  }
  return programs;
}

// Whether a font dictionary embeds its program. Read from the
// dictionaries alone, so it holds whether or not the call decodes
// programs.
bool EmbedsProgram(QPDFObjectHandle font) {
  return !ProgramStreamsOf(font).empty();
}

// A name as the engine passes it on: where it is not UTF-8, the engine
// puts U+FFFD for the bytes that are not (qpdf/object.h sanitize_utf8).
// Two fonts named in a legacy encoding often come out alike that way, so
// the engine's form is only the key a cell's font is found by.
std::string EngineName(const std::string& written) {
  return pdflib::qpdf_object::sanitize_utf8(written);
}

// What the document says of a font the engine names.
struct FontFacts {
  // Whether its dictionary embeds a program.
  bool embedded = false;
  // Its name as written, where the engine's form of it lost bytes.
  std::string written_name;
};

// Every resource dictionary a page can draw from: its own, and those of
// the form XObjects, tiling patterns and Type3 fonts it reaches, each
// once. A worklist rather than recursion, so a deep chain of nested forms
// cannot exhaust the stack.
std::vector<QPDFObjectHandle> ReachableResources(QPDFObjectHandle resources) {
  std::vector<QPDFObjectHandle> found;
  std::vector<QPDFObjectHandle> pending = {resources};
  std::set<QPDFObjGen> seen;
  // Direct objects cannot form a cycle; an indirect one is visited once.
  auto first_visit = [&seen](QPDFObjectHandle object) {
    return !object.isIndirect() || seen.insert(object.getObjGen()).second;
  };
  while (!pending.empty()) {
    QPDFObjectHandle current = pending.back();
    pending.pop_back();
    if (!current.isDictionary() || !first_visit(current)) continue;
    found.push_back(current);
    for (const char* category : {"/Font", "/XObject", "/Pattern"}) {
      QPDFObjectHandle entries = current.getKey(category);
      if (!entries.isDictionary()) continue;
      for (const std::string& key : entries.getKeys()) {
        QPDFObjectHandle owner = entries.getKey(key);
        if (owner.isStream()) owner = owner.getDict();
        if (!owner.isDictionary() || !first_visit(owner)) continue;
        pending.push_back(owner.getKey("/Resources"));
      }
    }
  }
  return found;
}

// The fonts a page can draw with, in every resource dictionary it
// reaches, under the key the engine gives a cell's font. Two fonts the
// engine cannot tell apart share an entry; the first one met stays.
std::map<FontKey, FontFacts> FontFactsOf(QPDFObjectHandle resources) {
  std::map<FontKey, FontFacts> facts;
  for (QPDFObjectHandle reachable : ReachableResources(resources)) {
    QPDFObjectHandle fonts = reachable.getKey("/Font");
    if (!fonts.isDictionary()) continue;
    for (const std::string& key : fonts.getKeys()) {
      QPDFObjectHandle font = fonts.getKey(key);
      const std::string written = WrittenFontName(font);
      facts.emplace(FontKey(EngineName(key), EngineName(written)),
                    FontFacts{EmbedsProgram(font), written});
    }
  }
  return facts;
}

// The pixels an image XObject declares, when it has a numeric /Width and
// /Height. The engine takes any number there and truncates it, so a real
// counts as its whole part; a side that is not finite, or under one pixel,
// counts as over every budget. Each side is capped at 2^32 so the product
// cannot overflow, and any side that large is over every budget anyway.
std::optional<uint64_t> ImagePixels(QPDFObjectHandle image) {
  if (!image.isStream()) return std::nullopt;
  QPDFObjectHandle dict = image.getDict();
  QPDFObjectHandle width = dict.getKey("/Width");
  QPDFObjectHandle height = dict.getKey("/Height");
  if (!width.isNumber() || !height.isNumber()) return std::nullopt;
  const double w = std::trunc(width.getNumericValue());
  const double h = std::trunc(height.getNumericValue());
  if (!(w >= 1.0) || !(h >= 1.0) || !std::isfinite(w) || !std::isfinite(h)) {
    return std::numeric_limits<uint64_t>::max();
  }
  constexpr double kSideCap = 4294967296.0;  // 2^32
  return static_cast<uint64_t>(std::min(w, kSideCap)) *
         static_cast<uint64_t>(std::min(h, kSideCap));
}

bool IsImage(QPDFObjectHandle xobject) {
  return xobject.isStream() &&
         xobject.getDict().getKey("/Subtype").isNameAndEquals("/Image");
}

// Text from qpdf or the engine for a proto string or a status message: cut
// to max_bytes and held to printable ASCII, since it can quote bytes of the
// document.
std::string PrintableAscii(const std::string& text, size_t max_bytes) {
  std::string out = text.substr(0, max_bytes);
  for (char& c : out) {
    if (c < 0x20 || c > 0x7e) c = '?';
  }
  if (text.size() > max_bytes) out += "...";
  return out;
}

// A resource name without its leading slash, so a name read from a qpdf
// dictionary and one read from the engine's instruction list compare.
std::string BareName(const std::string& name) {
  return !name.empty() && name.front() == '/' ? name.substr(1) : name;
}

// The images over the pixel budget among those a page reaches, by the
// objects that stand in for them. The engine defilters a page's every
// image whole, so a few kilobytes of deflated zeros declaring 12000 x 12000
// would otherwise cost the full sample buffer and the rasterizer's copy of
// it. Each such image XObject, or one whose /SMask or stencil /Mask is over
// the budget, is replaced in its resource dictionary by a one-pixel image
// before the engine decodes a sample. The replacement only keeps the
// decode cheap: a page that draws one is failed (DrawnOversizedImage),
// never rendered with it. Resource dictionaries are shared between pages,
// so the stand-ins are remembered for the life of the call's qpdf handle.
class OversizedImages {
 public:
  // Replaces the oversized images in the resource dictionaries reachable
  // from resources and returns the resource names of every stand-in found
  // there, those of earlier pages included. The edit touches only this
  // call's qpdf objects.
  std::vector<std::string> Replace(QPDF* qpdf, QPDFObjectHandle resources,
                                   uint64_t max_pixels) {
    std::vector<std::string> names;
    for (QPDFObjectHandle reachable : ReachableResources(resources)) {
      QPDFObjectHandle xobjects = reachable.getKey("/XObject");
      if (!xobjects.isDictionary()) continue;
      for (const std::string& key : xobjects.getKeys()) {
        QPDFObjectHandle image = xobjects.getKey(key);
        if (image.isIndirect() && stand_ins_.count(image.getObjGen()) > 0) {
          names.push_back(key);
          continue;
        }
        if (!IsImage(image)) continue;
        bool oversized = false;
        for (QPDFObjectHandle part : {image, image.getDict().getKey("/SMask"),
                                      image.getDict().getKey("/Mask")}) {
          const std::optional<uint64_t> pixels = ImagePixels(part);
          if (pixels.has_value() && *pixels > max_pixels) oversized = true;
        }
        if (!oversized) continue;
        QPDFObjectHandle stand_in = qpdf->newStream(std::string(1, '\xff'));
        QPDFObjectHandle dict = stand_in.getDict();
        dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
        dict.replaceKey("/Width", QPDFObjectHandle::newInteger(1));
        dict.replaceKey("/Height", QPDFObjectHandle::newInteger(1));
        dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
        dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
        xobjects.replaceKey(key, stand_in);
        stand_ins_.insert(stand_in.getObjGen());
        names.push_back(key);
      }
    }
    return names;
  }

 private:
  std::set<QPDFObjGen> stand_ins_;
};

// Finds the first oversized image a decoded page draws: an image
// instruction under one of the replaced resource names that carries the
// stand-in's single pixel, or nothing. Duck-typed against
// pdf_render_instructions::iterate_over_instructions.
class DrawnOversizedImage {
 public:
  explicit DrawnOversizedImage(const std::vector<std::string>& replaced) {
    for (const std::string& name : replaced) replaced_.insert(BareName(name));
  }

  void set_size(const pdflib::size_instruction&) {}
  void render_text(pdflib::text_instruction&) {}
  void render_widget(pdflib::text_widget_instruction&) {}
  void render_shape(pdflib::shape_instruction&) {}
  void render_shading(pdflib::shading_instruction&) {}

  void render_bitmap(pdflib::bitmap_instruction& instr) {
    if (found_.has_value() || instr.is_glyph()) return;
    const std::string name = BareName(instr.get_key());
    if (replaced_.count(name) == 0) return;
    const std::array<int, 3>& shape = instr.get_shape();
    if (instr.has_data() && (shape[0] > 1 || shape[1] > 1)) return;
    found_ = "/" + name;
  }

  // The resource name of the oversized image the page draws, if any.
  const std::optional<std::string>& found() const { return found_; }

 private:
  std::set<std::string> replaced_;
  std::optional<std::string> found_;
};

// One qpdf warning as a ParseWarning's message says it: what qpdf was
// reading, its message and where in the file, without the request's name.
// The images a decoded page draws that the engine has no samples for: the
// stream did not decode (damaged, or past the decoded stream limit), and
// the rasterizer draws nothing in their place. Duck-typed against
// pdf_render_instructions::iterate_over_instructions.
class ImagesWithoutSamples {
 public:
  void set_size(const pdflib::size_instruction&) {}
  void render_text(pdflib::text_instruction&) {}
  void render_widget(pdflib::text_widget_instruction&) {}
  void render_shape(pdflib::shape_instruction&) {}
  void render_shading(pdflib::shading_instruction&) {}

  void render_bitmap(pdflib::bitmap_instruction& instr) {
    if (instr.is_glyph() || instr.has_data()) return;
    ++count_;
    const std::string name = BareName(instr.get_key());
    if (!name.empty()) names_.insert("/" + PrintableAscii(name, 48));
  }

  // How many times the page draws such an image.
  uint64_t count() const { return count_; }
  // Their resource names; an inline image has none.
  const std::set<std::string>& names() const { return names_; }

 private:
  uint64_t count_ = 0;
  std::set<std::string> names_;
};

// How qpdf's type warning starts when a stream is asked what only a
// dictionary answers (QPDFObjectHandle::typeWarning).
constexpr char kDictionaryAskedOfStream[] =
    "operation for dictionary attempted on object of type stream";

std::string QpdfWarningText(const QPDFExc& warning) {
  std::string text = "qpdf: ";
  if (!warning.getObject().empty()) text += warning.getObject() + ": ";
  text += warning.getMessageDetail();
  if (warning.getFilePosition() > 0) {
    text += " (file offset " + std::to_string(warning.getFilePosition()) + ")";
  }
  return PrintableAscii(text, 512);
}

// Whether PDF version a ("1.4") is earlier than b ("1.7"). A string that
// is not a version compares as 0.0.
bool PdfVersionLess(const std::string& a, const std::string& b) {
  auto parts = [](const std::string& v) {
    int major = 0;
    int minor = 0;
    if (std::sscanf(v.c_str(), "%d.%d", &major, &minor) < 1) major = 0;
    return std::pair<int, int>(major, minor);
  };
  return parts(a) < parts(b);
}

// Whether a page decode keeps the annotation appearances the engine draws
// after the content stream. Render drops them when the request sets
// omit_annotations; Parse always keeps them.
enum class AnnotationAppearances { kDraw, kOmit };

// The document's pages for Parse and Render, on a qpdf handle of the
// call's own beside the engine's document decoder. The inventory comes
// from the page dictionaries without decoding any content; a page's
// content is decoded only when the call reaches it.
class DocumentPages {
 public:
  // Opens the bytes, which must outlive this object: qpdf reads them in
  // place. Returns LOAD_STATUS_OK, or the verdict qpdf's error code gives
  // with its message in error: a password qpdf refuses is
  // PASSWORD_REQUIRED, or PASSWORD_INCORRECT when the call supplied one,
  // damage qpdf cannot recover from is CORRUPT, encryption qpdf does not
  // implement is UNSUPPORTED_ENCRYPTION, and anything else is ENGINE_ERROR.
  // qpdf repairs what it can and says so in warnings, kept in
  // load_warnings(); a document it had to repair, or whose page tree
  // declares pages, and in which it then found no page is CORRUPT too, not
  // an empty document.
  pdfv1::LoadStatus Open(const std::string& bytes,
                         const std::optional<std::string>& password,
                         std::string* error) {
    try {
      qpdf_.setSuppressWarnings(true);
      qpdf_.processMemoryFile("grpc-qparse request", bytes.data(), bytes.size(),
                              password.has_value() ? password->c_str() : nullptr);
      // Read before the pages: qpdf rewrites /Count when it has to flatten
      // a damaged page tree.
      QPDFObjectHandle count =
          DictionaryKey(DictionaryKey(qpdf_.getRoot(), "/Pages"), "/Count");
      if (count.isInteger()) declared_pages_ = count.getIntValue();
      const std::set<QPDFObjGen> boxless =
          PagesWithoutMediaBox(DictionaryKey(qpdf_.getRoot(), "/Pages"));
      pages_ = qpdf_.getAllPages();
      geometry_.reserve(pages_.size());
      for (const auto& page : pages_) {
        geometry_.push_back(ReadPageGeometry(page));
        if (boxless.count(page.getObjGen()) > 0) {
          geometry_.back().media_box_guessed = true;
        }
        const std::array<double, 4>& crop = geometry_.back().crop_box;
        targets_.index_of.emplace(
            page.getObjGen(),
            static_cast<uint32_t>(targets_.crop_origin.size()));
        targets_.crop_origin.push_back({crop[0], crop[1]});
      }
      load_warnings_ = qpdf_.getWarnings();
      if (pages_.empty() &&
          (!load_warnings_.empty() || declared_pages_.value_or(0) > 0)) {
        *error = "no page could be read";
        if (declared_pages_.value_or(0) > 0) {
          *error += "; the page tree declares " +
                    std::to_string(*declared_pages_);
        }
        if (!load_warnings_.empty()) {
          *error += "; " + QpdfWarningText(load_warnings_.front());
        }
        return pdfv1::LOAD_STATUS_CORRUPT;
      }
    } catch (const QPDFExc& e) {
      *error = e.what();
      switch (e.getErrorCode()) {
        case qpdf_e_password:
          return password.has_value() ? pdfv1::LOAD_STATUS_PASSWORD_INCORRECT
                                      : pdfv1::LOAD_STATUS_PASSWORD_REQUIRED;
        case qpdf_e_damaged_pdf:
          return pdfv1::LOAD_STATUS_CORRUPT;
        case qpdf_e_unsupported:
          // qpdf found the trailer's /Encrypt and does not implement what
          // it asks for: a security handler other than the standard one,
          // or a /V or /R it does not know. Anything else it calls
          // unsupported is not about encryption.
          return qpdf_.isEncrypted() ? pdfv1::LOAD_STATUS_UNSUPPORTED_ENCRYPTION
                                     : pdfv1::LOAD_STATUS_ENGINE_ERROR;
        default:
          return pdfv1::LOAD_STATUS_ENGINE_ERROR;
      }
    } catch (const std::exception& e) {
      *error = e.what();
      return pdfv1::LOAD_STATUS_ENGINE_ERROR;
    }
    return pdfv1::LOAD_STATUS_OK;
  }

  int page_count() const { return static_cast<int>(pages_.size()); }

  const PageGeometry& geometry(int index) const {
    return geometry_.at(static_cast<size_t>(index));
  }

  // The fonts the page's content can draw with. Call it after Decode,
  // which pins inherited resources on the page.
  std::map<FontKey, FontFacts> Fonts(int index) const {
    QPDFObjectHandle page = pages_.at(static_cast<size_t>(index));
    return FontFactsOf(
        QPDFPageObjectHelper(page).getAttribute("/Resources", false));
  }

  // What qpdf said while it opened the document and read its page tree.
  const std::vector<QPDFExc>& load_warnings() const { return load_warnings_; }

  // The page tree's /Count when it is not the number of pages qpdf found:
  // qpdf leaves out a kid it cannot read without a warning.
  std::optional<long long> MismatchedPageCount() const {
    if (declared_pages_.has_value() &&
        *declared_pages_ != static_cast<long long>(pages_.size())) {
      return declared_pages_;
    }
    return std::nullopt;
  }

  // What qpdf said since the last call, or since the document opened:
  // after a page's decode, its warnings about that page.
  std::vector<QPDFExc> TakeWarnings() {
    HoldWarnings();
    return std::exchange(held_warnings_, {});
  }

  // What qpdf said while the engine read a page, less what it said about
  // the engine's own question. The engine asks each font program's stream
  // for its /Length1 to /Length3 as if the stream were a dictionary
  // (page_font.h), on every document with an embedded font, and qpdf
  // answers each time with a type warning that says nothing about the
  // document. Only that is left out, by the streams it names: a stream
  // anywhere else the engine expects a dictionary is the document's fault
  // and stays.
  std::vector<QPDFExc> TakePageWarnings(int index) {
    std::vector<QPDFExc> said = TakeWarnings();
    const auto asked_a_stream = [](const QPDFExc& warning) {
      return warning.getErrorCode() == qpdf_e_object &&
             warning.getMessageDetail().rfind(kDictionaryAskedOfStream, 0) == 0;
    };
    if (std::none_of(said.begin(), said.end(), asked_a_stream)) return said;
    std::set<std::string> programs;
    try {
      programs = FontProgramObjects(index);
    } catch (const std::exception&) {
      // The walk met an object qpdf cannot read; nothing is left out.
      return said;
    }
    said.erase(std::remove_if(said.begin(), said.end(),
                              [&](const QPDFExc& warning) {
                                return asked_a_stream(warning) &&
                                       programs.count(warning.getObject()) > 0;
                              }),
               said.end());
    return said;
  }

  // The outline, a page's links and a page's form fields, read from the
  // document itself (qpdf_readers.h).
  bool HasOutline() { return grpc_qparse::HasOutline(qpdf_); }
  OutlineRead ReadOutline(pdfv1::OutlineChunk* out) {
    return grpc_qparse::ReadOutline(qpdf_, targets_, out);
  }
  LinksRead ReadLinks(int index, pdfv1::PageChunk* chunk) {
    const size_t i = static_cast<size_t>(index);
    return grpc_qparse::ReadLinks(qpdf_, pages_.at(i), targets_,
                                  targets_.crop_origin.at(i), chunk);
  }
  FieldsRead ReadFormFields(int index, pdfv1::PageChunk* chunk) {
    const size_t i = static_cast<size_t>(index);
    return grpc_qparse::ReadFormFields(pages_.at(i), targets_.crop_origin.at(i),
                                       chunk);
  }

  // Document metadata from the trailer's info dictionary, the header and
  // the catalog. Text values come as UTF-8 whatever their encoding in the
  // file; dates and /Trapped are carried as stored. An info value that is
  // neither text, a name, a number nor a boolean has no place in the
  // contract, and its key is returned in not_carried.
  void FillDocMeta(pdfv1::DocMeta* meta, std::vector<std::string>* not_carried) {
    meta->set_pdf_version(qpdf_.getPDFVersion());
    QPDFObjectHandle root = qpdf_.getRoot();
    // A catalog /Version later than the header's replaces it (ISO 32000-1,
    // 7.2.2).
    QPDFObjectHandle catalog_version = DictionaryKey(root, "/Version");
    if (catalog_version.isName()) {
      const std::string version = BareName(catalog_version.getName());
      if (PdfVersionLess(meta->pdf_version(), version)) {
        meta->set_pdf_version(version);
      }
    }
    meta->set_linearized(qpdf_.isLinearized());
    QPDFObjectHandle marked =
        DictionaryKey(DictionaryKey(root, "/MarkInfo"), "/Marked");
    meta->set_tagged(marked.isBool() && marked.getBoolValue());

    QPDFObjectHandle info = qpdf_.getTrailer().getKey("/Info");
    if (!info.isDictionary()) return;
    for (const std::string& key : info.getKeys()) {
      QPDFObjectHandle stored = info.getKey(key);
      std::string value;
      if (stored.isString()) {
        value = stored.getUTF8Value();
      } else if (stored.isName()) {
        value = BareName(stored.getName());
      } else if (stored.isInteger() || stored.isReal() || stored.isBool()) {
        value = stored.unparse();
      } else {
        if (!stored.isNull()) not_carried->push_back(key);
        continue;
      }
      if (key == "/Title") {
        if (!value.empty()) meta->set_title(value);
      } else if (key == "/Author") {
        if (!value.empty()) meta->set_author(value);
      } else if (key == "/Subject") {
        if (!value.empty()) meta->set_subject(value);
      } else if (key == "/Keywords") {
        if (!value.empty()) meta->set_keywords(value);
      } else if (key == "/Creator") {
        if (!value.empty()) meta->set_creator(value);
      } else if (key == "/Producer") {
        if (!value.empty()) meta->set_producer(value);
      } else if (key == "/CreationDate") {
        if (!value.empty()) meta->set_created_raw(value);
      } else if (key == "/ModDate") {
        if (!value.empty()) meta->set_modified_raw(value);
      } else if (key == "/Trapped") {
        if (!value.empty()) meta->set_trapped(value);
      } else {
        auto* entry = meta->add_custom();
        entry->set_key(BareName(key));
        entry->set_value(value);
      }
    }
  }

  // Why the page's content cannot be read, when one of its content streams
  // does not decode: a damaged or mislabelled filter, or a stream over the
  // decoded stream limit. The engine reads a page's content streams as one
  // and, when any of them fails, logs it and goes on with an empty page;
  // this asks qpdf first, the same way the engine will, so the caller can
  // say so. Costs one more decode of the page's content streams. What qpdf
  // says meanwhile stays for TakeWarnings: it says some things only the
  // first time it reads an object.
  std::optional<std::string> UndecodableContent(int index) {
    QPDFObjectHandle page = pages_.at(static_cast<size_t>(index));
    std::optional<std::string> failure;
    try {
      for (QPDFObjectHandle stream :
           QPDFPageObjectHelper(page).getPageContents()) {
        if (!stream.isStream()) continue;
        HoldWarnings();
        const size_t said_before = held_warnings_.size();
        Pl_Discard discard;
        std::string reason;
        bool decoded = false;
        try {
          decoded = stream.pipeStreamData(&discard, 0, qpdf_dl_specialized);
        } catch (const std::exception& e) {
          reason = e.what();
        }
        if (decoded) continue;
        HoldWarnings();
        for (size_t i = said_before; i < held_warnings_.size(); ++i) {
          if (!reason.empty()) reason += "; ";
          reason += held_warnings_[i].getMessageDetail();
        }
        if (reason.empty()) reason = "qpdf cannot undo its filters";
        failure = "object " + stream.getObjGen().unparse(' ') + ": " + reason;
        break;
      }
    } catch (const std::exception& e) {
      failure = e.what();
    }
    if (failure.has_value()) *failure = PrintableAscii(*failure, 256);
    return failure;
  }

  // Decodes one page. The engine reads /CropBox and /Rotate from the page
  // dictionary alone, never through the page tree, and on a rotated page it
  // turns every cell, shape and image into display orientation and drops
  // the angle. Here it decodes the page with the boxes resolved above and
  // /Rotate 0, so every item stays unrotated, as the contract's frame wants;
  // Render passes the rotation to the rasterizer instead. The edits
  // touch only this call's qpdf objects, never the bytes.
  //
  // When the call decodes image samples, max_image_pixels bounds every image
  // the page reaches (OversizedImages): an image over it is not decoded,
  // and its resource name is added to oversized_images, for the caller to
  // fail the page if it draws one (DrawnOversizedImage).
  // With AnnotationAppearances::kOmit the page's annotation array is taken
  // off before the decode, so no annotation appearance (markup, widget or
  // otherwise) reaches the instruction list.
  std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> Decode(
      int index, const pdflib::decode_config& config,
      std::optional<uint64_t> max_image_pixels = std::nullopt,
      AnnotationAppearances annotations = AnnotationAppearances::kDraw,
      std::vector<std::string>* oversized_images = nullptr) {
    QPDFObjectHandle page = pages_.at(static_cast<size_t>(index));
    const PageGeometry& page_geometry = geometry(index);
    page.replaceKey("/MediaBox", BoxArray(page_geometry.media_box));
    page.replaceKey("/CropBox", BoxArray(page_geometry.crop_box));
    page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(0));
    if (annotations == AnnotationAppearances::kOmit) {
      // The engine reads both the standard /Annots and the misspelled
      // /Annot some producers write.
      page.removeKey("/Annots");
      page.removeKey("/Annot");
    }
    // The engine also looks for /Resources on the page and its direct
    // parent only; resources inherited from further up are pinned on the
    // page.
    if (!page.hasKey("/Resources")) {
      QPDFObjectHandle parent = page.getKey("/Parent");
      if (!(parent.isDictionary() && parent.hasKey("/Resources"))) {
        QPDFObjectHandle inherited =
            QPDFPageObjectHelper(page).getAttribute("/Resources", false);
        if (inherited.isDictionary()) page.replaceKey("/Resources", inherited);
      }
    }
    if (config.extract_bitmap_pixels && max_image_pixels.has_value()) {
      std::vector<std::string> replaced = oversized_.Replace(
          &qpdf_, QPDFPageObjectHelper(page).getAttribute("/Resources", false),
          *max_image_pixels);
      if (oversized_images != nullptr) *oversized_images = std::move(replaced);
    }
    auto decoder =
        std::make_shared<pdflib::pdf_decoder<pdflib::PAGE>>(page, index);
    decoder->decode_page(config);
    return decoder;
  }

 private:
  // How a qpdf warning names each font program stream the engine can load
  // for a page: those of the fonts the page's content reaches, of its
  // annotations' normal appearances, and of the form's default resources.
  std::set<std::string> FontProgramObjects(int index) {
    QPDFObjectHandle page = pages_.at(static_cast<size_t>(index));
    std::vector<QPDFObjectHandle> roots = {
        QPDFPageObjectHelper(page).getAttribute("/Resources", false),
        DictionaryKey(DictionaryKey(qpdf_.getRoot(), "/AcroForm"), "/DR")};
    auto add_appearance = [&roots](QPDFObjectHandle appearance) {
      if (appearance.isStream()) {
        roots.push_back(appearance.getDict().getKey("/Resources"));
      }
    };
    QPDFObjectHandle annotations = DictionaryKey(page, "/Annots");
    if (annotations.isArray()) {
      for (QPDFObjectHandle annotation : annotations.aitems()) {
        QPDFObjectHandle normal =
            DictionaryKey(DictionaryKey(annotation, "/AP"), "/N");
        add_appearance(normal);
        if (!normal.isDictionary()) continue;
        for (const std::string& state : normal.getKeys()) {
          add_appearance(normal.getKey(state));
        }
      }
    }
    std::set<std::string> named;
    for (QPDFObjectHandle root : roots) {
      for (QPDFObjectHandle reachable : ReachableResources(root)) {
        QPDFObjectHandle fonts = reachable.getKey("/Font");
        if (!fonts.isDictionary()) continue;
        for (const std::string& key : fonts.getKeys()) {
          for (QPDFObjectHandle program : ProgramStreamsOf(fonts.getKey(key))) {
            named.insert(qpdf_.getFilename() + ", stream object " +
                         program.getObjGen().unparse(' '));
          }
        }
      }
    }
    return named;
  }

  // Moves qpdf's warnings so far into held_warnings_; qpdf hands them out
  // once.
  void HoldWarnings() {
    for (QPDFExc& warning : qpdf_.getWarnings()) {
      held_warnings_.push_back(std::move(warning));
    }
  }

  QPDF qpdf_;
  std::vector<QPDFObjectHandle> pages_;
  std::vector<PageGeometry> geometry_;
  // Where destinations lead: each page's object and frame origin.
  PageTargets targets_;
  std::vector<QPDFExc> load_warnings_;
  // Warnings taken from qpdf and not yet given to the caller.
  std::vector<QPDFExc> held_warnings_;
  std::optional<long long> declared_pages_;
  OversizedImages oversized_;
};

struct LoadedDocument {
  pdflib::pdf_timings timings;
  std::unique_ptr<pdflib::pdf_decoder<pdflib::DOCUMENT>> doc;
  // The resolved document bytes; kept alive for the engine's decode.
  std::shared_ptr<std::string> buffer;
  // Per-page access for Parse and Render. It reads buffer in place, so it
  // is declared after it and destroyed first.
  std::unique_ptr<DocumentPages> pages;
  pdfv1::LoadStatus status = pdfv1::LOAD_STATUS_UNSPECIFIED;
  std::string detail;
};

// How ResolveDocumentBytes answered, so the RPC can pick its failure
// surface: a typed load verdict rides in the response, while a contract
// violation is a gRPC INVALID_ARGUMENT.
enum class ResolveOutcome { kOk, kVerdict, kInvalidArgument };

// The one content-addressing resolution path shared by Probe, Parse and
// Render. With data present and a sha256 given, the bytes are hashed and
// verified (a mismatch is LOAD_STATUS_HASH_MISMATCH) and cached under the
// digest; data without a sha256 is used without touching the cache. With
// data empty the request is a cache lookup by sha256 (a miss is
// LOAD_STATUS_BYTES_REQUIRED). data empty with sha256 absent is invalid.
ResolveOutcome ResolveDocumentBytes(const pdfv1::PdfDocument& request,
                                    DocumentCache* cache,
                                    LoadedDocument* out) {
  const std::string& data = request.data();
  if (!data.empty()) {
    out->buffer = std::make_shared<std::string>(data);
    if (!request.has_sha256()) return ResolveOutcome::kOk;
    const std::string digest = Sha256Hex(data);
    if (request.sha256() != digest) {
      out->status = pdfv1::LOAD_STATUS_HASH_MISMATCH;
      out->detail = "data does not hash to the given sha256";
      return ResolveOutcome::kVerdict;
    }
    cache->Insert(digest, out->buffer);
    return ResolveOutcome::kOk;
  }
  if (!request.has_sha256()) {
    out->detail = "document data is empty and no sha256 names a cached "
                  "document";
    return ResolveOutcome::kInvalidArgument;
  }
  std::shared_ptr<std::string> cached = cache->Lookup(request.sha256());
  if (cached == nullptr) {
    out->status = pdfv1::LOAD_STATUS_BYTES_REQUIRED;
    out->detail = "no cached bytes for sha256 " + request.sha256();
    return ResolveOutcome::kVerdict;
  }
  out->buffer = std::move(cached);
  return ResolveOutcome::kOk;
}

// The largest ParseStream upload accepted: the document cache's default
// byte ceiling. A stream is not held to the 520 MiB message limit, which
// bounds each chunk instead.
constexpr uint64_t kMaxStreamDocumentBytes = 2ull * 1024 * 1024 * 1024;

// How many of one ParseStream's requests run at once; each also holds a
// call slot (call_slots.h) while it decodes, so the process-wide bound
// still holds.
constexpr unsigned kStreamParallelRequests = 3;

// A ParseStream request's bytes are the stream's upload, complete and
// checked against the open's sha256 by the session already; any other
// request resolves through the handshake.
ResolveOutcome ResolveRequestBytes(const pdfv1::PdfDocument& request,
                                   const std::shared_ptr<std::string>& stream_bytes,
                                   DocumentCache* cache, LoadedDocument* out) {
  if (stream_bytes != nullptr) {
    out->buffer = stream_bytes;
    return ResolveOutcome::kOk;
  }
  return ResolveDocumentBytes(request, cache, out);
}

// Opens the resolved bytes in the engine and in the per-page handle Parse
// and Render work through. Probe opens both too, so its verdict is the one
// Parse and Render would give.
void LoadDocument(const pdfv1::PdfDocument& request, LoadedDocument* out) {
  const std::string& data = *out->buffer;
  if (data.rfind("%PDF-", 0) != 0) {
    out->status = pdfv1::LOAD_STATUS_NOT_PDF;
    out->detail = "missing %PDF- header";
    return;
  }
  out->doc =
      std::make_unique<pdflib::pdf_decoder<pdflib::DOCUMENT>>(out->timings);
  std::optional<std::string> password;
  if (request.has_password()) password = request.password();
  bool ok = false;
  try {
    ok = out->doc->process_document_from_bytesio(out->buffer, password,
                                                 "grpc-qparse request");
  } catch (const std::exception& e) {
    out->detail = e.what();
  }
  // The engine reports an open failure as a plain false, so the call's own
  // qpdf handle, which Parse and Render need anyway, opens the bytes too:
  // its error code tells a password or damaged file apart from an engine
  // failure, and it keeps what qpdf had to repair.
  auto pages = std::make_unique<DocumentPages>();
  std::string pages_detail;
  const pdfv1::LoadStatus pages_status =
      pages->Open(data, password, &pages_detail);
  if (ok && pages_status == pdfv1::LOAD_STATUS_OK) {
    out->pages = std::move(pages);
    out->status = pdfv1::LOAD_STATUS_OK;
    return;
  }
  out->doc.reset();
  if (pages_status != pdfv1::LOAD_STATUS_OK) {
    out->status = pages_status;
    out->detail = pages_detail;
    return;
  }
  out->status = pdfv1::LOAD_STATUS_ENGINE_ERROR;
  if (out->detail.empty()) out->detail = "engine could not open the document";
}

void FillCapabilities(const LoadedDocument& loaded,
                      pdfv1::BackendCapabilities* caps) {
  caps->set_backend_name(kBackendName);
  caps->set_engine_version(kEngineVersion);
  caps->set_load_status(loaded.status);
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    if (!loaded.detail.empty()) caps->set_load_detail(loaded.detail);
    return;
  }
  caps->set_page_count(
      static_cast<uint32_t>(loaded.doc->get_number_of_pages()));
  const bool has_outline = loaded.pages->HasOutline();
  for (int f = pdfv1::PdfFamily_MIN + 1; f <= pdfv1::PdfFamily_MAX; ++f) {
    if (!pdfv1::PdfFamily_IsValid(f)) continue;
    auto family = static_cast<pdfv1::PdfFamily>(f);
    auto* verdict = caps->add_families();
    verdict->set_family(family);
    if (!FamilySupported(family)) {
      verdict->set_support(pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND);
      verdict->set_detail(UnsupportedDetail(family));
    } else if (family == pdfv1::PDF_FAMILY_OUTLINE && !has_outline) {
      verdict->set_support(pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT);
    } else {
      verdict->set_support(pdfv1::FAMILY_SUPPORT_SUPPORTED);
      if (family == pdfv1::PDF_FAMILY_PLACED_IMAGES) {
        verdict->set_detail(kPlacedImagesDetail);
      }
    }
  }
}

bool WantFamily(const pdfv1::ParseRequest& request, pdfv1::PdfFamily family) {
  if (request.families().empty()) return true;
  return std::find(request.families().begin(), request.families().end(),
                   family) != request.families().end();
}

// Sets an axis-aligned box from two corners in either order; annotation
// rectangles may be stored with any pair of opposite corners.
void SetBox(double x0, double y0, double x1, double y1,
            pdfv1::BoundingBox* box) {
  box->set_x0(std::min(x0, x1));
  box->set_y0(std::min(y0, y1));
  box->set_x1(std::max(x0, x1));
  box->set_y1(std::max(y0, y1));
}

void SetBox(const std::array<double, 4>& corners, pdfv1::BoundingBox* box) {
  SetBox(corners[0], corners[1], corners[2], corners[3], box);
}

// PageInfo straight from the page dictionary: the size after /Rotate, the
// true (inherited) /Rotate, and the boxes as stored, in unrotated user
// space. Every other geometry on the page is relative to the CropBox.
void FillPageInfo(const PageGeometry& geometry, int index,
                  pdfv1::PageInfo* info) {
  info->set_page_index(static_cast<uint32_t>(index));
  info->set_width_pts(geometry.DisplayWidth());
  info->set_height_pts(geometry.DisplayHeight());
  info->set_rotation_degrees(geometry.rotation);
  SetBox(geometry.media_box, info->mutable_media_box());
  SetBox(geometry.crop_box, info->mutable_crop_box());
  info->set_page_space(pdfv1::PAGE_SPACE_CROP_BOX);
}

using FontProgram = std::shared_ptr<const pdflib::embedded_font_blob>;

// Assigns stable font ids within one stream. A font is the name the engine
// gives cells and text instructions alike (the descriptor's /FontName,
// else /BaseFont), together with the content key of its embedded program
// when the call decoded programs: two subsets that share a name but not a
// program stay apart, and one font used on many pages keeps one id.
class FontInterner {
 public:
  // Returns the font's id, announcing a font seen for the first time in
  // new_fonts, marked embedded when its dictionary embeds a program whether
  // or not the call decoded it.
  uint32_t Intern(const std::string& written_name, const FontProgram& program,
                  bool embedded, pdfv1::FontTableChunk* new_fonts) {
    std::string name = written_name;
    if (!name.empty() && name.front() == '/') name.erase(0, 1);
    std::pair<std::string, std::string> key(
        name, program != nullptr ? program->get_cache_key() : std::string());
    auto it = ids_.find(key);
    if (it != ids_.end()) return it->second;
    const auto id = static_cast<uint32_t>(ids_.size());
    ids_.emplace(std::move(key), id);
    auto* ref = new_fonts->add_fonts();
    ref->set_font_id(id);
    ref->set_base_name(name);
    ref->set_embedded(embedded || program != nullptr);
    return id;
  }

 private:
  std::map<std::pair<std::string, std::string>, uint32_t> ids_;
};

// The fonts one page draws text with, keyed the way the engine keys a
// cell's font (its resource name and font name), each with its embedded
// program when the call decoded programs. Duck-typed against
// pdf_render_instructions::iterate_over_instructions.
class PageFonts {
 public:
  using Key = FontKey;

  explicit PageFonts(std::map<Key, FontFacts> facts)
      : facts_(std::move(facts)) {}

  void set_size(const pdflib::size_instruction&) {}
  void render_widget(pdflib::text_widget_instruction&) {}
  void render_bitmap(pdflib::bitmap_instruction&) {}
  void render_shape(pdflib::shape_instruction&) {}
  void render_shading(pdflib::shading_instruction&) {}

  void render_text(pdflib::text_instruction& instr) {
    fonts_.emplace(Key(instr.get_font_key(), instr.get_font_name()),
                   instr.has_embedded_font() ? instr.get_embedded_font()
                                             : nullptr);
  }

  // The program a cell's font draws with; null when there is none or
  // programs were not decoded.
  FontProgram ProgramOf(const std::string& font_key,
                        const std::string& font_name) const {
    auto it = fonts_.find(Key(font_key, font_name));
    return it != fonts_.end() ? it->second : nullptr;
  }

  // Whether the font's dictionary embeds a program, decoded or not.
  bool Embedded(const std::string& font_key,
                const std::string& font_name) const {
    const auto it = facts_.find(Key(font_key, font_name));
    return it != facts_.end() && it->second.embedded;
  }

  // The font's name as the document writes it. A font the page's own
  // resources do not reach (one an annotation's appearance brings) keeps
  // the engine's form of its name.
  const std::string& WrittenName(const std::string& font_key,
                                 const std::string& font_name) const {
    const auto it = facts_.find(Key(font_key, font_name));
    return it != facts_.end() ? it->second.written_name : font_name;
  }

  const std::map<Key, FontProgram>& fonts() const { return fonts_; }

 private:
  std::map<Key, FontFacts> facts_;
  std::map<Key, FontProgram> fonts_;
};

pdfv1::FontProgramFormat ProgramFormat(pdflib::embedded_font_format format) {
  switch (format) {
    case pdflib::embedded_font_format::TYPE1:
      return pdfv1::FONT_PROGRAM_FORMAT_TYPE1;
    case pdflib::embedded_font_format::TRUETYPE:
      return pdfv1::FONT_PROGRAM_FORMAT_TRUETYPE;
    case pdflib::embedded_font_format::TYPE1C:
    case pdflib::embedded_font_format::CID_TYPE0C:
      return pdfv1::FONT_PROGRAM_FORMAT_CFF;
    case pdflib::embedded_font_format::OPENTYPE:
      return pdfv1::FONT_PROGRAM_FORMAT_OPENTYPE;
    default:
      return pdfv1::FONT_PROGRAM_FORMAT_UNSPECIFIED;
  }
}

// The embedded programs of the fonts a page draws with, under the ids
// their cells carry, each sent once per stream.
void CollectPrograms(const PageFonts& page_fonts, FontInterner* fonts,
                     pdfv1::FontTableChunk* new_fonts,
                     std::set<uint32_t>* sent,
                     std::vector<pdfv1::EmbeddedFont>* out) {
  for (const auto& [key, program] : page_fonts.fonts()) {
    if (program == nullptr || !program->has_bytes()) continue;
    const uint32_t id =
        fonts->Intern(page_fonts.WrittenName(key.first, key.second), program,
                      /*embedded=*/true, new_fonts);
    if (!sent->insert(id).second) continue;
    pdfv1::EmbeddedFont font;
    font.set_font_id(id);
    font.set_format(ProgramFormat(program->get_format()));
    const auto& bytes = program->get_bytes();
    font.set_program(std::string(bytes->begin(), bytes->end()));
    out->push_back(std::move(font));
  }
}

// Parse reports geometry and text, never image samples, so image XObjects
// are measured but not decoded, and it decodes font programs only for a
// call that asks for them. The page boundary is the MediaBox: the engine
// drops every cell that is not wholly inside its boundary, and a cell that
// only straddles the CropBox edge is still on the page.
pdflib::decode_config ParseDecodeConfig(bool font_programs) {
  pdflib::decode_config config;
  config.page_boundary = "media_box";
  config.extract_bitmap_pixels = false;
  config.extract_font_programs = font_programs;
  return config;
}

pdflib::decode_config RenderDecodeConfig(float scale) {
  pdflib::decode_config config;
  config.extract_font_programs = true;
  config.extract_bitmap_pixels = true;
  config.bitmap_target_pixels_per_unit = scale;
  return config;
}

// The canvas the rasterizer draws: the visible CropBox, turned by the
// page's /Rotate once drawn. The page itself was decoded unrotated.
pdflib::size_instruction SizeInstruction(const PageGeometry& geometry) {
  pdflib::size_instruction size;
  size.media_bbox = geometry.media_box;
  size.crop_bbox = geometry.crop_box;
  size.angle = geometry.rotation;
  return size;
}

// Blend2D refuses an image wider or taller than this.
constexpr double kMaxRasterSide = 65535.0;

// The rasterizer keeps each worker thread's last canvas for its next page
// (blend2d_renderer.h, canvas_pool). A canvas over this many pixels is
// released after use, so one large page does not stay resident on a gRPC
// thread for the life of the process.
constexpr double kPooledCanvasPixels = 4096.0 * 4096.0;

// The raster the rasterizer allocates for a page at a scale: the displayed
// page rounded up to whole pixels as render/config.h pixels_for_extent
// rounds, at least one pixel a side.
std::array<double, 2> RasterSize(const PageGeometry& geometry, float scale) {
  return {std::max(1.0, std::ceil(geometry.DisplayWidth() * scale - 1e-6)),
          std::max(1.0, std::ceil(geometry.DisplayHeight() * scale - 1e-6))};
}

// Writes the rasterizer's RGBA canvas (rows top-down, width * height * 4
// bytes) into the raster in the layout the caller asked for; RGBA8 when it
// named none. The canvas starts opaque white (blend2d_renderer.h set_size)
// and is only ever painted over, so dropping alpha loses nothing.
void FillPixels(const std::vector<uint8_t>& rgba, uint32_t width,
                uint32_t height, pdfv1::PixelFormat requested,
                pdfv1::PageRaster* raster) {
  pdfv1::PixelFormat format = requested;
  uint32_t channels = 4;
  switch (requested) {
    case pdfv1::PIXEL_FORMAT_RGB8:
    case pdfv1::PIXEL_FORMAT_BGR8:
      channels = 3;
      break;
    case pdfv1::PIXEL_FORMAT_GRAY8:
      channels = 1;
      break;
    case pdfv1::PIXEL_FORMAT_BGRA8:
      break;
    default:
      format = pdfv1::PIXEL_FORMAT_RGBA8;
      break;
  }
  raster->set_width_px(width);
  raster->set_height_px(height);
  raster->set_stride_bytes(width * channels);
  raster->set_pixel_format(format);
  if (format == pdfv1::PIXEL_FORMAT_RGBA8) {
    raster->set_pixels(rgba.data(), rgba.size());
    return;
  }
  const size_t pixels = static_cast<size_t>(width) * height;
  std::string* out = raster->mutable_pixels();
  out->resize(pixels * channels);
  const uint8_t* src = rgba.data();
  auto* dst = reinterpret_cast<uint8_t*>(out->data());
  switch (format) {
    case pdfv1::PIXEL_FORMAT_RGB8:
      for (size_t i = 0; i < pixels; ++i, src += 4, dst += 3) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
      }
      break;
    case pdfv1::PIXEL_FORMAT_BGR8:
      for (size_t i = 0; i < pixels; ++i, src += 4, dst += 3) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
      }
      break;
    case pdfv1::PIXEL_FORMAT_BGRA8:
      for (size_t i = 0; i < pixels; ++i, src += 4, dst += 4) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
      }
      break;
    default:  // GRAY8: Rec. 601 luma.
      for (size_t i = 0; i < pixels; ++i, src += 4, ++dst) {
        *dst = static_cast<uint8_t>(
            (299u * src[0] + 587u * src[1] + 114u * src[2] + 500u) / 1000u);
      }
      break;
  }
}

// Leaves a one-pixel canvas in this thread's pool in place of the last
// one: a renderer hands its canvas to the pool when it is destroyed.
void ReleasePooledCanvas() {
  pdflib::render_config config;
  config.scale = 1.0f;
  config.glyph_bbox_cache_capacity = 0;
  pdflib::renderer<pdflib::BLEND2D> release(config);
  pdflib::size_instruction size;
  size.media_bbox = {0.0, 0.0, 1.0, 1.0};
  size.crop_bbox = size.media_bbox;
  release.set_size(size);
}

// The request's background components, each a finite number; NaN or an
// infinity cannot be laid down as a color.
grpc::Status CheckBackground(const pdfv1::RenderRequest& request) {
  if (!request.has_background()) return grpc::Status::OK;
  const pdfv1::Color& c = request.background();
  for (double v : {c.red(), c.green(), c.blue(), c.alpha()}) {
    if (!std::isfinite(v)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "background components must be finite numbers");
    }
  }
  return grpc::Status::OK;
}

// The request's background as a fill the rasterizer paints before any page
// item: the page's CropBox, a point wider on every side so no edge pixel is
// left partly covered. Components are clamped to [0, 1]. The canvas starts
// opaque white (blend2d_renderer.h set_size) and the raster carries no
// transparency, so a background with alpha below 1 lands on that white,
// the way a translucent page shows on a white sheet. nullopt when the
// request sets none, which keeps the plain white canvas.
std::optional<pdflib::shape_instruction> BackgroundFill(
    const pdfv1::RenderRequest& request, const PageGeometry& geometry) {
  if (!request.has_background()) return std::nullopt;
  const pdfv1::Color& c = request.background();
  auto to_byte = [](double v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
  };
  const std::array<int, 3> rgb = {to_byte(c.red()), to_byte(c.green()),
                                  to_byte(c.blue())};
  const double x0 = geometry.crop_box[0] - 1.0;
  const double y0 = geometry.crop_box[1] - 1.0;
  const double x1 = geometry.crop_box[2] + 1.0;
  const double y1 = geometry.crop_box[3] + 1.0;
  std::vector<pdflib::shape_subpath> rect;
  rect.emplace_back(
      x0, y0,
      std::vector<pdflib::shape_segment_op>(3, pdflib::SEGMENT_LINE_TO),
      std::vector<double>{x1, x1, x0}, std::vector<double>{y0, y1, y1},
      pdflib::CLOSED, pdflib::RECTANGLE);
  return pdflib::shape_instruction(
      std::move(rect), pdflib::SHAPE_PAINT_FILL, pdflib::SHAPE_FILL_NONZERO,
      /*line_width=*/0.0, /*line_cap=*/0, /*line_join=*/0,
      /*miter_limit=*/10.0, /*dash_array=*/{}, /*dash_phase=*/0.0, rgb, rgb,
      /*stroke_alpha=*/1.0, std::clamp(c.alpha(), 0.0, 1.0));
}

// Hands the page's instructions to the rasterizer, painting the background
// fill, when there is one, right after the canvas is sized and so beneath
// every page item. Duck-typed against
// pdf_render_instructions::iterate_over_instructions.
class BackgroundPainter {
 public:
  BackgroundPainter(pdflib::renderer<pdflib::BLEND2D>& renderer,
                    std::optional<pdflib::shape_instruction> background)
      : renderer_(renderer), background_(std::move(background)) {}

  void set_size(pdflib::size_instruction& instr) {
    renderer_.set_size(instr);
    if (background_.has_value()) renderer_.render_shape(*background_);
  }
  void render_text(pdflib::text_instruction& instr) {
    renderer_.render_text(instr);
  }
  void render_widget(pdflib::text_widget_instruction& instr) {
    renderer_.render_widget(instr);
  }
  void render_bitmap(pdflib::bitmap_instruction& instr) {
    renderer_.render_bitmap(instr);
  }
  void render_shape(pdflib::shape_instruction& instr) {
    renderer_.render_shape(instr);
  }
  void render_shading(pdflib::shading_instruction& instr) {
    renderer_.render_shading(instr);
  }

 private:
  pdflib::renderer<pdflib::BLEND2D>& renderer_;
  std::optional<pdflib::shape_instruction> background_;
};

// The page-level families, the ones that need a page decoded.
bool WantPageItems(const pdfv1::ParseRequest& request) {
  for (pdfv1::PdfFamily family :
       {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_FONTS,
        pdfv1::PDF_FAMILY_EMBEDDED_FONTS, pdfv1::PDF_FAMILY_PLACED_IMAGES,
        pdfv1::PDF_FAMILY_VECTOR_SHAPES}) {
    if (WantFamily(request, family)) return true;
  }
  return false;
}

// The pages a call covers, zero-based and half-open.
struct PageSpan {
  int begin = 0;
  int end = 0;
};

// The contract's one rule for a set PageRange: end greater than begin. Any
// other uint32 range is valid, and SelectPages clamps it to the document.
grpc::Status CheckPageRange(const pdfv1::PageRange& range) {
  if (range.end() <= range.begin()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "page range end must be greater than begin (got " +
                            std::to_string(range.begin()) + ", " +
                            std::to_string(range.end()) + ")");
  }
  return grpc::Status::OK;
}

// The pages a checked range selects, clamped to the document in unsigned
// space before narrowing, so an end past the document stops at its last
// page and a begin past it (2^31 and up included) selects no page rather
// than turning into a negative index; unset selects every page.
PageSpan SelectPages(bool has_range, const pdfv1::PageRange& range,
                     int page_count) {
  const uint32_t count = page_count > 0 ? static_cast<uint32_t>(page_count) : 0;
  if (!has_range) return {0, static_cast<int>(count)};
  const uint32_t begin = std::min(range.begin(), count);
  const uint32_t end = std::max(begin, std::min(range.end(), count));
  return {static_cast<int>(begin), static_cast<int>(end)};
}

// Fails the call with RESOURCE_EXHAUSTED when any page of the span would
// need a raster wider or taller than Blend2D allows or with more pixels
// than the budget.
grpc::Status CheckRasterSizes(const DocumentPages& pages, PageSpan span,
                              double dpi, float scale, uint64_t max_pixels) {
  for (int i = span.begin; i < span.end; ++i) {
    const std::array<double, 2> size = RasterSize(pages.geometry(i), scale);
    if (size[0] > kMaxRasterSide || size[1] > kMaxRasterSide ||
        size[0] * size[1] > static_cast<double>(max_pixels)) {
      char detail[256];
      std::snprintf(detail, sizeof(detail),
                    "page %d at %g dpi needs a %.0fx%.0f pixel raster; the "
                    "limit is %llu pixels and %.0f per side",
                    i, dpi, size[0], size[1],
                    static_cast<unsigned long long>(max_pixels),
                    kMaxRasterSide);
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, detail);
    }
  }
  return grpc::Status::OK;
}

// The status a Render call ends with at a page it cannot produce. The
// contract has no per-page message on the Render stream, so the page index
// and the reason ride in the status (RequestDone.detail over ParseStream),
// and the same line goes to the log. The reason may be the engine's
// exception text: it is cut to a length a status trailer carries and held
// to printable ASCII, because it ends up in a proto string.
grpc::Status RenderPageFailure(
    int index, const std::string& reason,
    grpc::StatusCode code = grpc::StatusCode::INTERNAL) {
  const std::string message = "Render failed on page " + std::to_string(index) +
                              ": " + PrintableAscii(reason, 320);
  LOG_S(ERROR) << message;
  return grpc::Status(code, message);
}

// Rasterizes one decoded page into msg's raster, over the request's
// background. A page the rasterizer throws on, or that leaves no canvas of
// its size, is a failure of the call, like a page the engine cannot
// decode: the status says which page and why.
grpc::Status RasterizePage(pdflib::pdf_decoder<pdflib::PAGE>& decoder,
                           int index, const PageGeometry& geometry,
                           const pdflib::render_config& render_cfg,
                           const pdfv1::RenderRequest& request,
                           pdfv1::RenderResponse* msg) {
  pdflib::size_instruction size = SizeInstruction(geometry);
  decoder.get_instructions().add_size_instruction(size);
  try {
    pdflib::renderer<pdflib::BLEND2D> rnd(render_cfg);
    BackgroundPainter painter(rnd, BackgroundFill(request, geometry));
    decoder.get_instructions().iterate_over_instructions(painter);
    auto canvas = rnd.get_canvas();
    const auto& shape = rnd.get_shape();
    const auto height = static_cast<uint32_t>(shape[0]);
    const auto width = static_cast<uint32_t>(shape[1]);
    if (canvas == nullptr || canvas->empty() ||
        canvas->size() != static_cast<size_t>(width) * height * 4) {
      return RenderPageFailure(
          index, "the rasterizer produced no canvas of its size");
    }
    auto* raster = msg->mutable_raster();
    raster->set_page_index(static_cast<uint32_t>(index));
    raster->set_dpi(request.dpi());
    FillPixels(*canvas, width, height, request.pixel_format(), raster);
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    return RenderPageFailure(
        index, std::string("the rasterizer failed on it: ") + e.what());
  }
}

void AddPageWarning(pdfv1::ParseTrailer* trailer, int page_index,
                    const std::string& message,
                    std::optional<pdfv1::PdfFamily> family = std::nullopt) {
  auto* warning = trailer->add_warnings();
  warning->set_page_index(static_cast<uint32_t>(page_index));
  if (family.has_value()) warning->set_family(*family);
  warning->set_message(message);
}

// What a page with no usable /MediaBox is told, in Parse and in Render.
constexpr char kGuessedMediaBox[] =
    "the page stores no usable /MediaBox; it is sized as US Letter (612 x 792 "
    "points), and its geometry is relative to that guess";

// The most pages a Parse lists one by one for a fault every page of a
// damaged document can share; the rest are counted.
constexpr int kMaxPagesListed = 16;

// What the link reader left out of a page, as warnings on that page.
void AddLinkWarnings(const LinksRead& read, int page_index,
                     pdfv1::ParseTrailer* trailer) {
  if (read.without_rect > 0) {
    AddPageWarning(trailer, page_index,
                   std::to_string(read.without_rect) +
                       " link(s) have no usable /Rect; each is listed with "
                       "no bbox",
                   pdfv1::PDF_FAMILY_HYPERLINKS);
  }
  if (read.unresolved > 0) {
    AddPageWarning(trailer, page_index,
                   std::to_string(read.unresolved) +
                       " link(s) point at a page the document does not hold "
                       "and are not listed",
                   pdfv1::PDF_FAMILY_HYPERLINKS);
  }
  if (!read.untyped.empty()) {
    uint64_t total = 0;
    std::string kinds;
    for (const auto& [action, count] : read.untyped) {
      total += count;
      if (!kinds.empty()) kinds += ", ";
      kinds += PrintableAscii(action, 48);
    }
    AddPageWarning(trailer, page_index,
                   std::to_string(total) +
                       " link(s) are not listed: their action (" + kinds +
                       ") is neither a URI nor a go-to within the document",
                   pdfv1::PDF_FAMILY_HYPERLINKS);
  }
}

// The most different qpdf warnings one page, or the document's load, puts
// in the trailer; the rest are counted. A badly damaged file can make qpdf
// say thousands.
constexpr size_t kMaxQpdfWarnings = 16;

// Adds what qpdf said to the trailer's warnings: about the page given, or
// with no page about the document as a whole. A warning qpdf repeats (it
// says it again each time it reads the object) is added once.
using WarningList = google::protobuf::RepeatedPtrField<pdfv1::ParseWarning>;

void AddQpdfWarnings(const std::vector<QPDFExc>& said,
                     std::optional<int> page_index, WarningList* warnings,
                     std::optional<pdfv1::PdfFamily> family = std::nullopt);

// The Parse form: into the trailer.
void AddQpdfWarnings(const std::vector<QPDFExc>& said,
                     std::optional<int> page_index,
                     pdfv1::ParseTrailer* trailer) {
  AddQpdfWarnings(said, page_index, trailer->mutable_warnings());
}

void AddQpdfWarnings(const std::vector<QPDFExc>& said,
                     std::optional<int> page_index, WarningList* warnings,
                     std::optional<pdfv1::PdfFamily> family) {
  auto add = [&](std::string message) {
    auto* warning = warnings->Add();
    if (page_index.has_value()) {
      warning->set_page_index(static_cast<uint32_t>(*page_index));
    }
    if (family.has_value()) warning->set_family(*family);
    warning->set_message(std::move(message));
  };
  std::set<std::string> listed;
  size_t unlisted = 0;
  for (const QPDFExc& warning : said) {
    std::string text = QpdfWarningText(warning);
    if (listed.count(text) > 0) continue;
    if (listed.size() >= kMaxQpdfWarnings) {
      ++unlisted;
      continue;
    }
    listed.insert(text);
    add(std::move(text));
  }
  if (unlisted > 0) {
    add("qpdf: " + std::to_string(unlisted) + " more warning(s) here, past the " +
        std::to_string(kMaxQpdfWarnings) + " listed");
  }
}

// Writes a message once its strings are valid UTF-8 (valid_utf8.h); every
// message of a Parse or Render answer leaves through here.
template <typename Message>
bool Send(MessageSink<Message>* writer, Message* message) {
  MakeStringsValidUtf8(message);
  return writer->Write(*message);
}

}  // namespace

RenderLimits RenderLimitsFromEnv() {
  RenderLimits limits;
  const char* value = std::getenv("GRPC_QPARSE_RENDER_MAX_PIXELS");
  if (value == nullptr || *value == '\0') return limits;
  char* end = nullptr;
  const uint64_t parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0) return limits;
  limits.max_pixels = std::min(parsed, kMaxRenderPixels);
  return limits;
}

uint64_t DecodedStreamLimitFromEnv() {
  const char* value = std::getenv("GRPC_QPARSE_MAX_DECODED_STREAM_BYTES");
  if (value == nullptr || *value == '\0') return kMaxDecodedStreamBytes;
  char* end = nullptr;
  const uint64_t parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0) {
    return kMaxDecodedStreamBytes;
  }
  return parsed;
}

void InitEngine(const std::string& resources_dir,
                uint64_t decoded_stream_limit) {
  loguru::g_stderr_verbosity = loguru::Verbosity_ERROR;
  // qpdf throws once a Flate, RunLength or LZW decoder has written this
  // much, and the engine treats that stream as undecodable. qpdf's own LZW
  // decoder has no limit, so ours replaces it. jpeglib is held to the same
  // bytes. qpdf's other decoders (ASCIIHex, ASCII85, Crypt) give at most
  // four bytes per input byte, and the engine's own image codecs are
  // bounded by the pixels an image declares (OversizedImages).
  Pl_Flate::memory_limit(decoded_stream_limit);
  Pl_RunLength::setMemoryLimit(decoded_stream_limit);
  InstallLimitedLzwDecode(decoded_stream_limit);
  Pl_DCT::setMemoryLimit(static_cast<long>(std::min<uint64_t>(
      decoded_stream_limit,
      static_cast<uint64_t>(std::numeric_limits<long>::max()))));
  resource_utils::set_resources_dir(resources_dir);
  // The font resource registries (glyphs, encodings, cmaps, base fonts)
  // load once per process; without this, decoding throws on the first
  // encoding lookup.
  nlohmann::json data = nlohmann::json::object();
  data[pdflib::pdf_resource<pdflib::PAGE_FONT>::RESOURCE_DIR_KEY] =
      resources_dir;
  std::unordered_map<std::string, double> timings;
  pdflib::pdf_resource<pdflib::PAGE_FONT>::initialise(data, timings);
}

grpc::Status QparseServiceImpl::Probe(grpc::ServerContext* /*context*/,
                                      const pdfv1::ProbeRequest* request,
                                      pdfv1::ProbeResponse* response) {
  LoadedDocument loaded;
  switch (ResolveDocumentBytes(request->document(), &cache_, &loaded)) {
    case ResolveOutcome::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, loaded.detail);
    case ResolveOutcome::kVerdict:
      break;
    case ResolveOutcome::kOk:
      LoadDocument(request->document(), &loaded);
      break;
  }
  FillCapabilities(loaded, response->mutable_capabilities());
  MakeStringsValidUtf8(response);
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Parse(
    grpc::ServerContext* context, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  ServerWriterSink<pdfv1::ParseResponse> sink(writer);
  return ParseTo(context, request, nullptr, &sink);
}

grpc::Status QparseServiceImpl::ParseTo(
    grpc::ServerContext* context, const pdfv1::ParseRequest* request,
    const std::shared_ptr<std::string>& stream_bytes,
    MessageSink<pdfv1::ParseResponse>* writer) {
  if (request->has_pages()) {
    if (grpc::Status range = CheckPageRange(request->pages()); !range.ok()) {
      return range;
    }
  }
  CallSlots::Slot slot;
  if (grpc::Status admitted = slots_.Acquire(context, &slot); !admitted.ok()) {
    return admitted;
  }
  LoadedDocument loaded;
  switch (ResolveRequestBytes(request->document(), stream_bytes, &cache_, &loaded)) {
    case ResolveOutcome::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, loaded.detail);
    case ResolveOutcome::kVerdict:
      break;
    case ResolveOutcome::kOk:
      LoadDocument(request->document(), &loaded);
      break;
  }
  const int page_count =
      loaded.status == pdfv1::LOAD_STATUS_OK ? loaded.pages->page_count() : 0;

  // The header carries the whole inventory, read from the page
  // dictionaries; no page content is decoded for it.
  pdfv1::ParseResponse header_msg;
  auto* header = header_msg.mutable_header();
  FillCapabilities(loaded, header->mutable_capabilities());
  for (int i = 0; i < page_count; ++i) {
    FillPageInfo(loaded.pages->geometry(i), i, header->add_pages());
  }
  if (!Send(writer, &header_msg) || loaded.status != pdfv1::LOAD_STATUS_OK) {
    return grpc::Status::OK;
  }

  bool client_ok = true;
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  // What qpdf repaired to open the document: a rebuilt cross-reference
  // table, an object it could not read. The document parsed, and the
  // caller is told it was not whole.
  AddQpdfWarnings(loaded.pages->load_warnings(), std::nullopt, trailer);
  if (const std::optional<long long> declared =
          loaded.pages->MismatchedPageCount()) {
    auto* warning = trailer->add_warnings();
    warning->set_family(pdfv1::PDF_FAMILY_PAGE_INVENTORY);
    warning->set_message("the page tree declares " + std::to_string(*declared) +
                         " page(s), " + std::to_string(page_count) +
                         " could be read");
  }
  // The inventory is the whole document's, so these are too.
  int guessed_boxes = 0;
  for (int i = 0; i < page_count; ++i) {
    if (!loaded.pages->geometry(i).media_box_guessed) continue;
    if (++guessed_boxes <= kMaxPagesListed) {
      AddPageWarning(trailer, i, kGuessedMediaBox,
                     pdfv1::PDF_FAMILY_PAGE_INVENTORY);
    }
  }
  if (guessed_boxes > kMaxPagesListed) {
    auto* warning = trailer->add_warnings();
    warning->set_family(pdfv1::PDF_FAMILY_PAGE_INVENTORY);
    warning->set_message(std::to_string(guessed_boxes - kMaxPagesListed) +
                         " more page(s) store no usable /MediaBox, past the " +
                         std::to_string(kMaxPagesListed) + " listed");
  }

  // Document-level families.
  if (WantFamily(*request, pdfv1::PDF_FAMILY_DOC_METADATA)) {
    pdfv1::ParseResponse msg;
    auto* meta = msg.mutable_doc_meta();
    std::vector<std::string> not_carried;
    loaded.pages->FillDocMeta(meta, &not_carried);
    for (const std::string& key : not_carried) {
      auto* warning = trailer->add_warnings();
      warning->set_family(pdfv1::PDF_FAMILY_DOC_METADATA);
      warning->set_message("info key " + PrintableAscii(key, 128) +
                           " is not carried: its value is not text, a name, "
                           "a number or a boolean");
    }
    nlohmann::json xmp = loaded.doc->get_meta_xml();
    if (xmp.is_string() && !xmp.get<std::string>().empty()) {
      meta->set_xmp_xml(xmp.get<std::string>());
    }
    ++counts[pdfv1::PDF_FAMILY_DOC_METADATA];
    client_ok = Send(writer, &msg);
  }
  if (client_ok && WantFamily(*request, pdfv1::PDF_FAMILY_OUTLINE) &&
      loaded.pages->HasOutline()) {
    pdfv1::ParseResponse msg;
    try {
      const OutlineRead read = loaded.pages->ReadOutline(msg.mutable_outline());
      counts[pdfv1::PDF_FAMILY_OUTLINE] = read.items;
      if (read.unresolved > 0) {
        auto* warning = trailer->add_warnings();
        warning->set_family(pdfv1::PDF_FAMILY_OUTLINE);
        warning->set_message(
            std::to_string(read.unresolved) +
            " outline item(s) point at a page the document does not hold; "
            "each keeps its title and has no destination");
      }
      if (read.cut > 0) {
        auto* warning = trailer->add_warnings();
        warning->set_family(pdfv1::PDF_FAMILY_OUTLINE);
        warning->set_message(
            std::to_string(read.cut) + " outline item(s) lie below level " +
            std::to_string(kMaxOutlineLevels) +
            ", the deepest this service follows; they and every item under "
            "them are left out");
      }
      if (msg.outline().roots_size() > 0) client_ok = Send(writer, &msg);
    } catch (const std::exception& e) {
      auto* warning = trailer->add_warnings();
      warning->set_family(pdfv1::PDF_FAMILY_OUTLINE);
      warning->set_message("the outline could not be read and is left out: " +
                           PrintableAscii(e.what(), 256));
    }
    // What qpdf said while the outline's objects were read.
    AddQpdfWarnings(loaded.pages->TakeWarnings(), std::nullopt, trailer);
  }

  const PageSpan span =
      SelectPages(request->has_pages(), request->pages(), page_count);

  const bool want_fonts = WantFamily(*request, pdfv1::PDF_FAMILY_FONTS);
  const bool want_programs =
      WantFamily(*request, pdfv1::PDF_FAMILY_EMBEDDED_FONTS);
  const pdflib::decode_config config = ParseDecodeConfig(want_programs);
  const bool want_page_items = WantPageItems(*request);
  const bool want_links = WantFamily(*request, pdfv1::PDF_FAMILY_HYPERLINKS);
  const bool want_fields = WantFamily(*request, pdfv1::PDF_FAMILY_FORM_FIELDS);
  FontInterner fonts;
  std::set<uint32_t> programs_sent;
  for (int i = span.begin; client_ok && i < span.end; ++i) {
    // A cancelled or expired call stops decoding at the next page.
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "the call was cancelled");
    }
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    pdfv1::FontTableChunk new_fonts;
    std::vector<pdfv1::EmbeddedFont> embedded;
    uint64_t page_cells = 0;
    bool content_skipped = false;

    // Links and form fields come from the page's annotations, not from the
    // engine's decode of its content, so a page the engine cannot decode
    // still has them.
    if (want_links) {
      try {
        AddLinkWarnings(loaded.pages->ReadLinks(i, chunk), i, trailer);
      } catch (const std::exception& e) {
        chunk->clear_hyperlinks();
        AddPageWarning(trailer, i,
                       "the page's links could not be read and are left out: " +
                           PrintableAscii(e.what(), 256),
                       pdfv1::PDF_FAMILY_HYPERLINKS);
      }
    }
    if (want_fields) {
      try {
        const FieldsRead read = loaded.pages->ReadFormFields(i, chunk);
        if (read.without_rect > 0) {
          AddPageWarning(trailer, i,
                         std::to_string(read.without_rect) +
                             " form field widget(s) have no usable /Rect; "
                             "each is listed with no rect",
                         pdfv1::PDF_FAMILY_FORM_FIELDS);
        }
      } catch (const std::exception& e) {
        chunk->clear_form_fields();
        AddPageWarning(trailer, i,
                       "the page's form fields could not be read and are left "
                       "out: " + PrintableAscii(e.what(), 256),
                       pdfv1::PDF_FAMILY_FORM_FIELDS);
      }
    }

    if (want_page_items) {
      // A content stream that does not decode leaves the engine with an
      // empty page, which it returns as if the page were blank. The chunk
      // still goes out, with what the page has besides its content (links,
      // fields), and the trailer says the content is missing.
      if (const std::optional<std::string> undecodable =
              loaded.pages->UndecodableContent(i)) {
        AddPageWarning(trailer, i,
                       "content stream does not decode, the page's content "
                       "is missing: " + *undecodable);
      }
      try {
        ++decoded_pages_;
        std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> decoder =
            loaded.pages->Decode(i, config);
        nlohmann::json pj = decoder->get(config);
        const std::array<double, 4> media =
            decoder->get_page_dimension().get_media_bbox();
        const std::array<double, 4>& crop =
            loaded.pages->geometry(i).crop_box;
        const PageSpaceOffset offset{media[0] - crop[0], media[1] - crop[1]};

        // Which program each of the page's fonts draws with, so a cell and
        // its program get the same font id, and which fonts embed one.
        PageFonts page_fonts(loaded.pages->Fonts(i));
        if (want_fonts || want_programs) {
          decoder->get_instructions().iterate_over_instructions(page_fonts);
        }

        // The engine writes all three for every decoded page, empty when
        // the page has none; one that is missing, or not in the shape this
        // build reads, fails the page (engine_tables.h). A row that does
        // not fit is left out and counted.
        const auto& sanitized = pj.contains("sanitized") ? pj["sanitized"] : pj;
        auto entry = [&sanitized](const char* key) -> const nlohmann::json& {
          auto it = sanitized.find(key);
          if (it == sanitized.end()) {
            throw EngineShapeError(
                std::string("the engine's page output has no \"") + key +
                "\" entry");
          }
          return *it;
        };
        auto said_skipped = [trailer, i](const TableRead& read,
                                         pdfv1::PdfFamily family,
                                         const char* what) {
          if (read.skipped == 0) return;
          AddPageWarning(trailer, i,
                         std::to_string(read.skipped) + " " + what +
                             " in the engine's output are not in the shape "
                             "this build reads and are left out",
                         family);
        };
        if (WantFamily(*request, pdfv1::PDF_FAMILY_TEXT_CELLS)) {
          const TableRead read = FillTextCells(
              entry("cells"), offset,
              [&](const std::string& key, const std::string& name) {
                return fonts.Intern(page_fonts.WrittenName(key, name),
                                    page_fonts.ProgramOf(key, name),
                                    page_fonts.Embedded(key, name), &new_fonts);
              },
              chunk);
          page_cells = read.items;
          said_skipped(read, pdfv1::PDF_FAMILY_TEXT_CELLS, "text cell row(s)");
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_VECTOR_SHAPES)) {
          said_skipped(FillShapes(entry("shapes"), offset, chunk),
                       pdfv1::PDF_FAMILY_VECTOR_SHAPES, "shape(s)");
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_PLACED_IMAGES)) {
          said_skipped(FillImages(entry("images"), offset, chunk),
                       pdfv1::PDF_FAMILY_PLACED_IMAGES, "image row(s)");
        }
        // The font table lists every font the page draws with, cells
        // requested or not; programs go only to a call that asked for them.
        if (want_fonts) {
          for (const auto& [key, program] : page_fonts.fonts()) {
            fonts.Intern(page_fonts.WrittenName(key.first, key.second), program,
                         page_fonts.Embedded(key.first, key.second),
                         &new_fonts);
          }
        }
        if (want_programs) {
          CollectPrograms(page_fonts, &fonts, &new_fonts, &programs_sent,
                          &embedded);
        }
      } catch (const std::exception& e) {
        // The page's content is skipped, and said so in the trailer: what
        // the engine had given of it so far is taken back, so no half-read
        // page goes out as a page. Font ids it already took are still
        // announced, so later pages that share a font never point at an
        // entry the client has not seen.
        AddPageWarning(trailer, i,
                       std::string("page skipped: ") + e.what());
        chunk->clear_text_cells();
        chunk->clear_shapes();
        chunk->clear_images();
        embedded.clear();
        page_cells = 0;
        content_skipped = true;
      }
    }
    // What qpdf said while the page was read: a stream it could not decode,
    // an object it had to repair.
    AddQpdfWarnings(loaded.pages->TakePageWarnings(i), i, trailer);

    counts[pdfv1::PDF_FAMILY_TEXT_CELLS] += page_cells;
    counts[pdfv1::PDF_FAMILY_PLACED_IMAGES] += chunk->images_size();
    counts[pdfv1::PDF_FAMILY_HYPERLINKS] += chunk->hyperlinks_size();
    counts[pdfv1::PDF_FAMILY_FORM_FIELDS] += chunk->form_fields_size();
    counts[pdfv1::PDF_FAMILY_VECTOR_SHAPES] += chunk->shapes_size();
    if (new_fonts.fonts_size() > 0) {
      counts[pdfv1::PDF_FAMILY_FONTS] +=
          static_cast<uint64_t>(new_fonts.fonts_size());
      pdfv1::ParseResponse fonts_msg;
      *fonts_msg.mutable_fonts() = new_fonts;
      client_ok = Send(writer, &fonts_msg);
      if (!client_ok) break;
    }
    // A page whose content was skipped has a chunk only for what was read
    // without the engine, its links and form fields.
    if (content_skipped && chunk->hyperlinks_size() == 0 &&
        chunk->form_fields_size() == 0) {
      continue;
    }
    client_ok = Send(writer, &page_msg);
    for (auto& program : embedded) {
      if (!client_ok) break;
      pdfv1::ParseResponse font_msg;
      *font_msg.mutable_embedded_font() = std::move(program);
      ++counts[pdfv1::PDF_FAMILY_EMBEDDED_FONTS];
      client_ok = Send(writer, &font_msg);
    }
  }
  if (!client_ok) return grpc::Status::OK;

  if (const auto images = counts.find(pdfv1::PDF_FAMILY_PLACED_IMAGES);
      request->options().include_image_data() && images != counts.end() &&
      images->second > 0) {
    auto* warning = trailer->add_warnings();
    warning->set_family(pdfv1::PDF_FAMILY_PLACED_IMAGES);
    warning->set_message(
        "include_image_data is not served by this backend: the " +
        std::to_string(images->second) +
        " placed image(s) are listed with their placement and declared size "
        "and carry no image samples");
  }

  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  Send(writer, &trailer_msg);
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Render(
    grpc::ServerContext* context, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  ServerWriterSink<pdfv1::RenderResponse> sink(writer);
  return RenderTo(context, request, nullptr, &sink);
}

grpc::Status QparseServiceImpl::RenderTo(
    grpc::ServerContext* context, const pdfv1::RenderRequest* request,
    const std::shared_ptr<std::string>& stream_bytes,
    MessageSink<pdfv1::RenderResponse>* writer) {
  // NaN fails every comparison, so it is rejected by name; the scale the
  // rasterizer takes is a float, which a tiny DPI rounds to zero.
  const double dpi = request->dpi();
  const float scale = static_cast<float>(dpi / 72.0);
  if (!std::isfinite(dpi) || dpi <= 0.0 || !(scale > 0.0f)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi must be a positive finite number");
  }
  if (request->has_pages()) {
    if (grpc::Status range = CheckPageRange(request->pages()); !range.ok()) {
      return range;
    }
  }
  if (grpc::Status background = CheckBackground(*request); !background.ok()) {
    return background;
  }
  CallSlots::Slot slot;
  if (grpc::Status admitted = slots_.Acquire(context, &slot); !admitted.ok()) {
    return admitted;
  }
  LoadedDocument loaded;
  switch (ResolveRequestBytes(request->document(), stream_bytes, &cache_, &loaded)) {
    case ResolveOutcome::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, loaded.detail);
    case ResolveOutcome::kVerdict:
      break;
    case ResolveOutcome::kOk:
      LoadDocument(request->document(), &loaded);
      break;
  }
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    // A failed load, cache miss included, is typed in a one-message head
    // stream, never a bare gRPC error.
    pdfv1::RenderResponse head_msg;
    auto* head = head_msg.mutable_head();
    head->set_load_status(loaded.status);
    if (!loaded.detail.empty()) head->set_load_detail(loaded.detail);
    Send(writer, &head_msg);
    return grpc::Status::OK;
  }

  const PageSpan span = SelectPages(request->has_pages(), request->pages(),
                                    loaded.pages->page_count());

  // Every page is sized before any is rendered, so a page over the budget
  // fails the call before a raster streams or a canvas is allocated.
  if (grpc::Status sized = CheckRasterSizes(*loaded.pages, span, dpi, scale,
                                            render_limits_.max_pixels);
      !sized.ok()) {
    return sized;
  }

  const pdflib::decode_config config = RenderDecodeConfig(scale);
  pdflib::render_config render_cfg;
  render_cfg.scale = scale;
  const AnnotationAppearances annotations =
      request->omit_annotations() ? AnnotationAppearances::kOmit
                                  : AnnotationAppearances::kDraw;

  for (int i = span.begin; i < span.end; ++i) {
    // A cancelled or expired call stops rendering at the next page.
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "the call was cancelled");
    }
    // A page that cannot be decoded or rasterized fails the call there: an
    // OK Render stream carries every page of its range.
    // The engine would draw a page whose content stream does not decode
    // as a blank page.
    if (const std::optional<std::string> undecodable =
            loaded.pages->UndecodableContent(i)) {
      return RenderPageFailure(
          i, "its content stream does not decode: " + *undecodable);
    }
    std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> decoder;
    std::vector<std::string> oversized_images;
    try {
      ++decoded_pages_;
      decoder = loaded.pages->Decode(i, config, render_limits_.max_pixels,
                                     annotations, &oversized_images);
    } catch (const std::exception& e) {
      return RenderPageFailure(
          i, std::string("the engine could not decode it: ") + e.what());
    }
    // What qpdf said while the engine read the page rides beside the
    // page's raster.
    const std::vector<QPDFExc> said = loaded.pages->TakePageWarnings(i);
    // An image over the pixel budget was never decoded. A page that draws
    // one is not rendered without it; a page that only shares a resource
    // dictionary with it is.
    if (!oversized_images.empty()) {
      DrawnOversizedImage drawn(oversized_images);
      decoder->get_instructions().iterate_over_instructions(drawn);
      if (drawn.found().has_value()) {
        return RenderPageFailure(
            i,
            "it draws image " + *drawn.found() +
                ", which has, or whose mask has, more than " +
                std::to_string(render_limits_.max_pixels) +
                " pixels (GRPC_QPARSE_RENDER_MAX_PIXELS)",
            grpc::StatusCode::RESOURCE_EXHAUSTED);
      }
    }
    const PageGeometry& geometry = loaded.pages->geometry(i);
    pdfv1::RenderResponse msg;
    const grpc::Status rendered =
        RasterizePage(*decoder, i, geometry, render_cfg, *request, &msg);
    const std::array<double, 2> raster_size = RasterSize(geometry, scale);
    if (raster_size[0] * raster_size[1] > kPooledCanvasPixels) {
      ReleasePooledCanvas();
    }
    if (!rendered.ok()) return rendered;
    // The raster is there and is not the whole page: said beside it.
    auto warn = [&msg, i](std::string message) {
      auto* warning = msg.add_warnings();
      warning->set_page_index(static_cast<uint32_t>(i));
      warning->set_family(pdfv1::PDF_FAMILY_PAGE_RASTER);
      warning->set_message(std::move(message));
    };
    if (geometry.media_box_guessed) warn(kGuessedMediaBox);
    ImagesWithoutSamples missing;
    decoder->get_instructions().iterate_over_instructions(missing);
    if (missing.count() > 0) {
      std::string names;
      for (const std::string& name : missing.names()) {
        if (!names.empty()) names += ", ";
        names += name;
      }
      warn(std::to_string(missing.count()) +
           " image(s) the page draws are missing from the raster: their "
           "samples did not decode" +
           (names.empty() ? std::string() : " (" + names + ")"));
    }
    AddQpdfWarnings(said, i, msg.mutable_warnings(),
                    pdfv1::PDF_FAMILY_PAGE_RASTER);
    if (!Send(writer, &msg)) return grpc::Status::OK;
  }
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::ParseStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<pdfv1::ParseStreamResponse, pdfv1::ParseStreamRequest>*
        stream) {
  ParseStreamHandlers handlers;
  handlers.parse = [this, context](const pdfv1::ParseRequest& request,
                                   const std::shared_ptr<std::string>& bytes,
                                   MessageSink<pdfv1::ParseResponse>* sink,
                                   RequestCancel*) {
    return ParseTo(context, &request, bytes, sink);
  };
  handlers.render = [this, context](const pdfv1::RenderRequest& request,
                                    const std::shared_ptr<std::string>& bytes,
                                    MessageSink<pdfv1::RenderResponse>* sink,
                                    RequestCancel*) {
    return RenderTo(context, &request, bytes, sink);
  };
  handlers.verdict = [](pdfv1::BackendCapabilities* caps, pdfv1::LoadStatus status,
                        const std::string& detail) {
    LoadedDocument verdict;
    verdict.status = status;
    verdict.detail = detail;
    FillCapabilities(verdict, caps);
  };
  handlers.sha256_hex = [](const std::string& bytes) { return Sha256Hex(bytes); };
  // A verified upload is cached like a verified unary upload, so a client
  // that falls back to Probe, Parse or Render can address it by hash.
  handlers.verified = [this](const std::string& sha256,
                             const std::shared_ptr<std::string>& bytes) {
    cache_.Insert(sha256, bytes);
  };
  ParseStreamLimits limits;
  limits.max_document_bytes = kMaxStreamDocumentBytes;
  limits.max_parallel_requests = kStreamParallelRequests;
  return ServeParseStream(context, stream, handlers, limits);
}

grpc::Status QparseServiceImpl::GetServiceInfo(
    grpc::ServerContext* /*context*/,
    const pdfv1::ServiceInfoRequest* /*request*/,
    pdfv1::ServiceInfoResponse* response) {
  // backend_name and engine_version are the same identity strings every
  // Probe reports; build_version is stamped at compile time (the image tag
  // in Docker, git describe in a checkout, "dev" otherwise).
  response->set_backend_name(kBackendName);
  response->set_engine_version(kEngineVersion);
  response->set_build_version(GRPC_QPARSE_BUILD_VERSION);
  auto* ui = response->mutable_ui();
  ui->set_title("qparse");
  ui->set_path("/ui/qparse");
  ui->set_description(
      "Reading-order text cells, vector shapes, and page rasters from the "
      "qpdf-based cell parser; no web UI yet, the tab appears when one lands");
  return grpc::Status::OK;
}

}  // namespace grpc_qparse

#include "qparse_service_impl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// The engine umbrella headers (header-only library over qpdf).
#include <parse.h>
#include <render.h>

#include "sha256.h"

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
  // usable one, as PDFium falls back.
  std::array<double, 4> media_box = {0.0, 0.0, 612.0, 792.0};
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

QPDFObjectHandle BoxArray(const std::array<double, 4>& box) {
  return QPDFObjectHandle::newArray(
      QPDFObjectHandle::Rectangle(box[0], box[1], box[2], box[3]));
}

// The document's pages for Parse and Render, on a qpdf handle of the
// call's own beside the engine's document decoder. The inventory comes
// from the page dictionaries without decoding any content; a page's
// content is decoded only when the call reaches it.
class DocumentPages {
 public:
  // Opens the bytes, which must outlive this object: qpdf reads them in
  // place. Returns false with qpdf's message when the bytes do not open.
  bool Open(const std::string& bytes, const std::optional<std::string>& password,
            std::string* error) {
    try {
      qpdf_.setSuppressWarnings(true);
      qpdf_.processMemoryFile("grpc-qparse request", bytes.data(), bytes.size(),
                              password.has_value() ? password->c_str() : nullptr);
      pages_ = qpdf_.getAllPages();
      geometry_.reserve(pages_.size());
      for (const auto& page : pages_) geometry_.push_back(ReadPageGeometry(page));
    } catch (const std::exception& e) {
      *error = e.what();
      return false;
    }
    return true;
  }

  int page_count() const { return static_cast<int>(pages_.size()); }

  const PageGeometry& geometry(int index) const {
    return geometry_.at(static_cast<size_t>(index));
  }

  // Decodes one page. The engine reads /CropBox and /Rotate from the page
  // dictionary alone, never through the page tree, and on a rotated page it
  // turns every cell, shape and image into display orientation and drops
  // the angle. Here it decodes the page with the boxes resolved above and
  // /Rotate 0, so every item stays in unrotated user space, the contract's
  // frame; Render passes the rotation to the rasterizer instead. The edits
  // touch only this call's qpdf objects, never the bytes.
  std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> Decode(
      int index, const pdflib::decode_config& config) {
    QPDFObjectHandle page = pages_.at(static_cast<size_t>(index));
    const PageGeometry& page_geometry = geometry(index);
    page.replaceKey("/MediaBox", BoxArray(page_geometry.media_box));
    page.replaceKey("/CropBox", BoxArray(page_geometry.crop_box));
    page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(0));
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
    auto decoder =
        std::make_shared<pdflib::pdf_decoder<pdflib::PAGE>>(page, index);
    decoder->decode_page(config);
    return decoder;
  }

 private:
  QPDF qpdf_;
  std::vector<QPDFObjectHandle> pages_;
  std::vector<PageGeometry> geometry_;
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

// Opens the resolved bytes in the engine; with_pages also opens the
// per-page handle Parse and Render work through.
void LoadDocument(const pdfv1::PdfDocument& request, bool with_pages,
                  LoadedDocument* out) {
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
  if (ok && with_pages) {
    out->pages = std::make_unique<DocumentPages>();
    ok = out->pages->Open(data, password, &out->detail);
    if (!ok) out->pages.reset();
  }
  if (ok) {
    out->status = pdfv1::LOAD_STATUS_OK;
    return;
  }
  out->doc.reset();
  // The engine reports open failures as a plain false; qpdf's password and
  // damage cases are not distinguishable from here yet.
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
  nlohmann::json toc = loaded.doc->get_table_of_contents();
  bool has_outline = toc.is_array() ? !toc.empty() : toc.is_object();
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
    }
  }
}

bool WantFamily(const pdfv1::ParseRequest& request, pdfv1::PdfFamily family) {
  if (request.families().empty()) return true;
  return std::find(request.families().begin(), request.families().end(),
                   family) != request.families().end();
}

double NumberOr(const nlohmann::json& j, const char* key, double fallback) {
  auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<double>() : fallback;
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
// true (inherited) /Rotate, and the boxes in unrotated user space.
void FillPageInfo(const PageGeometry& geometry, int index,
                  pdfv1::PageInfo* info) {
  info->set_page_index(static_cast<uint32_t>(index));
  info->set_width_pts(geometry.DisplayWidth());
  info->set_height_pts(geometry.DisplayHeight());
  info->set_rotation_degrees(geometry.rotation);
  SetBox(geometry.media_box, info->mutable_media_box());
  SetBox(geometry.crop_box, info->mutable_crop_box());
}

// Where a decoded page's sanitized cells, shapes and images sit: the engine
// moves them to the origin of its page boundary (the MediaBox, see
// ParseDecodeConfig), and the contract wants them back in user space.
struct UserSpaceOffset {
  double dx = 0.0;
  double dy = 0.0;
};

// Column indices resolved from a header+data table once per table.
int ColumnIndex(const nlohmann::json& header, const std::string& name) {
  for (size_t i = 0; i < header.size(); ++i) {
    if (header[i].get<std::string>() == name) return static_cast<int>(i);
  }
  return -1;
}

// Assigns stable ids to font names within one stream.
class FontInterner {
 public:
  uint32_t Intern(const std::string& name, bool* is_new) {
    auto it = ids_.find(name);
    if (it != ids_.end()) {
      *is_new = false;
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(ids_.size());
    ids_.emplace(name, id);
    *is_new = true;
    return id;
  }

 private:
  std::map<std::string, uint32_t> ids_;
};

void FillTextCells(const nlohmann::json& cells, const UserSpaceOffset& offset,
                   FontInterner* fonts, pdfv1::PageChunk* chunk,
                   pdfv1::FontTableChunk* new_fonts, uint64_t* cell_count) {
  if (!cells.contains("header") || !cells.contains("data")) return;
  const auto& header = cells["header"];
  const int ix0 = ColumnIndex(header, "x0");
  const int iy0 = ColumnIndex(header, "y0");
  const int ix1 = ColumnIndex(header, "x1");
  const int iy1 = ColumnIndex(header, "y1");
  const int irx0 = ColumnIndex(header, "r_x0");
  const int itext = ColumnIndex(header, "text");
  const int imode = ColumnIndex(header, "rendering-mode");
  const int ispace = ColumnIndex(header, "space-width");
  const int ifont = ColumnIndex(header, "font-name");
  const int iltr = ColumnIndex(header, "left_to_right");
  const int iwidget = ColumnIndex(header, "widget");
  for (const auto& row : cells["data"]) {
    if (itext < 0 || !row[itext].is_string()) continue;
    // Widget cells re-enter through the form-field family.
    if (iwidget >= 0 && row[iwidget].is_boolean() && row[iwidget].get<bool>()) {
      continue;
    }
    auto* cell = chunk->add_text_cells();
    cell->set_text(row[itext].get<std::string>());
    auto* bbox = cell->mutable_bbox();
    bbox->set_x0(row[ix0].get<double>() + offset.dx);
    bbox->set_y0(row[iy0].get<double>() + offset.dy);
    bbox->set_x1(row[ix1].get<double>() + offset.dx);
    bbox->set_y1(row[iy1].get<double>() + offset.dy);
    auto* quad = cell->mutable_quad();
    quad->set_x0(row[irx0 + 0].get<double>() + offset.dx);
    quad->set_y0(row[irx0 + 1].get<double>() + offset.dy);
    quad->set_x1(row[irx0 + 2].get<double>() + offset.dx);
    quad->set_y1(row[irx0 + 3].get<double>() + offset.dy);
    quad->set_x2(row[irx0 + 4].get<double>() + offset.dx);
    quad->set_y2(row[irx0 + 5].get<double>() + offset.dy);
    quad->set_x3(row[irx0 + 6].get<double>() + offset.dx);
    quad->set_y3(row[irx0 + 7].get<double>() + offset.dy);
    if (iltr >= 0 && row[iltr].is_boolean()) {
      cell->set_direction(row[iltr].get<bool>()
                              ? pdfv1::TEXT_DIRECTION_LEFT_TO_RIGHT
                              : pdfv1::TEXT_DIRECTION_RIGHT_TO_LEFT);
    }
    if (ispace >= 0 && row[ispace].is_number()) {
      cell->set_space_width(row[ispace].get<double>());
    }
    if (imode >= 0 && row[imode].is_number()) {
      int mode = row[imode].get<int>();
      if (mode >= 0 && mode <= 7) {
        cell->set_rendering_mode(
            static_cast<pdfv1::TextRenderingMode>(mode + 1));
      }
    }
    if (ifont >= 0 && row[ifont].is_string()) {
      std::string name = row[ifont].get<std::string>();
      if (!name.empty() && name.front() == '/') name.erase(0, 1);
      bool is_new = false;
      uint32_t id = fonts->Intern(name, &is_new);
      cell->set_font_id(id);
      if (is_new) {
        auto* ref = new_fonts->add_fonts();
        ref->set_font_id(id);
        ref->set_base_name(name);
      }
    }
    ++*cell_count;
  }
}

void FillShapes(const nlohmann::json& shapes, const UserSpaceOffset& offset,
                pdfv1::PageChunk* chunk) {
  if (!shapes.is_array()) return;
  for (const auto& s : shapes) {
    if (!s.contains("x") || !s.contains("y")) continue;
    const auto& xs = s["x"];
    const auto& ys = s["y"];
    if (!xs.is_array() || xs.size() != ys.size() || xs.empty()) continue;
    auto* shape = chunk->add_shapes();
    double min_x = xs[0].get<double>() + offset.dx;
    double min_y = ys[0].get<double>() + offset.dy;
    double max_x = min_x;
    double max_y = min_y;
    for (size_t i = 0; i < xs.size(); ++i) {
      double x = xs[i].get<double>() + offset.dx;
      double y = ys[i].get<double>() + offset.dy;
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_x = std::max(max_x, x);
      max_y = std::max(max_y, y);
      auto* seg = shape->add_segments();
      pdfv1::PathPoint* point =
          i == 0 ? seg->mutable_move_to() : seg->mutable_line_to();
      point->set_x(x);
      point->set_y(y);
    }
    // closing-type 2 marks an explicitly closed subpath in the engine's
    // sampled polyline representation.
    if (static_cast<int>(NumberOr(s, "closing-type", 0)) == 2) {
      shape->add_segments()->set_close(true);
    }
    auto* box = shape->mutable_bbox();
    box->set_x0(min_x);
    box->set_y0(min_y);
    box->set_x1(max_x);
    box->set_y1(max_y);
    if (s.contains("line-width") && s["line-width"].is_number()) {
      shape->set_line_width(s["line-width"].get<double>());
    }
    auto fill_color = [](const nlohmann::json& rgb, pdfv1::Color* color) {
      if (!rgb.is_array() || rgb.size() != 3) return;
      color->set_red(rgb[0].get<double>() / 255.0);
      color->set_green(rgb[1].get<double>() / 255.0);
      color->set_blue(rgb[2].get<double>() / 255.0);
      color->set_alpha(1.0);
    };
    if (s.contains("rgb-filling")) {
      fill_color(s["rgb-filling"], shape->mutable_fill_color());
    }
    if (s.contains("rgb-stroking")) {
      fill_color(s["rgb-stroking"], shape->mutable_stroke_color());
    }
  }
}

void FillImages(const nlohmann::json& images, const UserSpaceOffset& offset,
                pdfv1::PageChunk* chunk) {
  if (!images.contains("header") || !images.contains("data")) return;
  const auto& header = images["header"];
  const int ix0 = ColumnIndex(header, "x0");
  const int ikey = ColumnIndex(header, "xobject_key");
  const int iw = ColumnIndex(header, "image_width");
  const int ih = ColumnIndex(header, "image_height");
  const int ibpc = ColumnIndex(header, "bits_per_component");
  const int ics = ColumnIndex(header, "color_space");
  for (const auto& row : images["data"]) {
    auto* image = chunk->add_images();
    auto* bbox = image->mutable_bbox();
    bbox->set_x0(row[ix0 + 0].get<double>() + offset.dx);
    bbox->set_y0(row[ix0 + 1].get<double>() + offset.dy);
    bbox->set_x1(row[ix0 + 2].get<double>() + offset.dx);
    bbox->set_y1(row[ix0 + 3].get<double>() + offset.dy);
    auto* quad = image->mutable_quad();
    quad->set_x0(bbox->x0());
    quad->set_y0(bbox->y0());
    quad->set_x1(bbox->x1());
    quad->set_y1(bbox->y0());
    quad->set_x2(bbox->x1());
    quad->set_y2(bbox->y1());
    quad->set_x3(bbox->x0());
    quad->set_y3(bbox->y1());
    if (iw >= 0 && row[iw].is_number()) {
      image->set_source_width_px(row[iw].get<uint32_t>());
    }
    if (ih >= 0 && row[ih].is_number()) {
      image->set_source_height_px(row[ih].get<uint32_t>());
    }
    if (ibpc >= 0 && row[ibpc].is_number()) {
      image->set_bits_per_component(row[ibpc].get<uint32_t>());
    }
    if (ics >= 0 && row[ics].is_string()) {
      std::string cs = row[ics].get<std::string>();
      if (!cs.empty() && cs.front() == '/') cs.erase(0, 1);
      if (!cs.empty()) image->set_colorspace(cs);
    }
    if (ikey >= 0 && row[ikey].is_string()) {
      std::string key = row[ikey].get<std::string>();
      if (!key.empty() && key.front() == '/') key.erase(0, 1);
      if (!key.empty()) image->set_resource_name(key);
    }
  }
}

void FillHyperlinks(const nlohmann::json& links, pdfv1::PageChunk* chunk) {
  if (!links.is_array()) return;
  for (const auto& l : links) {
    if (!l.contains("uri")) continue;
    auto* link = chunk->add_hyperlinks();
    link->set_uri(l["uri"].get<std::string>());
    // The link /Rect as stored: unrotated user space, the cells' frame.
    SetBox(NumberOr(l, "x0", 0.0), NumberOr(l, "y0", 0.0),
           NumberOr(l, "x1", 0.0), NumberOr(l, "y1", 0.0),
           link->mutable_bbox());
  }
}

// ISO 32000-1 field flag bits (table 221, 226, 228); bit n of the spec is
// 1 << (n - 1).
constexpr uint32_t kFieldFlagReadOnly = 1u << 0;
constexpr uint32_t kFieldFlagRadio = 1u << 15;
constexpr uint32_t kFieldFlagPushButton = 1u << 16;
constexpr uint32_t kFieldFlagCombo = 1u << 17;

// The typed kind from the inherited /FT and /Ff: /Btn and /Ch each cover
// several kinds that only the flags tell apart.
pdfv1::FormFieldKind FieldKind(const std::string& type, uint32_t flags) {
  if (type == "/Tx") return pdfv1::FORM_FIELD_KIND_TEXT;
  if (type == "/Sig") return pdfv1::FORM_FIELD_KIND_SIGNATURE;
  if (type == "/Btn") {
    if (flags & kFieldFlagPushButton) return pdfv1::FORM_FIELD_KIND_PUSH_BUTTON;
    if (flags & kFieldFlagRadio) return pdfv1::FORM_FIELD_KIND_RADIO_BUTTON;
    return pdfv1::FORM_FIELD_KIND_CHECK_BOX;
  }
  if (type == "/Ch") {
    return (flags & kFieldFlagCombo) ? pdfv1::FORM_FIELD_KIND_COMBO_BOX
                                     : pdfv1::FORM_FIELD_KIND_LIST_BOX;
  }
  return pdfv1::FORM_FIELD_KIND_UNSPECIFIED;
}

// One FormField per widget the engine reports. The engine resolves the
// field type, /Ff and /TU through the /Parent chain (qpdf's form field
// helper) and reads /AS from the widget itself, the same raw state
// docling-core exposes as PdfWidget.widget_field_flags and
// widget_appearance_state.
void FillFormFields(const nlohmann::json& widgets, pdfv1::PageChunk* chunk) {
  if (!widgets.is_array()) return;
  for (const auto& w : widgets) {
    auto* field = chunk->add_form_fields();
    const std::string type = w.value("field_type", "");
    uint32_t flags = 0;
    if (w.contains("field_flags") && w["field_flags"].is_number_integer()) {
      flags = static_cast<uint32_t>(w["field_flags"].get<int64_t>());
      field->set_flags(flags);
      field->set_read_only((flags & kFieldFlagReadOnly) != 0);
    }
    const pdfv1::FormFieldKind kind = FieldKind(type, flags);
    field->set_kind(kind);
    field->set_name(w.value("field_name", ""));
    std::string value = w.value("text", "");
    // A button's /V is a name, which the engine spells with its slash;
    // the other backends (PDFium, poppler) report the bare state name, so
    // this one does too.
    if ((kind == pdfv1::FORM_FIELD_KIND_CHECK_BOX ||
         kind == pdfv1::FORM_FIELD_KIND_RADIO_BUTTON) &&
        value.starts_with('/')) {
      value.erase(0, 1);
    }
    if (!value.empty()) field->set_value(value);
    std::string desc = w.value("description", "");
    if (!desc.empty()) field->set_alternate_name(desc);
    std::string state = w.value("appearance_state", "");
    if (!state.empty()) field->set_appearance_state(state);
    // The widget /Rect as stored: unrotated user space, the cells' frame.
    SetBox(NumberOr(w, "x0", 0.0), NumberOr(w, "y0", 0.0),
           NumberOr(w, "x1", 0.0), NumberOr(w, "y1", 0.0),
           field->mutable_rect());
  }
}

void FillOutlineNode(const nlohmann::json& entry, pdfv1::OutlineNode* node) {
  node->set_title(entry.value("title", ""));
  if (entry.contains("children") && entry["children"].is_array()) {
    for (const auto& child : entry["children"]) {
      FillOutlineNode(child, node->add_children());
    }
  }
}

// Instruction visitor that collects embedded font programs. Duck-typed
// against pdf_render_instructions::iterate_over_instructions.
class EmbeddedFontCollector {
 public:
  EmbeddedFontCollector(FontInterner* fonts, pdfv1::FontTableChunk* new_fonts,
                        std::vector<pdfv1::EmbeddedFont>* out)
      : fonts_(fonts), new_fonts_(new_fonts), out_(out) {}

  void set_size(const pdflib::size_instruction&) {}
  void render_widget(pdflib::text_widget_instruction&) {}
  void render_bitmap(pdflib::bitmap_instruction&) {}
  void render_shape(pdflib::shape_instruction&) {}
  void render_shading(pdflib::shading_instruction&) {}

  void render_text(pdflib::text_instruction& instr) {
    if (!instr.has_embedded_font()) return;
    const auto& blob = instr.get_embedded_font();
    if (blob == nullptr || !blob->has_bytes()) return;
    std::string name = blob->get_base_font().empty() ? blob->get_font_name()
                                                     : blob->get_base_font();
    if (!name.empty() && name.front() == '/') name.erase(0, 1);
    bool is_new = false;
    uint32_t id = fonts_->Intern(name, &is_new);
    if (is_new) {
      auto* ref = new_fonts_->add_fonts();
      ref->set_font_id(id);
      ref->set_base_name(name);
      ref->set_embedded(true);
    }
    if (!emitted_.insert(id).second) return;
    pdfv1::EmbeddedFont program;
    program.set_font_id(id);
    const auto& bytes = blob->get_bytes();
    program.set_program(std::string(bytes->begin(), bytes->end()));
    if (blob->get_source_key() == "/FontFile") {
      program.set_format(pdfv1::FONT_PROGRAM_FORMAT_TYPE1);
    } else if (blob->get_source_key() == "/FontFile2") {
      program.set_format(pdfv1::FONT_PROGRAM_FORMAT_TRUETYPE);
    } else if (blob->get_source_key() == "/FontFile3") {
      program.set_format(pdfv1::FONT_PROGRAM_FORMAT_CFF);
    }
    out_->push_back(std::move(program));
  }

 private:
  FontInterner* fonts_;
  pdfv1::FontTableChunk* new_fonts_;
  std::vector<pdfv1::EmbeddedFont>* out_;
  std::set<uint32_t> emitted_;
};

// Parse reports geometry and text, never image samples, so image XObjects
// are measured but not decoded. The page boundary is the MediaBox: the
// engine drops every cell that is not wholly inside its boundary, and a
// cell that only straddles the CropBox edge is still on the page.
pdflib::decode_config ParseDecodeConfig() {
  pdflib::decode_config config;
  config.page_boundary = "media_box";
  config.extract_bitmap_pixels = false;
  config.extract_font_programs = true;
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

// The page-level families, the ones that need a page decoded.
bool WantPageItems(const pdfv1::ParseRequest& request) {
  for (pdfv1::PdfFamily family :
       {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_FONTS,
        pdfv1::PDF_FAMILY_EMBEDDED_FONTS, pdfv1::PDF_FAMILY_PLACED_IMAGES,
        pdfv1::PDF_FAMILY_VECTOR_SHAPES, pdfv1::PDF_FAMILY_HYPERLINKS,
        pdfv1::PDF_FAMILY_FORM_FIELDS}) {
    if (WantFamily(request, family)) return true;
  }
  return false;
}

void AddPageWarning(pdfv1::ParseTrailer* trailer, int page_index,
                    const std::string& message) {
  auto* warning = trailer->add_warnings();
  warning->set_page_index(static_cast<uint32_t>(page_index));
  warning->set_message(message);
}

}  // namespace

void InitEngine(const std::string& resources_dir) {
  loguru::g_stderr_verbosity = loguru::Verbosity_ERROR;
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
      LoadDocument(request->document(), /*with_pages=*/false, &loaded);
      break;
  }
  FillCapabilities(loaded, response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Parse(
    grpc::ServerContext* context, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
  LoadedDocument loaded;
  switch (ResolveDocumentBytes(request->document(), &cache_, &loaded)) {
    case ResolveOutcome::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, loaded.detail);
    case ResolveOutcome::kVerdict:
      break;
    case ResolveOutcome::kOk:
      LoadDocument(request->document(), /*with_pages=*/true, &loaded);
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
  if (!writer->Write(header_msg) || loaded.status != pdfv1::LOAD_STATUS_OK) {
    return grpc::Status::OK;
  }

  bool client_ok = true;

  // Document-level families.
  if (WantFamily(*request, pdfv1::PDF_FAMILY_DOC_METADATA)) {
    pdfv1::ParseResponse msg;
    auto* meta = msg.mutable_doc_meta();
    nlohmann::json xmp = loaded.doc->get_meta_xml();
    if (xmp.is_string() && !xmp.get<std::string>().empty()) {
      meta->set_xmp_xml(xmp.get<std::string>());
    }
    client_ok = writer->Write(msg);
  }
  if (client_ok && WantFamily(*request, pdfv1::PDF_FAMILY_OUTLINE)) {
    nlohmann::json toc = loaded.doc->get_table_of_contents();
    if (toc.is_object() && toc.contains("children")) {
      pdfv1::ParseResponse msg;
      auto* chunk = msg.mutable_outline();
      for (const auto& child : toc["children"]) {
        FillOutlineNode(child, chunk->add_roots());
      }
      client_ok = writer->Write(msg);
    } else if (toc.is_array() && !toc.empty()) {
      pdfv1::ParseResponse msg;
      auto* chunk = msg.mutable_outline();
      for (const auto& entry : toc) {
        FillOutlineNode(entry, chunk->add_roots());
      }
      client_ok = writer->Write(msg);
    }
  }

  int begin = 0;
  int end = page_count;
  if (request->has_pages()) {
    begin =
        std::min<int>(static_cast<int>(request->pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request->pages().end()), page_count);
  }

  const pdflib::decode_config config = ParseDecodeConfig();
  const bool want_page_items = WantPageItems(*request);
  FontInterner fonts;
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (int i = begin; client_ok && i < end; ++i) {
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

    if (want_page_items) {
      try {
        ++decoded_pages_;
        std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> decoder =
            loaded.pages->Decode(i, config);
        nlohmann::json pj = decoder->get(config);
        const std::array<double, 4> media =
            decoder->get_page_dimension().get_media_bbox();
        const UserSpaceOffset offset{media[0], media[1]};

        const auto& sanitized = pj.contains("sanitized") ? pj["sanitized"] : pj;
        const auto& original = pj.contains("original") ? pj["original"] : pj;
        if (WantFamily(*request, pdfv1::PDF_FAMILY_TEXT_CELLS) &&
            sanitized.contains("cells")) {
          FillTextCells(sanitized["cells"], offset, &fonts, chunk, &new_fonts,
                        &counts[pdfv1::PDF_FAMILY_TEXT_CELLS]);
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_VECTOR_SHAPES) &&
            sanitized.contains("shapes")) {
          FillShapes(sanitized["shapes"], offset, chunk);
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_PLACED_IMAGES) &&
            sanitized.contains("images")) {
          FillImages(sanitized["images"], offset, chunk);
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_HYPERLINKS) &&
            original.contains("hyperlinks")) {
          FillHyperlinks(original["hyperlinks"], chunk);
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_FORM_FIELDS) &&
            original.contains("widgets")) {
          FillFormFields(original["widgets"], chunk);
        }
        if (WantFamily(*request, pdfv1::PDF_FAMILY_FONTS) ||
            WantFamily(*request, pdfv1::PDF_FAMILY_EMBEDDED_FONTS)) {
          EmbeddedFontCollector collector(&fonts, &new_fonts, &embedded);
          decoder->get_instructions().iterate_over_instructions(collector);
        }
      } catch (const std::exception& e) {
        // The page is skipped, and said so in the trailer. Font ids it
        // already took are still announced, so later pages that share a
        // font never point at an entry the client has not seen.
        AddPageWarning(trailer, i,
                       std::string("page skipped: ") + e.what());
        if (new_fonts.fonts_size() > 0) {
          counts[pdfv1::PDF_FAMILY_FONTS] +=
              static_cast<uint64_t>(new_fonts.fonts_size());
          pdfv1::ParseResponse fonts_msg;
          *fonts_msg.mutable_fonts() = new_fonts;
          client_ok = writer->Write(fonts_msg);
        }
        continue;
      }
    }

    counts[pdfv1::PDF_FAMILY_PLACED_IMAGES] += chunk->images_size();
    counts[pdfv1::PDF_FAMILY_HYPERLINKS] += chunk->hyperlinks_size();
    counts[pdfv1::PDF_FAMILY_FORM_FIELDS] += chunk->form_fields_size();
    counts[pdfv1::PDF_FAMILY_VECTOR_SHAPES] += chunk->shapes_size();
    if (new_fonts.fonts_size() > 0) {
      counts[pdfv1::PDF_FAMILY_FONTS] +=
          static_cast<uint64_t>(new_fonts.fonts_size());
      pdfv1::ParseResponse fonts_msg;
      *fonts_msg.mutable_fonts() = new_fonts;
      client_ok = writer->Write(fonts_msg);
      if (!client_ok) break;
    }
    client_ok = writer->Write(page_msg);
    for (auto& program : embedded) {
      if (!client_ok) break;
      pdfv1::ParseResponse font_msg;
      *font_msg.mutable_embedded_font() = std::move(program);
      ++counts[pdfv1::PDF_FAMILY_EMBEDDED_FONTS];
      client_ok = writer->Write(font_msg);
    }
  }
  if (!client_ok) return grpc::Status::OK;

  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  writer->Write(trailer_msg);
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Render(
    grpc::ServerContext* context, const pdfv1::RenderRequest* request,
    grpc::ServerWriter<pdfv1::RenderResponse>* writer) {
  if (request->dpi() <= 0.0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "dpi must be positive");
  }
  LoadedDocument loaded;
  switch (ResolveDocumentBytes(request->document(), &cache_, &loaded)) {
    case ResolveOutcome::kInvalidArgument:
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, loaded.detail);
    case ResolveOutcome::kVerdict:
      break;
    case ResolveOutcome::kOk:
      LoadDocument(request->document(), /*with_pages=*/true, &loaded);
      break;
  }
  if (loaded.status != pdfv1::LOAD_STATUS_OK) {
    // A failed load, cache miss included, is typed in a one-message head
    // stream, never a bare gRPC error.
    pdfv1::RenderResponse head_msg;
    auto* head = head_msg.mutable_head();
    head->set_load_status(loaded.status);
    if (!loaded.detail.empty()) head->set_load_detail(loaded.detail);
    writer->Write(head_msg);
    return grpc::Status::OK;
  }

  int page_count = loaded.pages->page_count();
  int begin = 0;
  int end = page_count;
  if (request->has_pages()) {
    begin =
        std::min<int>(static_cast<int>(request->pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request->pages().end()), page_count);
  }

  const float scale = static_cast<float>(request->dpi() / 72.0);
  const pdflib::decode_config config = RenderDecodeConfig(scale);
  pdflib::render_config render_cfg;
  render_cfg.scale = scale;

  for (int i = begin; i < end; ++i) {
    // A cancelled or expired call stops rendering at the next page.
    if (context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "the call was cancelled");
    }
    std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> decoder;
    try {
      ++decoded_pages_;
      decoder = loaded.pages->Decode(i, config);
    } catch (const std::exception&) {
      continue;
    }
    pdflib::size_instruction size = SizeInstruction(loaded.pages->geometry(i));
    decoder->get_instructions().add_size_instruction(size);
    pdflib::renderer<pdflib::BLEND2D> rnd(render_cfg);
    decoder->get_instructions().iterate_over_instructions(rnd);
    auto canvas = rnd.get_canvas();
    const auto& shape = rnd.get_shape();
    if (canvas == nullptr || canvas->empty()) continue;
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(static_cast<uint32_t>(i));
    raster->set_height_px(static_cast<uint32_t>(shape[0]));
    raster->set_width_px(static_cast<uint32_t>(shape[1]));
    raster->set_stride_bytes(static_cast<uint32_t>(shape[1] * 4));
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_RGBA8);
    raster->set_dpi(request->dpi());
    raster->set_pixels(canvas->data(), canvas->size());
    if (!writer->Write(msg)) return grpc::Status::OK;
  }
  return grpc::Status::OK;
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

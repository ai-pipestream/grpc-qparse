#include "qparse_service_impl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// The engine umbrella headers (header-only library over qpdf).
#include <parse.h>
#include <render.h>

#include "sha256.h"

namespace grpc_qparse {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

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

struct LoadedDocument {
  pdflib::pdf_timings timings;
  std::unique_ptr<pdflib::pdf_decoder<pdflib::DOCUMENT>> doc;
  // The resolved document bytes; kept alive for the engine's decode.
  std::shared_ptr<std::string> buffer;
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

void FillBoxFromArray(const nlohmann::json& arr, pdfv1::BoundingBox* box) {
  if (!arr.is_array() || arr.size() != 4) return;
  box->set_x0(arr[0].get<double>());
  box->set_y0(arr[1].get<double>());
  box->set_x1(arr[2].get<double>());
  box->set_y1(arr[3].get<double>());
}

void FillPageInfo(const nlohmann::json& dim, int index, pdfv1::PageInfo* info) {
  info->set_page_index(static_cast<uint32_t>(index));
  const auto& bbox = dim["bbox"];
  double width = NumberOr(dim, "width", 0.0);
  double height = NumberOr(dim, "height", 0.0);
  if (width == 0.0 && bbox.is_array() && bbox.size() == 4) {
    width = bbox[2].get<double>() - bbox[0].get<double>();
  }
  if (height == 0.0 && bbox.is_array() && bbox.size() == 4) {
    height = bbox[3].get<double>() - bbox[1].get<double>();
  }
  info->set_width_pts(width);
  info->set_height_pts(height);
  int angle = static_cast<int>(NumberOr(dim, "angle", 0.0));
  info->set_rotation_degrees(((angle % 360) + 360) % 360);
  if (dim.contains("rectangles")) {
    const auto& rects = dim["rectangles"];
    if (rects.contains("media-bbox")) {
      FillBoxFromArray(rects["media-bbox"], info->mutable_media_box());
    }
    if (rects.contains("crop-bbox")) {
      FillBoxFromArray(rects["crop-bbox"], info->mutable_crop_box());
    }
  }
}

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

void FillTextCells(const nlohmann::json& cells, FontInterner* fonts,
                   pdfv1::PageChunk* chunk, pdfv1::FontTableChunk* new_fonts,
                   uint64_t* cell_count) {
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
    bbox->set_x0(row[ix0].get<double>());
    bbox->set_y0(row[iy0].get<double>());
    bbox->set_x1(row[ix1].get<double>());
    bbox->set_y1(row[iy1].get<double>());
    auto* quad = cell->mutable_quad();
    quad->set_x0(row[irx0 + 0].get<double>());
    quad->set_y0(row[irx0 + 1].get<double>());
    quad->set_x1(row[irx0 + 2].get<double>());
    quad->set_y1(row[irx0 + 3].get<double>());
    quad->set_x2(row[irx0 + 4].get<double>());
    quad->set_y2(row[irx0 + 5].get<double>());
    quad->set_x3(row[irx0 + 6].get<double>());
    quad->set_y3(row[irx0 + 7].get<double>());
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

void FillShapes(const nlohmann::json& shapes, pdfv1::PageChunk* chunk) {
  if (!shapes.is_array()) return;
  for (const auto& s : shapes) {
    if (!s.contains("x") || !s.contains("y")) continue;
    const auto& xs = s["x"];
    const auto& ys = s["y"];
    if (!xs.is_array() || xs.size() != ys.size() || xs.empty()) continue;
    auto* shape = chunk->add_shapes();
    double min_x = xs[0].get<double>();
    double min_y = ys[0].get<double>();
    double max_x = min_x;
    double max_y = min_y;
    for (size_t i = 0; i < xs.size(); ++i) {
      double x = xs[i].get<double>();
      double y = ys[i].get<double>();
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

void FillImages(const nlohmann::json& images, pdfv1::PageChunk* chunk) {
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
    bbox->set_x0(row[ix0 + 0].get<double>());
    bbox->set_y0(row[ix0 + 1].get<double>());
    bbox->set_x1(row[ix0 + 2].get<double>());
    bbox->set_y1(row[ix0 + 3].get<double>());
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
    auto* box = link->mutable_bbox();
    box->set_x0(NumberOr(l, "x0", 0.0));
    box->set_y0(NumberOr(l, "y0", 0.0));
    box->set_x1(NumberOr(l, "x1", 0.0));
    box->set_y1(NumberOr(l, "y1", 0.0));
  }
}

void FillFormFields(const nlohmann::json& widgets, pdfv1::PageChunk* chunk) {
  if (!widgets.is_array()) return;
  for (const auto& w : widgets) {
    auto* field = chunk->add_form_fields();
    std::string type = w.value("field_type", "");
    if (type == "/Tx") {
      field->set_kind(pdfv1::FORM_FIELD_KIND_TEXT);
    } else if (type == "/Btn") {
      field->set_kind(pdfv1::FORM_FIELD_KIND_PUSH_BUTTON);
    } else if (type == "/Ch") {
      field->set_kind(pdfv1::FORM_FIELD_KIND_COMBO_BOX);
    } else if (type == "/Sig") {
      field->set_kind(pdfv1::FORM_FIELD_KIND_SIGNATURE);
    }
    field->set_name(w.value("field_name", ""));
    std::string value = w.value("text", "");
    if (!value.empty()) field->set_value(value);
    std::string desc = w.value("description", "");
    if (!desc.empty()) field->set_alternate_name(desc);
    auto* box = field->mutable_rect();
    box->set_x0(NumberOr(w, "x0", 0.0));
    box->set_y0(NumberOr(w, "y0", 0.0));
    box->set_x1(NumberOr(w, "x1", 0.0));
    box->set_y1(NumberOr(w, "y1", 0.0));
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

pdflib::decode_config MakeDecodeConfig() {
  pdflib::decode_config config;
  config.extract_font_programs = true;


  return config;
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
      LoadDocument(request->document(), &loaded);
      break;
  }
  FillCapabilities(loaded, response->mutable_capabilities());
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Parse(
    grpc::ServerContext* /*context*/, const pdfv1::ParseRequest* request,
    grpc::ServerWriter<pdfv1::ParseResponse>* writer) {
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
  const pdflib::decode_config config = MakeDecodeConfig();
  int page_count =
      loaded.status == pdfv1::LOAD_STATUS_OK ? loaded.doc->get_number_of_pages()
                                             : 0;

  // Decode every page once; the header needs the inventory up front and
  // the page chunks reuse the same decoders.
  std::vector<std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>>> decoders(
      static_cast<size_t>(page_count));
  std::vector<nlohmann::json> page_jsons(static_cast<size_t>(page_count));
  pdfv1::ParseResponse header_msg;
  auto* header = header_msg.mutable_header();
  FillCapabilities(loaded, header->mutable_capabilities());
  for (int i = 0; i < page_count; ++i) {
    try {
      decoders[static_cast<size_t>(i)] = loaded.doc->decode_page(i, config);
    } catch (const std::exception&) {
      continue;
    }
    if (decoders[static_cast<size_t>(i)] == nullptr) continue;
    page_jsons[static_cast<size_t>(i)] =
        decoders[static_cast<size_t>(i)]->get(config);
    const auto& pj = page_jsons[static_cast<size_t>(i)];
    if (pj.contains("sanitized") && pj["sanitized"].contains("dimension")) {
      FillPageInfo(pj["sanitized"]["dimension"], i, header->add_pages());
    }
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

  FontInterner fonts;
  std::map<pdfv1::PdfFamily, uint64_t> counts;
  counts[pdfv1::PDF_FAMILY_PAGE_INVENTORY] = static_cast<uint64_t>(page_count);
  for (int i = begin; client_ok && i < end; ++i) {
    auto& decoder = decoders[static_cast<size_t>(i)];
    if (decoder == nullptr) continue;
    const auto& pj = page_jsons[static_cast<size_t>(i)];
    pdfv1::ParseResponse page_msg;
    auto* chunk = page_msg.mutable_page();
    chunk->set_page_index(static_cast<uint32_t>(i));
    pdfv1::FontTableChunk new_fonts;
    std::vector<pdfv1::EmbeddedFont> embedded;

    const auto& sanitized = pj.contains("sanitized") ? pj["sanitized"] : pj;
    const auto& original = pj.contains("original") ? pj["original"] : pj;
    if (WantFamily(*request, pdfv1::PDF_FAMILY_TEXT_CELLS) &&
        sanitized.contains("cells")) {
      FillTextCells(sanitized["cells"], &fonts, chunk, &new_fonts,
                    &counts[pdfv1::PDF_FAMILY_TEXT_CELLS]);
    }
    if (WantFamily(*request, pdfv1::PDF_FAMILY_VECTOR_SHAPES) &&
        sanitized.contains("shapes")) {
      FillShapes(sanitized["shapes"], chunk);
    }
    if (WantFamily(*request, pdfv1::PDF_FAMILY_PLACED_IMAGES) &&
        sanitized.contains("images")) {
      FillImages(sanitized["images"], chunk);
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

  pdfv1::ParseResponse trailer_msg;
  auto* trailer = trailer_msg.mutable_trailer();
  for (const auto& [family, count] : counts) {
    auto* entry = trailer->add_counts();
    entry->set_family(family);
    entry->set_count(count);
  }
  writer->Write(trailer_msg);
  return grpc::Status::OK;
}

grpc::Status QparseServiceImpl::Render(
    grpc::ServerContext* /*context*/, const pdfv1::RenderRequest* request,
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
    writer->Write(head_msg);
    return grpc::Status::OK;
  }

  int page_count = loaded.doc->get_number_of_pages();
  int begin = 0;
  int end = page_count;
  if (request->has_pages()) {
    begin =
        std::min<int>(static_cast<int>(request->pages().begin()), page_count);
    end = std::min<int>(static_cast<int>(request->pages().end()), page_count);
  }

  const float scale = static_cast<float>(request->dpi() / 72.0);
  pdflib::decode_config config = MakeDecodeConfig();
  config.extract_bitmap_pixels = true;
  config.bitmap_target_pixels_per_unit = scale;
  pdflib::render_config render_cfg;
  render_cfg.scale = scale;

  for (int i = begin; i < end; ++i) {
    std::shared_ptr<pdflib::pdf_decoder<pdflib::PAGE>> decoder;
    try {
      decoder = loaded.doc->decode_page(i, config);
    } catch (const std::exception&) {
      continue;
    }
    if (decoder == nullptr) continue;
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

}  // namespace grpc_qparse

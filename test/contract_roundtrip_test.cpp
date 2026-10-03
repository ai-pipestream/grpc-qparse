// Contract test for grpc-qparse: start the engine-backed service in
// process, dial it through the generated stubs, and walk the tier 0
// families plus this backend's own strengths (reading-order cells, shapes,
// embedded fonts) over the hello.pdf, rich.pdf, fonts.pdf, frames.pdf,
// encrypted.pdf, bomb.pdf and lzw.pdf fixtures.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <loguru.hpp>

#include "qparse_service_impl.h"
#include "sha256.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void Check(bool ok, const std::string& what) { Check(ok, what.c_str()); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

bool Near(double a, double b, double tolerance) {
  return std::fabs(a - b) <= tolerance;
}

bool BoxNear(const pdfv1::BoundingBox& box, const std::array<double, 4>& want,
             double tolerance) {
  return Near(box.x0(), want[0], tolerance) &&
         Near(box.y0(), want[1], tolerance) &&
         Near(box.x1(), want[2], tolerance) &&
         Near(box.y1(), want[3], tolerance);
}

// Writes numbered objects into a PDF with a classic cross-reference table.
std::string AssemblePdf(const std::vector<std::string>& objects) {
  std::string out = "%PDF-1.7\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
    out += line;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return out;
}

std::string Stream(const std::string& data) {
  return "<< /Length " + std::to_string(data.size()) + " >>\nstream\n" +
         data + "\nendstream";
}

// A document of `pages` Letter pages that all draw one text-heavy content
// stream: slow enough to decode that a cancelled call visibly stops early.
std::string ManyPagePdf(int pages) {
  std::string content;
  for (int line = 0; line < 60; ++line) {
    content += "BT /F1 9 Tf 72 " + std::to_string(760 - line * 12) +
               " Td (The quick brown fox jumps over the lazy dog, line " +
               std::to_string(line) + ") Tj ET\n";
  }
  std::string kids;
  for (int i = 0; i < pages; ++i) kids += std::to_string(5 + i) + " 0 R ";
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) +
          " /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      Stream(content)};
  for (int i = 0; i < pages; ++i) {
    objects.push_back("<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>");
  }
  return AssemblePdf(objects);
}

std::unique_ptr<grpc::Server> StartServer(grpc::Service* service,
                                          int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           port);
  builder.RegisterService(service);
  return builder.BuildAndStart();
}

std::unique_ptr<pdfv1::PdfBackendService::Stub> Dial(int port) {
  return pdfv1::PdfBackendService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                          grpc::InsecureChannelCredentials()));
}

// Everything one Parse stream carried.
struct ParseResult {
  grpc::Status status;
  pdfv1::ParseHeader header;
  std::map<uint32_t, pdfv1::PageChunk> pages;
  std::map<uint32_t, pdfv1::FontRef> fonts;
  std::vector<pdfv1::EmbeddedFont> embedded;
  std::optional<pdfv1::ParseTrailer> trailer;
};

ParseResult ParseDocument(pdfv1::PdfBackendService::Stub* stub,
                          const std::string& pdf,
                          const std::vector<pdfv1::PdfFamily>& families,
                          std::optional<std::pair<uint32_t, uint32_t>> range) {
  grpc::ClientContext ctx;
  pdfv1::ParseRequest request;
  request.mutable_document()->set_data(pdf);
  for (pdfv1::PdfFamily family : families) request.add_families(family);
  if (range.has_value()) {
    request.mutable_pages()->set_begin(range->first);
    request.mutable_pages()->set_end(range->second);
  }
  ParseResult result;
  auto reader = stub->Parse(&ctx, request);
  pdfv1::ParseResponse msg;
  while (reader->Read(&msg)) {
    if (msg.has_header()) {
      result.header = msg.header();
    } else if (msg.has_page()) {
      result.pages[msg.page().page_index()].MergeFrom(msg.page());
    } else if (msg.has_fonts()) {
      for (const auto& font : msg.fonts().fonts()) {
        result.fonts[font.font_id()] = font;
      }
    } else if (msg.has_embedded_font()) {
      result.embedded.push_back(msg.embedded_font());
    } else if (msg.has_trailer()) {
      result.trailer = msg.trailer();
    }
  }
  result.status = reader->Finish();
  return result;
}

std::vector<pdfv1::PageRaster> RenderDocument(
    pdfv1::PdfBackendService::Stub* stub, const std::string& pdf, double dpi,
    pdfv1::PixelFormat format,
    std::optional<std::pair<uint32_t, uint32_t>> range, grpc::Status* status) {
  grpc::ClientContext ctx;
  pdfv1::RenderRequest request;
  request.mutable_document()->set_data(pdf);
  request.set_dpi(dpi);
  request.set_pixel_format(format);
  if (range.has_value()) {
    request.mutable_pages()->set_begin(range->first);
    request.mutable_pages()->set_end(range->second);
  }
  std::vector<pdfv1::PageRaster> rasters;
  auto reader = stub->Render(&ctx, request);
  pdfv1::RenderResponse msg;
  while (reader->Read(&msg)) {
    if (msg.has_raster()) rasters.push_back(msg.raster());
  }
  *status = reader->Finish();
  return rasters;
}

const pdfv1::TextCell* FindCell(const pdfv1::PageChunk& page,
                                const std::string& text) {
  for (const auto& cell : page.text_cells()) {
    if (cell.text().find(text) != std::string::npos) return &cell;
  }
  return nullptr;
}

// A box drawn at user-space corners, as the contract frame reports it on
// this page: shifted so the CropBox's lower-left corner is (0, 0).
std::array<double, 4> CropRelative(const pdfv1::PageInfo& info,
                                   const std::array<double, 4>& user) {
  const double x = info.crop_box().x0();
  const double y = info.crop_box().y0();
  return {user[0] - x, user[1] - y, user[2] - x, user[3] - y};
}

// The client's mapping (gRParse src/remote_page_source.cpp, PageFrame):
// contract boxes are relative to the CropBox and before /Rotate
// (PAGE_SPACE_CROP_BOX), and the rendered page is the CropBox turned
// clockwise by /Rotate with a top-left origin. Returns {left, top, right,
// bottom} in pixels at the given DPI.
std::array<double, 4> ToDisplayPixels(const pdfv1::PageInfo& info,
                                      const pdfv1::BoundingBox& box,
                                      double dpi) {
  const int rotation = ((info.rotation_degrees() % 360) + 360) % 360;
  const bool crop_relative =
      info.page_space() == pdfv1::PAGE_SPACE_CROP_BOX;
  const double origin_x = crop_relative ? 0.0 : info.crop_box().x0();
  const double origin_y = crop_relative ? 0.0 : info.crop_box().y0();
  const double width = info.crop_box().x1() - info.crop_box().x0();
  const double height = info.crop_box().y1() - info.crop_box().y0();
  auto to_display = [&](double x, double y) -> std::array<double, 2> {
    const double u = x - origin_x;
    const double down = height - (y - origin_y);
    switch (rotation) {
      case 90:
        return {height - down, u};
      case 180:
        return {width - u, height - down};
      case 270:
        return {down, width - u};
      default:
        return {u, down};
    }
  };
  const auto a = to_display(box.x0(), box.y0());
  const auto b = to_display(box.x1(), box.y1());
  const double scale = dpi / 72.0;
  return {std::min(a[0], b[0]) * scale, std::min(a[1], b[1]) * scale,
          std::max(a[0], b[0]) * scale, std::max(a[1], b[1]) * scale};
}

// Counts the pixels of an RGBA8 raster that pass `match`, inside or
// outside a pixel box.
template <typename Match>
std::pair<int, int> CountPixels(const pdfv1::PageRaster& raster,
                                const std::array<double, 4>& box,
                                Match match) {
  int inside = 0;
  int outside = 0;
  const auto* pixels =
      reinterpret_cast<const unsigned char*>(raster.pixels().data());
  for (uint32_t y = 0; y < raster.height_px(); ++y) {
    for (uint32_t x = 0; x < raster.width_px(); ++x) {
      const unsigned char* p = pixels + y * raster.stride_bytes() + x * 4;
      if (!match(p)) continue;
      const bool in = x + 0.5 >= box[0] && x + 0.5 <= box[2] &&
                      y + 0.5 >= box[1] && y + 0.5 <= box[3];
      ++(in ? inside : outside);
    }
  }
  return {inside, outside};
}

bool IsRed(const unsigned char* p) {
  return p[0] > 200 && p[1] < 60 && p[2] < 60;
}

// Near-black: glyph ink, not the renderer's light blue outline fallback.
bool IsInk(const unsigned char* p) {
  return p[0] < 100 && p[1] < 100 && p[2] < 100;
}

// The service's log lines at ERROR, collected from every server thread.
struct ErrorLog {
  std::mutex mutex;
  std::vector<std::string> lines;

  static void Collect(void* user_data, const loguru::Message& message) {
    auto* log = static_cast<ErrorLog*>(user_data);
    std::lock_guard<std::mutex> lock(log->mutex);
    log->lines.emplace_back(message.message);
  }

  bool Contains(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
      return l.find(text) != std::string::npos;
    });
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <hello.pdf> <resources dir>\n", argv[0]);
    return 2;
  }
  const std::string fixture_dir =
      std::string(argv[1]).substr(0, std::string(argv[1]).rfind('/'));
  const std::string hello = ReadFile(argv[1]);
  const std::string rich = ReadFile(fixture_dir + "/rich.pdf");
  Check(!hello.empty() && !rich.empty(), "fixtures read");

  // A 64 MiB decoded stream limit, well under the 512 MiB default, so the
  // bomb.pdf check below needs no more than that to prove the limit holds.
  constexpr uint64_t kTestDecodedStreamLimit = uint64_t{64} << 20;
  grpc_qparse::InitEngine(argv[2], kTestDecodedStreamLimit);

  grpc_qparse::QparseServiceImpl service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  Check(server != nullptr && port != 0, "server started");
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = pdfv1::PdfBackendService::NewStub(channel);

  // Probe basics.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "Probe RPC OK");
    const auto& caps = response.capabilities();
    Check(caps.backend_name() == "grpc-qparse", "backend name reported");
    Check(caps.load_status() == pdfv1::LOAD_STATUS_OK, "hello.pdf loads");
    Check(caps.page_count() == 1, "hello.pdf has one page");
    Check(caps.families_size() == pdfv1::PdfFamily_MAX,
          "a verdict for each family");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data("not a pdf");
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "non-PDF Probe RPC OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "non-PDF bytes report LOAD_STATUS_NOT_PDF");
  }

  // A document that does not open says why: encrypted.pdf (user password
  // "secret") needs a password, takes only the right one, and bytes past a
  // %PDF- header that qpdf cannot recover are corrupt, on all three RPCs.
  {
    const std::string encrypted = ReadFile(fixture_dir + "/encrypted.pdf");
    Check(!encrypted.empty(), "encrypted.pdf read");
    struct Opening {
      std::string data;
      std::optional<std::string> password;
      pdfv1::LoadStatus want;
      const char* what;
    };
    for (const Opening& opening :
         {Opening{encrypted, std::nullopt, pdfv1::LOAD_STATUS_PASSWORD_REQUIRED,
                  "encrypted.pdf without a password"},
          Opening{encrypted, "wrong", pdfv1::LOAD_STATUS_PASSWORD_INCORRECT,
                  "encrypted.pdf with a wrong password"},
          Opening{encrypted, "secret", pdfv1::LOAD_STATUS_OK,
                  "encrypted.pdf with its password"},
          Opening{"%PDF-1.7\nnothing a PDF parser can recover\n", std::nullopt,
                  pdfv1::LOAD_STATUS_CORRUPT, "a header and no body"}}) {
      const std::string what = std::string(opening.what) + ": ";
      pdfv1::PdfDocument document;
      document.set_data(opening.data);
      if (opening.password.has_value()) {
        document.set_password(*opening.password);
      }
      {
        grpc::ClientContext ctx;
        pdfv1::ProbeRequest request;
        *request.mutable_document() = document;
        pdfv1::ProbeResponse response;
        Check(stub->Probe(&ctx, request, &response).ok() &&
                  response.capabilities().load_status() == opening.want,
              what + "Probe reports " + pdfv1::LoadStatus_Name(opening.want));
        Check(opening.want == pdfv1::LOAD_STATUS_OK ||
                  !response.capabilities().load_detail().empty(),
              what + "Probe says why");
      }
      {
        grpc::ClientContext ctx;
        pdfv1::ParseRequest request;
        *request.mutable_document() = document;
        auto reader = stub->Parse(&ctx, request);
        pdfv1::ParseResponse msg;
        Check(reader->Read(&msg) && msg.has_header() &&
                  msg.header().capabilities().load_status() == opening.want,
              what + "Parse header reports " +
                  pdfv1::LoadStatus_Name(opening.want));
        while (reader->Read(&msg)) {
        }
        Check(reader->Finish().ok(), what + "Parse finishes OK");
      }
      {
        grpc::ClientContext ctx;
        pdfv1::RenderRequest request;
        *request.mutable_document() = document;
        request.set_dpi(9.0);
        auto reader = stub->Render(&ctx, request);
        pdfv1::RenderResponse msg;
        const bool read = reader->Read(&msg);
        Check(opening.want == pdfv1::LOAD_STATUS_OK
                  ? read && msg.has_raster()
                  : read && msg.has_head() &&
                        msg.head().load_status() == opening.want,
              what + "Render answers " + pdfv1::LoadStatus_Name(opening.want));
        while (reader->Read(&msg)) {
        }
        Check(reader->Finish().ok(), what + "Render finishes OK");
      }
    }
  }

  // GetServiceInfo: the identity block, independent of any document.
  {
    grpc::ClientContext ctx;
    pdfv1::ServiceInfoRequest request;
    pdfv1::ServiceInfoResponse response;
    Check(stub->GetServiceInfo(&ctx, request, &response).ok(),
          "GetServiceInfo RPC OK");
    Check(response.backend_name() == "grpc-qparse", "info backend name");
    Check(!response.engine_version().empty(), "info engine version");
    Check(!response.build_version().empty(), "info build version");
    Check(response.ui().title() == "qparse", "info UI title");
    Check(response.ui().path() == "/ui/qparse", "info UI path");
    Check(!response.ui().description().empty(), "info UI description");

    // backend_name and engine_version must be the strings Probe reports.
    grpc::ClientContext probe_ctx;
    pdfv1::ProbeRequest probe_request;
    probe_request.mutable_document()->set_data(hello);
    pdfv1::ProbeResponse probe_response;
    Check(stub->Probe(&probe_ctx, probe_request, &probe_response).ok(),
          "identity Probe RPC OK");
    Check(probe_response.capabilities().backend_name() ==
              response.backend_name(),
          "info backend name matches Probe");
    Check(probe_response.capabilities().engine_version() ==
              response.engine_version(),
          "info engine version matches Probe");
  }

  // Parse rich.pdf: tier 0 plus this engine's strengths.
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_data(rich);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;

    Check(reader->Read(&msg) && msg.has_header(), "header first");
    Check(msg.header().pages_size() == 1, "inventory lists the page");
    if (msg.header().pages_size() == 1) {
      const auto& page = msg.header().pages(0);
      Check(page.width_pts() > 611 && page.width_pts() < 613, "page width");
      Check(page.height_pts() > 791 && page.height_pts() < 793, "page height");
      Check(page.has_crop_box(), "crop box populated");
    }

    std::string all_text;
    int shapes = 0;
    int images = 0;
    int links = 0;
    int fields = 0;
    std::vector<pdfv1::FormField> form_fields;
    int outline_roots = 0;
    bool cell_has_quad = false;
    bool cell_has_space_width = false;
    std::vector<pdfv1::EmbeddedFont> embedded;
    std::map<int, uint64_t> counts;
    while (reader->Read(&msg)) {
      if (msg.has_page()) {
        for (const auto& cell : msg.page().text_cells()) {
          all_text += cell.text() + " ";
          if (cell.quad().x1() > cell.quad().x0()) cell_has_quad = true;
          if (cell.has_space_width()) cell_has_space_width = true;
        }
        shapes += msg.page().shapes_size();
        images += msg.page().images_size();
        links += msg.page().hyperlinks_size();
        fields += msg.page().form_fields_size();
        for (const auto& f : msg.page().form_fields()) form_fields.push_back(f);
      } else if (msg.has_embedded_font()) {
        embedded.push_back(msg.embedded_font());
      } else if (msg.has_outline()) {
        outline_roots = msg.outline().roots_size();
      } else if (msg.has_trailer()) {
        for (const auto& c : msg.trailer().counts()) {
          counts[c.family()] = c.count();
        }
      }
    }
    Check(reader->Finish().ok(), "Parse finished OK");
    Check(all_text.find("Tagged Hello") != std::string::npos,
          "cells carry the page text");
    Check(cell_has_quad, "cells carry the rotated quad");
    Check(cell_has_space_width, "cells carry the space width");
    Check(shapes >= 1, "vector shape extracted");
    Check(images == 1, "placed image extracted");
    Check(links >= 1, "hyperlink extracted");
    Check(fields == 2, "both form field widgets extracted");
    const pdfv1::FormField* text_field = nullptr;
    const pdfv1::FormField* check_box = nullptr;
    for (const auto& f : form_fields) {
      if (f.name() == "customer_name") text_field = &f;
      if (f.name() == "agree") check_box = &f;
    }
    Check(text_field != nullptr && check_box != nullptr,
          "text field and check box named");
    if (text_field != nullptr) {
      Check(text_field->kind() == pdfv1::FORM_FIELD_KIND_TEXT, "text kind");
      Check(text_field->value() == "Jordan Example", "text value");
      Check(text_field->alternate_name() == "Customer name", "text tooltip");
      Check(text_field->has_flags() && text_field->flags() == 0 &&
                !text_field->read_only(),
            "text field flags are empty");
      Check(!text_field->has_appearance_state(), "text widget has no /AS");
    }
    if (check_box != nullptr) {
      // /FT and /Ff (ReadOnly) come from the parent field, /AS from the
      // widget.
      Check(check_box->kind() == pdfv1::FORM_FIELD_KIND_CHECK_BOX,
            "check box kind from the inherited /FT and /Ff");
      Check(check_box->has_flags() && check_box->flags() == 1,
            "/Ff inherited from the parent");
      Check(check_box->read_only(), "read-only follows the inherited /Ff");
      Check(check_box->appearance_state() == "/Yes",
            "/AS keeps the leading slash");
      Check(check_box->alternate_name() == "I agree", "check box tooltip");
      Check(check_box->value() == "Yes", "button value is the bare state name");
    }
    Check(outline_roots == 2, "outline items extracted");
    Check(!embedded.empty() && embedded[0].program().size() > 100000,
          "embedded font program extracted");
    Check(counts[pdfv1::PDF_FAMILY_TEXT_CELLS] >= 1, "trailer counts cells");
  }

  // Font ids: a cell and its embedded program share one id even when the
  // font's /BaseFont differs from its /FontName, two subsets that share a
  // name but not a program stay apart, and programs go only to a call that
  // asks for EMBEDDED_FONTS.
  {
    const std::string fonts_pdf = ReadFile(fixture_dir + "/fonts.pdf");
    Check(!fonts_pdf.empty(), "fonts.pdf read");
    ParseResult full = ParseDocument(
        stub.get(), fonts_pdf,
        {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_FONTS,
         pdfv1::PDF_FAMILY_EMBEDDED_FONTS},
        std::nullopt);
    Check(full.status.ok(), "fonts.pdf Parse finished OK");
    const pdfv1::TextCell* first =
        full.pages.count(0) ? FindCell(full.pages[0], "First") : nullptr;
    const pdfv1::TextCell* second =
        full.pages.count(1) ? FindCell(full.pages[1], "Second") : nullptr;
    Check(first != nullptr && second != nullptr && first->has_font_id() &&
              second->has_font_id(),
          "fonts.pdf cells carry font ids");
    if (first != nullptr && second != nullptr) {
      Check(first->font_id() != second->font_id(),
            "two subsets sharing a name but not a program get two ids");
      std::map<uint32_t, int> programs;
      for (const auto& program : full.embedded) ++programs[program.font_id()];
      Check(full.embedded.size() == 2 && programs[first->font_id()] == 1 &&
                programs[second->font_id()] == 1,
            "each cell's font id names its own embedded program, once");
      for (uint32_t id : {first->font_id(), second->font_id()}) {
        const auto it = full.fonts.find(id);
        Check(it != full.fonts.end() &&
                  it->second.base_name() == "ABCDEF+SharedSans-Bold" &&
                  it->second.embedded(),
              "the font entry is the descriptor's name, marked embedded");
      }
      for (const auto& program : full.embedded) {
        Check(program.format() == pdfv1::FONT_PROGRAM_FORMAT_TRUETYPE &&
                  !program.program().empty(),
              "the program is the /FontFile2 TrueType bytes");
      }
    }

    ParseResult no_programs = ParseDocument(
        stub.get(), fonts_pdf,
        {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_FONTS}, std::nullopt);
    Check(no_programs.status.ok() && no_programs.embedded.empty(),
          "FONTS without EMBEDDED_FONTS sends no program");
    const pdfv1::TextCell* cell = no_programs.pages.count(0)
                                      ? FindCell(no_programs.pages[0], "First")
                                      : nullptr;
    Check(cell != nullptr && cell->has_font_id() &&
              no_programs.fonts.count(cell->font_id()) == 1 &&
              no_programs.fonts[cell->font_id()].base_name() ==
                  "ABCDEF+SharedSans-Bold",
          "without programs a cell still resolves in the font table");

    ParseResult fonts_only = ParseDocument(
        stub.get(), fonts_pdf, {pdfv1::PDF_FAMILY_FONTS}, std::nullopt);
    Check(fonts_only.status.ok() && !fonts_only.fonts.empty() &&
              fonts_only.embedded.empty(),
          "a FONTS-only call lists the fonts, without cells or programs");
    // embedded says whether the document embeds the program, read from the
    // descriptor, whether or not the call asked for the programs.
    for (const auto& [id, font] : fonts_only.fonts) {
      Check(font.embedded(), "a FONTS-only call marks " + font.base_name() +
                                 " embedded");
    }
    for (const auto& [id, font] : no_programs.fonts) {
      Check(font.embedded(), "a call without programs marks " +
                                 font.base_name() + " embedded");
    }

    // rich.pdf, the way gRParse asks: cells and the font table, no programs.
    ParseResult rich_fonts = ParseDocument(
        stub.get(), rich,
        {pdfv1::PDF_FAMILY_TEXT_CELLS, pdfv1::PDF_FAMILY_FONTS}, std::nullopt);
    Check(rich_fonts.status.ok() && rich_fonts.embedded.empty(),
          "rich.pdf with FONTS only sends no program");
    const pdfv1::TextCell* embedded_cell =
        rich_fonts.pages.count(0) ? FindCell(rich_fonts.pages[0], "Embedded")
                                  : nullptr;
    Check(embedded_cell != nullptr &&
              rich_fonts.fonts.count(embedded_cell->font_id()) == 1 &&
              rich_fonts.fonts[embedded_cell->font_id()].base_name() ==
                  "UbuntuMono" &&
              rich_fonts.fonts[embedded_cell->font_id()].embedded(),
          "rich.pdf's embedded-font cell resolves to UbuntuMono, embedded");
    const pdfv1::TextCell* standard_cell =
        rich_fonts.pages.count(0) ? FindCell(rich_fonts.pages[0], "Tagged")
                                  : nullptr;
    Check(standard_cell != nullptr &&
              rich_fonts.fonts.count(standard_cell->font_id()) == 1 &&
              !rich_fonts.fonts[standard_cell->font_id()].embedded(),
          "rich.pdf's standard 14 font is not marked embedded");
    ParseResult rich_fonts_only = ParseDocument(
        stub.get(), rich, {pdfv1::PDF_FAMILY_FONTS}, std::nullopt);
    bool ubuntu_embedded = false;
    for (const auto& [id, font] : rich_fonts_only.fonts) {
      if (font.base_name() == "UbuntuMono") ubuntu_embedded = font.embedded();
    }
    Check(rich_fonts_only.status.ok() && ubuntu_embedded,
          "a FONTS-only call on rich.pdf marks UbuntuMono embedded");
  }

  // A link whose action is not a URI is not emitted: the engine leaves its
  // uri empty and does not resolve destinations.
  {
    const std::string links = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>",
         "<< /Type /Page /Parent 2 0 R /Annots [4 0 R 5 0 R] >>",
         "<< /Type /Annot /Subtype /Link /Rect [72 700 200 720] "
         "/A << /S /URI /URI (https://example.com/kept) >> >>",
         "<< /Type /Annot /Subtype /Link /Rect [72 600 200 620] "
         "/A << /S /GoTo /D [3 0 R /Fit] >> >>"});
    ParseResult parsed = ParseDocument(
        stub.get(), links, {pdfv1::PDF_FAMILY_HYPERLINKS}, std::nullopt);
    Check(parsed.status.ok() && parsed.pages.count(0) == 1 &&
              parsed.pages[0].hyperlinks_size() == 1 &&
              parsed.pages[0].hyperlinks(0).uri() ==
                  "https://example.com/kept",
          "a GoTo link is not emitted as an empty URI");
    uint64_t counted = 0;
    if (parsed.trailer.has_value()) {
      for (const auto& count : parsed.trailer->counts()) {
        if (count.family() == pdfv1::PDF_FAMILY_HYPERLINKS) {
          counted = count.count();
        }
      }
    }
    Check(counted == 1, "the trailer counts only the links emitted");
  }

  // Render hello.pdf at 72 DPI.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(hello);
    request.set_dpi(72.0);
    request.set_pixel_format(pdfv1::PIXEL_FORMAT_RGBA8);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg), "render produced a raster");
    const pdfv1::PageRaster raster = msg.raster();
    Check(raster.width_px() >= 611 && raster.width_px() <= 613,
          "raster width near Letter at 72 DPI");
    Check(raster.height_px() >= 791 && raster.height_px() <= 793,
          "raster height near Letter at 72 DPI");
    Check(raster.pixel_format() == pdfv1::PIXEL_FORMAT_RGBA8,
          "raster is RGBA8");
    Check(raster.pixels().size() ==
              static_cast<size_t>(raster.stride_bytes()) * raster.height_px(),
          "raster payload matches stride * height");
    Check(!reader->Read(&msg), "one raster for the one page");
    Check(reader->Finish().ok(), "render finished OK");

    // Helvetica is not embedded, so the glyphs come from the bundled
    // fallback faces. Real glyphs leave dark ink inside the cell's box and
    // only there; a missing face draws a faint blue outline instead, and a
    // filled box would cover the whole cell.
    ParseResult parsed = ParseDocument(
        stub.get(), hello, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    const pdfv1::TextCell* cell =
        parsed.pages.count(0) ? FindCell(parsed.pages[0], "Hello") : nullptr;
    Check(cell != nullptr && parsed.header.pages_size() == 1,
          "hello.pdf cell for the glyph check");
    if (cell != nullptr && parsed.header.pages_size() == 1) {
      const auto box =
          ToDisplayPixels(parsed.header.pages(0), cell->bbox(), 72.0);
      const auto [inside, outside] = CountPixels(
          raster, {box[0] - 2, box[1] - 2, box[2] + 2, box[3] + 2}, IsInk);
      const double area = (box[2] - box[0]) * (box[3] - box[1]);
      Check(inside >= 150, "raster draws the glyphs of the text cell");
      Check(inside < 0.7 * area, "the glyphs are glyphs, not a filled box");
      Check(outside == 0, "no ink outside the text cell");
    }
  }

  // Render load failures are typed in a one-message head stream, never a
  // bare gRPC error.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data("not a pdf");
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_head(),
          "unloadable render answers with a head");
    Check(msg.head().load_status() == pdfv1::LOAD_STATUS_NOT_PDF,
          "head carries LOAD_STATUS_NOT_PDF");
    Check(!reader->Read(&msg), "head stream ends after one message");
    Check(reader->Finish().ok(), "unloadable render still finishes OK");
  }

  // Render takes only a positive finite DPI, and refuses a raster over the
  // pixel budget (or 65535 pixels on a side) before rendering anything.
  {
    for (double bad : {std::nan(""), std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(), 0.0, -72.0,
                       1e-300}) {
      grpc::Status status;
      std::vector<pdfv1::PageRaster> rasters = RenderDocument(
          stub.get(), hello, bad, pdfv1::PIXEL_FORMAT_RGBA8, std::nullopt,
          &status);
      Check(status.error_code() == grpc::INVALID_ARGUMENT && rasters.empty(),
            "dpi " + std::to_string(bad) + " is INVALID_ARGUMENT");
    }
    grpc::Status status;
    std::vector<pdfv1::PageRaster> rasters = RenderDocument(
        stub.get(), hello, 1e6, pdfv1::PIXEL_FORMAT_RGBA8, std::nullopt,
        &status);
    Check(status.error_code() == grpc::RESOURCE_EXHAUSTED && rasters.empty(),
          "a raster wider than the rasterizer allows is RESOURCE_EXHAUSTED");

    // 14400 pt square, the largest page the spec allows: 207 megapixels at
    // 72 dpi, over the 2^27 default; at 4 dpi it is about 800 x 800 (the
    // rasterizer's float scale can round it up a pixel).
    const std::string huge = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 14400 14400] "
         "/Contents 4 0 R >>",
         Stream("0 0 1 rg 0 0 7200 7200 re f")});
    rasters = RenderDocument(stub.get(), huge, 72.0, pdfv1::PIXEL_FORMAT_RGBA8,
                             std::nullopt, &status);
    Check(status.error_code() == grpc::RESOURCE_EXHAUSTED && rasters.empty(),
          "a raster over the default pixel budget is RESOURCE_EXHAUSTED");
    rasters = RenderDocument(stub.get(), huge, 4.0, pdfv1::PIXEL_FORMAT_RGBA8,
                             std::nullopt, &status);
    Check(status.ok() && rasters.size() == 1 &&
              rasters[0].width_px() >= 800 && rasters[0].width_px() <= 801 &&
              rasters[0].height_px() == rasters[0].width_px(),
          "the same page renders at a DPI within the budget");

    // A configured budget applies to every page of the range before the
    // first one renders: page 0 fits, page 1 does not, nothing streams and
    // nothing is decoded.
    grpc_qparse::RenderLimits limits;
    limits.max_pixels = 100000;
    grpc_qparse::QparseServiceImpl small_service(
        grpc_qparse::DocumentCacheConfig{}, limits);
    int small_port = 0;
    std::unique_ptr<grpc::Server> small_server =
        StartServer(&small_service, &small_port);
    auto small_stub = Dial(small_port);
    const std::string mixed = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>",
         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>"});
    rasters = RenderDocument(small_stub.get(), mixed, 36.0,
                             pdfv1::PIXEL_FORMAT_RGBA8, std::nullopt, &status);
    Check(status.error_code() == grpc::RESOURCE_EXHAUSTED && rasters.empty(),
          "one page over a configured budget fails the whole call up front");
    Check(small_service.decoded_pages() == 0,
          "a refused Render decodes nothing");
    rasters = RenderDocument(small_stub.get(), mixed, 36.0,
                             pdfv1::PIXEL_FORMAT_RGBA8, std::make_pair(0u, 1u),
                             &status);
    Check(status.ok() && rasters.size() == 1,
          "the page within the budget renders on its own");

    // An image with more pixels than the budget is left out of the page
    // before its samples are decoded, whatever the page's own size: page 0
    // draws a 400 x 400 black image (160000 pixels, over the budget) and
    // page 1 a 200 x 200 one (within it), each over its whole 100 pt page.
    // Page 2's image declares its 400 x 400 as reals, which the engine
    // takes as well, so it counts the same.
    auto image_page = [](int side, const std::string& suffix = "") {
      const std::string size = std::to_string(side) + suffix;
      return "<< /Type /XObject /Subtype /Image /Width " + size +
             " /Height " + size +
             " /ColorSpace /DeviceGray /BitsPerComponent 8 /Length " +
             std::to_string(side * side) + " >>\nstream\n" +
             std::string(static_cast<size_t>(side * side), '\0') +
             "\nendstream";
    };
    const std::string images = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R 4 0 R 8 0 R] /Count 3 "
         "/MediaBox [0 0 100 100] >>",
         "<< /Type /Page /Parent 2 0 R /Contents 5 0 R "
         "/Resources << /XObject << /Im0 6 0 R >> >> >>",
         "<< /Type /Page /Parent 2 0 R /Contents 5 0 R "
         "/Resources << /XObject << /Im0 7 0 R >> >> >>",
         Stream("q 100 0 0 100 0 0 cm /Im0 Do Q"), image_page(400),
         image_page(200),
         "<< /Type /Page /Parent 2 0 R /Contents 5 0 R "
         "/Resources << /XObject << /Im0 9 0 R >> >> >>",
         image_page(400, ".0")});
    rasters = RenderDocument(small_stub.get(), images, 36.0,
                             pdfv1::PIXEL_FORMAT_GRAY8, std::nullopt, &status);
    Check(status.ok() && rasters.size() == 3,
          "a page with an image over the budget still renders");
    if (rasters.size() == 3) {
      auto center = [](const pdfv1::PageRaster& raster) {
        return static_cast<unsigned char>(
            raster.pixels()[(raster.height_px() / 2) * raster.stride_bytes() +
                            raster.width_px() / 2]);
      };
      Check(center(rasters[0]) > 200,
            "the image over the budget is left out, the page stays white");
      Check(center(rasters[1]) < 50, "the image within the budget is drawn");
      Check(center(rasters[2]) > 200,
            "an image over the budget in real numbers is left out too");
    }
    small_server->Shutdown();
    Check(grpc_qparse::RenderLimits{}.max_pixels ==
              grpc_qparse::kMaxRenderPixels,
          "the default budget is the most one message carries");
  }

  // bomb.pdf: two images deflated twice, a few kilobytes that inflate to
  // hundreds of megabytes of black. Page 0's honestly declares 12000 x
  // 12000, over the default pixel budget, and is left out before a sample
  // is decoded; page 1's declares 64 x 64 but inflates to 256 MiB, and the
  // decoded stream limit stops it. Both pages render, white where the
  // images would be, and Parse, which never decodes samples, still lists
  // both.
  {
    const std::string bomb = ReadFile(fixture_dir + "/bomb.pdf");
    Check(!bomb.empty() && bomb.size() < 8192, "bomb.pdf read, and tiny");
    ErrorLog errors;
    loguru::add_callback("bomb", &ErrorLog::Collect, &errors,
                         loguru::Verbosity_ERROR);
    grpc::Status status;
    std::vector<pdfv1::PageRaster> rasters = RenderDocument(
        stub.get(), bomb, 36.0, pdfv1::PIXEL_FORMAT_GRAY8, std::nullopt,
        &status);
    loguru::remove_callback("bomb");
    Check(status.ok() && rasters.size() == 2, "bomb.pdf renders both pages");
    for (const auto& raster : rasters) {
      // The square is 100..300 by 400..600 pt, (100, 146) px at 36 dpi.
      const size_t center = 146 * size_t{raster.stride_bytes()} + 100;
      Check(raster.pixels().size() > center &&
                static_cast<unsigned char>(raster.pixels()[center]) > 200,
            "bomb.pdf page " + std::to_string(raster.page_index()) +
                ": the image is not drawn");
    }
    Check(errors.Contains("page 0: image /Im0 is left out"),
          "the log names the image left out of page 0");
    ParseResult parsed = ParseDocument(
        stub.get(), bomb, {pdfv1::PDF_FAMILY_PLACED_IMAGES}, std::nullopt);
    Check(parsed.status.ok() && parsed.pages.size() == 2 &&
              parsed.pages[0].images_size() == 1 &&
              parsed.pages[0].images(0).source_width_px() == 12000 &&
              parsed.pages[1].images_size() == 1 &&
              parsed.pages[1].images(0).source_width_px() == 64,
          "Parse still places both bomb.pdf images");
  }

  // lzw.pdf: page 0's content stream and page 1's image samples are LZW
  // bombs, a few kilobytes of codes that decode to 260 MiB of zeros under
  // a Flate layer that inflates to far less than the limit. The decoded
  // stream limit stops the LZW stage: Parse returns, and Render leaves the
  // image out, so page 1's square stays white. Pages 2 and 3 are honest
  // LZW text, the second with /EarlyChange 0 and a PNG predictor, and
  // still decode.
  {
    const std::string lzw = ReadFile(fixture_dir + "/lzw.pdf");
    Check(!lzw.empty() && lzw.size() < 65536, "lzw.pdf read, and small");
    ParseResult parsed = ParseDocument(
        stub.get(), lzw, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    Check(parsed.status.ok() && parsed.pages.size() == 4,
          "Parse of lzw.pdf returns every page");
    Check(parsed.pages[0].text_cells_size() == 0,
          "the LZW bomb content stream yields no cells");
    for (uint32_t page : {2u, 3u}) {
      Check(FindCell(parsed.pages[page], "LZW text") != nullptr,
            "lzw.pdf page " + std::to_string(page) + " decodes its LZW text");
    }
    grpc::Status status;
    std::vector<pdfv1::PageRaster> rasters = RenderDocument(
        stub.get(), lzw, 36.0, pdfv1::PIXEL_FORMAT_GRAY8,
        std::make_pair(1u, 2u), &status);
    Check(status.ok() && rasters.size() == 1,
          "lzw.pdf renders the page with the LZW bomb image");
    if (rasters.size() == 1) {
      // The square is 100..300 by 400..600 pt, (100, 146) px at 36 dpi.
      const size_t center = 146 * size_t{rasters[0].stride_bytes()} + 100;
      Check(rasters[0].pixels().size() > center &&
                static_cast<unsigned char>(rasters[0].pixels()[center]) > 200,
            "the LZW bomb image is not drawn");
    }
  }

  // The decoded stream limit comes from the environment; unset, zero or
  // unparseable keeps the default.
  {
    setenv("GRPC_QPARSE_MAX_DECODED_STREAM_BYTES", "1048576", 1);
    Check(grpc_qparse::DecodedStreamLimitFromEnv() == 1048576,
          "GRPC_QPARSE_MAX_DECODED_STREAM_BYTES sets the limit");
    for (const char* bad : {"", "0", "lots", "12abc"}) {
      setenv("GRPC_QPARSE_MAX_DECODED_STREAM_BYTES", bad, 1);
      Check(grpc_qparse::DecodedStreamLimitFromEnv() ==
                grpc_qparse::kMaxDecodedStreamBytes,
            std::string("GRPC_QPARSE_MAX_DECODED_STREAM_BYTES=\"") + bad +
                "\" keeps the default");
    }
    unsetenv("GRPC_QPARSE_MAX_DECODED_STREAM_BYTES");
    Check(grpc_qparse::DecodedStreamLimitFromEnv() ==
              grpc_qparse::kMaxDecodedStreamBytes,
          "the decoded stream limit defaults to 512 MiB");
  }

  // Render produces the requested pixel layout: frames.pdf page 0 at 72 dpi
  // has its red rectangle around pixel (330, 277).
  {
    const std::string frames_pdf = ReadFile(fixture_dir + "/frames.pdf");
    struct Layout {
      pdfv1::PixelFormat requested;
      pdfv1::PixelFormat produced;
      uint32_t channels;
      std::vector<int> red;
    };
    for (const Layout& want :
         {Layout{pdfv1::PIXEL_FORMAT_RGBA8, pdfv1::PIXEL_FORMAT_RGBA8, 4,
                 {255, 0, 0, 255}},
          Layout{pdfv1::PIXEL_FORMAT_UNSPECIFIED, pdfv1::PIXEL_FORMAT_RGBA8, 4,
                 {255, 0, 0, 255}},
          Layout{pdfv1::PIXEL_FORMAT_BGRA8, pdfv1::PIXEL_FORMAT_BGRA8, 4,
                 {0, 0, 255, 255}},
          Layout{pdfv1::PIXEL_FORMAT_RGB8, pdfv1::PIXEL_FORMAT_RGB8, 3,
                 {255, 0, 0}},
          Layout{pdfv1::PIXEL_FORMAT_BGR8, pdfv1::PIXEL_FORMAT_BGR8, 3,
                 {0, 0, 255}},
          Layout{pdfv1::PIXEL_FORMAT_GRAY8, pdfv1::PIXEL_FORMAT_GRAY8, 1,
                 {76}}}) {
      const std::string name =
          std::string(pdfv1::PixelFormat_Name(want.requested)) + ": ";
      grpc::Status status;
      std::vector<pdfv1::PageRaster> rasters =
          RenderDocument(stub.get(), frames_pdf, 72.0, want.requested,
                         std::make_pair(0u, 1u), &status);
      Check(status.ok() && rasters.size() == 1, name + "one raster");
      if (rasters.size() != 1) continue;
      const pdfv1::PageRaster& raster = rasters[0];
      Check(raster.pixel_format() == want.produced,
            name + "the layout produced is reported");
      Check(raster.stride_bytes() == raster.width_px() * want.channels &&
                raster.pixels().size() ==
                    static_cast<size_t>(raster.stride_bytes()) *
                        raster.height_px(),
            name + "stride and payload follow the layout");
      if (raster.pixels().size() <
          static_cast<size_t>(raster.stride_bytes()) * 278) {
        continue;
      }
      const auto* pixel = reinterpret_cast<const unsigned char*>(
                              raster.pixels().data()) +
                          277 * raster.stride_bytes() + 330 * want.channels;
      bool matches = true;
      for (uint32_t c = 0; c < want.channels; ++c) {
        if (std::abs(pixel[c] - want.red[c]) > 2) matches = false;
      }
      Check(matches, name + "the red rectangle comes out red");
    }
  }

  // Content-addressed handshake (PdfDocument.sha256).
  const std::string hello_hash = grpc_qparse::Sha256Hex(hello);
  const std::string rich_hash = grpc_qparse::Sha256Hex(rich);

  // A first call addressed by hash misses with BYTES_REQUIRED on all three
  // surfaces, never as a gRPC error.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "hash Probe RPC OK");
    Check(response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "cold hash Probe misses with BYTES_REQUIRED");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    Check(reader->Read(&msg) && msg.has_header(),
          "cold hash Parse answers with a header");
    Check(msg.header().capabilities().load_status() ==
              pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "cold hash Parse header carries BYTES_REQUIRED");
    Check(!reader->Read(&msg), "cold hash Parse stream ends after the header");
    Check(reader->Finish().ok(), "cold hash Parse finishes OK");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_head(),
          "cold hash Render answers with a head");
    Check(msg.head().load_status() == pdfv1::LOAD_STATUS_BYTES_REQUIRED,
          "cold hash Render head carries BYTES_REQUIRED");
    Check(!reader->Read(&msg), "cold hash Render stream ends after the head");
    Check(reader->Finish().ok(), "cold hash Render finishes OK");
  }

  // The one retry with bytes and hash succeeds and caches the document.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "upload Probe RPC OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "upload Probe loads");
  }

  // Now the hash alone addresses the document, no bytes on the wire.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "cached Probe RPC OK");
    Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "cached Probe hits");
    Check(response.capabilities().page_count() == 1,
          "cached Probe sees the one page");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    Check(reader->Read(&msg) && msg.has_header(), "cached Parse header first");
    Check(msg.header().capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
          "cached Parse loads");
    Check(msg.header().pages_size() == 1, "cached Parse lists the page");
    bool saw_trailer = false;
    while (reader->Read(&msg)) {
      if (msg.has_trailer()) saw_trailer = true;
    }
    Check(reader->Finish().ok(), "cached Parse finishes OK");
    Check(saw_trailer, "cached Parse reaches the trailer");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_sha256(hello_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_raster(), "cached Render rasterizes");
    Check(reader->Finish().ok(), "cached Render finishes OK");
  }

  // Bytes that do not hash to the given sha256 are a typed verdict.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(rich_hash);
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).ok(), "mismatch Probe RPC OK");
    Check(response.capabilities().load_status() ==
              pdfv1::LOAD_STATUS_HASH_MISMATCH,
          "mismatch Probe reports HASH_MISMATCH");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data(hello);
    request.mutable_document()->set_sha256(rich_hash);
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(reader->Read(&msg) && msg.has_head(),
          "mismatch Render answers with a head");
    Check(msg.head().load_status() == pdfv1::LOAD_STATUS_HASH_MISMATCH,
          "mismatch Render head carries HASH_MISMATCH");
    Check(!reader->Read(&msg), "mismatch Render stream ends after the head");
    Check(reader->Finish().ok(), "mismatch Render finishes OK");
  }

  // Empty data with no sha256 is INVALID_ARGUMENT on every RPC.
  {
    grpc::ClientContext ctx;
    pdfv1::ProbeRequest request;
    pdfv1::ProbeResponse response;
    Check(stub->Probe(&ctx, request, &response).error_code() ==
              grpc::INVALID_ARGUMENT,
          "empty Probe is INVALID_ARGUMENT");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::ParseRequest request;
    auto reader = stub->Parse(&ctx, request);
    pdfv1::ParseResponse msg;
    Check(!reader->Read(&msg), "empty Parse streams nothing");
    Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
          "empty Parse is INVALID_ARGUMENT");
  }
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(!reader->Read(&msg), "empty Render streams nothing");
    Check(reader->Finish().error_code() == grpc::INVALID_ARGUMENT,
          "empty Render is INVALID_ARGUMENT");
  }

  // Eviction under a tiny capacity: a one-document cache forgets hello once
  // rich is cached.
  {
    grpc_qparse::DocumentCacheConfig tiny_config;
    tiny_config.max_documents = 1;
    grpc_qparse::QparseServiceImpl tiny_service(tiny_config);
    grpc::ServerBuilder tiny_builder;
    int tiny_port = 0;
    tiny_builder.AddListeningPort("127.0.0.1:0",
                                  grpc::InsecureServerCredentials(),
                                  &tiny_port);
    tiny_builder.RegisterService(&tiny_service);
    std::unique_ptr<grpc::Server> tiny_server = tiny_builder.BuildAndStart();
    Check(tiny_server != nullptr && tiny_port != 0, "tiny-cache server up");
    auto tiny_stub = pdfv1::PdfBackendService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(tiny_port),
                            grpc::InsecureChannelCredentials()));
    for (const auto* doc : {&hello, &rich}) {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(*doc);
      request.mutable_document()->set_sha256(grpc_qparse::Sha256Hex(*doc));
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok(),
            "tiny-cache upload Probe RPC OK");
      Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
            "tiny-cache upload loads");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(hello_hash);
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok(),
            "evicted Probe RPC OK");
      Check(response.capabilities().load_status() ==
                pdfv1::LOAD_STATUS_BYTES_REQUIRED,
            "evicted document misses with BYTES_REQUIRED");
    }
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_sha256(rich_hash);
      pdfv1::ProbeResponse response;
      Check(tiny_stub->Probe(&ctx, request, &response).ok(),
            "surviving Probe RPC OK");
      Check(response.capabilities().load_status() == pdfv1::LOAD_STATUS_OK,
            "most recent document survives eviction");
    }
    tiny_server->Shutdown();
  }

  // frames.pdf: the same content on eight differently framed pages. Every
  // family comes out unrotated and relative to the CropBox, PageInfo says
  // so in page_space and carries the true (inherited) /Rotate and the
  // stored boxes, and the raster is the CropBox
  // turned by /Rotate, so the client's page mapping lands each box on its
  // ink.
  const std::string frames = ReadFile(fixture_dir + "/frames.pdf");
  Check(!frames.empty(), "frames.pdf read");
  struct FramedPage {
    int rotation;
    double width;
    double height;
    std::array<double, 4> media;
    std::array<double, 4> crop;
    // Where "Frame" starts, its baseline, and the red rectangle, in user
    // space as the content stream draws them.
    double text_x;
    double baseline;
    std::array<double, 4> red;
  };
  const std::array<double, 4> letter = {0, 0, 612, 792};
  const std::array<double, 4> red = {300, 500, 360, 530};
  const FramedPage framed[8] = {
      {0, 612, 792, letter, letter, 72, 700, red},
      {90, 792, 612, letter, letter, 72, 700, red},
      {180, 612, 792, letter, letter, 72, 700, red},
      {270, 792, 612, letter, letter, 72, 700, red},
      {90, 792, 612, letter, letter, 72, 700, red},
      {0, 540, 720, letter, {36, 36, 576, 756}, 72, 700, red},
      {0, 612, 792, {100, 200, 712, 992}, {100, 200, 712, 992}, 172, 900,
       {400, 700, 460, 730}},
      {90, 672, 512, letter, {50, 60, 562, 732}, 72, 650,
       {300, 400, 360, 430}},
  };
  {
    ParseResult parsed = ParseDocument(stub.get(), frames, {}, std::nullopt);
    Check(parsed.status.ok(), "frames Parse finished OK");
    Check(parsed.header.pages_size() == 8, "frames inventory lists every page");
    for (int i = 0; i < 8 && i < parsed.header.pages_size(); ++i) {
      const std::string page = "frames page " + std::to_string(i) + ": ";
      const auto& info = parsed.header.pages(i);
      const FramedPage& want = framed[i];
      Check(info.rotation_degrees() == want.rotation,
            page + "rotation_degrees is the page's own or inherited /Rotate");
      Check(Near(info.width_pts(), want.width, 0.01) &&
                Near(info.height_pts(), want.height, 0.01),
            page + "size is the CropBox after /Rotate");
      Check(BoxNear(info.media_box(), want.media, 0.01),
            page + "media_box as stored, unrotated");
      Check(BoxNear(info.crop_box(), want.crop, 0.01),
            page + "crop_box as stored, unrotated");
      Check(info.page_space() == pdfv1::PAGE_SPACE_CROP_BOX,
            page + "page_space names the CropBox frame");
      // Where the content stream draws, moved by the CropBox origin.
      const double text_x = want.text_x - want.crop[0];
      const double baseline = want.baseline - want.crop[1];

      const auto it = parsed.pages.find(static_cast<uint32_t>(i));
      if (it == parsed.pages.end()) {
        Check(false, page + "page chunk streamed");
        continue;
      }
      const pdfv1::TextCell* cell = FindCell(it->second, "Frame");
      Check(cell != nullptr, page + "\"Frame\" cell present");
      if (cell != nullptr) {
        Check(Near(cell->bbox().x0(), text_x, 1.5),
              page + "cell starts where the text is drawn");
        Check(cell->bbox().y0() <= baseline + 0.5 &&
                  cell->bbox().y0() >= baseline - 8,
              page + "cell bottom at the baseline");
        Check(cell->bbox().y1() >= baseline + 12 &&
                  cell->bbox().y1() <= baseline + 25,
              page + "cell top an em above the baseline");
        Check(Near(cell->quad().x0(), cell->bbox().x0(), 1.5) &&
                  Near(cell->quad().y0(), cell->bbox().y0(), 1.5),
              page + "quad in the bbox's frame");
      }
      bool red_found = false;
      for (const auto& shape : it->second.shapes()) {
        if (BoxNear(shape.bbox(), CropRelative(info, want.red), 0.5)) {
          red_found = true;
        }
      }
      Check(red_found, page + "red rectangle where it is drawn");
    }
    // Pages 1 to 5 draw exactly what page 0 draws: rotating the page must
    // not move a cell, and cropping it moves the cell by exactly the
    // CropBox origin.
    const pdfv1::TextCell* upright =
        parsed.pages.count(0) ? FindCell(parsed.pages[0], "Frame") : nullptr;
    for (uint32_t i = 1; i <= 5 && upright != nullptr; ++i) {
      const pdfv1::TextCell* cell =
          parsed.pages.count(i) ? FindCell(parsed.pages[i], "Frame") : nullptr;
      const pdfv1::BoundingBox& u = upright->bbox();
      Check(cell != nullptr &&
                BoxNear(cell->bbox(),
                        CropRelative(parsed.header.pages(static_cast<int>(i)),
                                     {u.x0(), u.y0(), u.x1(), u.y1()}),
                        0.01),
            "frames page " + std::to_string(i) +
                ": the cell sits where the upright page has it, less the "
                "CropBox origin");
    }
    // Links and widgets share the cells' frame, CropBox or not. The rects
    // below are as stored, in user space.
    struct FramedLink {
      uint32_t page;
      std::string uri;
      std::array<double, 4> rect;
    };
    for (const FramedLink& want :
         {FramedLink{1, "https://example.com/rotated", {70, 695, 200, 725}},
          FramedLink{5, "https://example.com/cropped", {70, 695, 200, 725}},
          FramedLink{7, "https://example.com/both", {70, 645, 200, 675}}}) {
      const std::string page = "frames page " + std::to_string(want.page) + ": ";
      const pdfv1::PageChunk& chunk = parsed.pages[want.page];
      Check(chunk.hyperlinks_size() == 1, page + "one link");
      if (chunk.hyperlinks_size() != 1) continue;
      const auto& link = chunk.hyperlinks(0);
      Check(link.uri() == want.uri, page + "link target");
      const auto& info = parsed.header.pages(static_cast<int>(want.page));
      Check(BoxNear(link.bbox(), CropRelative(info, want.rect), 0.01),
            page + "link rect relative to the CropBox, corners normalized");
      const pdfv1::TextCell* cell = FindCell(chunk, "Frame");
      if (cell != nullptr) {
        const double cx = (cell->bbox().x0() + cell->bbox().x1()) / 2;
        const double cy = (cell->bbox().y0() + cell->bbox().y1()) / 2;
        Check(cx > link.bbox().x0() && cx < link.bbox().x1() &&
                  cy > link.bbox().y0() && cy < link.bbox().y1(),
              page + "the link covers the text it was drawn over");
      }
    }
    struct FramedField {
      uint32_t page;
      std::string name;
      std::array<double, 4> rect;
    };
    for (const FramedField& want :
         {FramedField{5, "cropped_field", {300, 300, 450, 320}},
          FramedField{7, "rotated_field", {300, 250, 450, 270}}}) {
      const pdfv1::PageChunk& chunk = parsed.pages[want.page];
      const std::string page = "frames page " + std::to_string(want.page) + ": ";
      Check(chunk.form_fields_size() == 1 &&
                chunk.form_fields(0).name() == want.name &&
                BoxNear(chunk.form_fields(0).rect(),
                        CropRelative(parsed.header.pages(
                                         static_cast<int>(want.page)),
                                     want.rect),
                        0.01),
            page + "widget rect relative to the CropBox");
    }
    const pdfv1::TextCell* edge =
        parsed.pages.count(5) ? FindCell(parsed.pages[5], "Edge") : nullptr;
    // "Edge" is drawn at x 20, left of the CropBox at x 36.
    Check(edge != nullptr && Near(edge->bbox().x0(), 20 - 36, 1.5),
          "frames page 5: a cell straddling the CropBox edge is kept, at a "
          "negative x");
    // Drawn at (120, 200, 170, 230) on a page whose CropBox starts at
    // (50, 60).
    Check(parsed.pages[7].images_size() == 1 &&
              BoxNear(parsed.pages[7].images(0).bbox(), {70, 140, 120, 170},
                      0.5),
          "frames page 7: image placed where its matrix puts it, relative "
          "to the CropBox");
  }
  // Render agrees with Parse: the client maps each page's red rectangle and
  // "Frame" cell through PageInfo onto the raster, the red ink is there and
  // nowhere else, and the glyphs are where the cell is.
  {
    ParseResult parsed = ParseDocument(
        stub.get(), frames, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    grpc::Status status;
    std::vector<pdfv1::PageRaster> rasters = RenderDocument(
        stub.get(), frames, 72.0, pdfv1::PIXEL_FORMAT_RGBA8, std::nullopt,
        &status);
    Check(status.ok() && rasters.size() == 8, "frames renders every page");
    for (const auto& raster : rasters) {
      const uint32_t i = raster.page_index();
      if (i >= 8 || static_cast<int>(i) >= parsed.header.pages_size()) continue;
      const std::string page = "frames raster " + std::to_string(i) + ": ";
      const auto& info = parsed.header.pages(static_cast<int>(i));
      Check(raster.width_px() ==
                    static_cast<uint32_t>(std::ceil(info.width_pts())) &&
                raster.height_px() ==
                    static_cast<uint32_t>(std::ceil(info.height_pts())),
            page + "raster is the displayed page");
      const std::array<double, 4> red_box = CropRelative(info, framed[i].red);
      pdfv1::BoundingBox box;
      box.set_x0(red_box[0]);
      box.set_y0(red_box[1]);
      box.set_x1(red_box[2]);
      box.set_y1(red_box[3]);
      const auto px = ToDisplayPixels(info, box, 72.0);
      const auto [inside, outside] = CountPixels(
          raster, {px[0] - 2, px[1] - 2, px[2] + 2, px[3] + 2}, IsRed);
      const double area = (px[2] - px[0]) * (px[3] - px[1]);
      Check(inside >= 0.8 * area,
            page + "red rectangle drawn where the client maps it");
      Check(outside == 0, page + "no red ink anywhere else");
      const auto chunk = parsed.pages.find(i);
      const pdfv1::TextCell* text = chunk != parsed.pages.end()
                                        ? FindCell(chunk->second, "Frame")
                                        : nullptr;
      Check(text != nullptr, page + "\"Frame\" cell for the glyph check");
      if (text != nullptr) {
        const auto glyphs = ToDisplayPixels(info, text->bbox(), 72.0);
        const auto ink = CountPixels(
            raster,
            {glyphs[0] - 2, glyphs[1] - 2, glyphs[2] + 2, glyphs[3] + 2},
            IsInk);
        Check(ink.first >= 100,
              page + "glyphs drawn where the client maps the cell");
      }
    }
  }

  // A landscape page as producers write one: the text runs up the page in
  // user space and /Rotate 90 turns it upright. The engine merges the line
  // whole in user space, its quad runs up the page, and the client's
  // mapping turns its box into a wide display box where the glyphs are.
  {
    const std::string landscape = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 90 "
         "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
         Stream("BT /F1 18 Tf 0 1 -1 0 300 100 Tm (Landscape line of text) Tj "
                "ET"),
         "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"});
    ParseResult parsed = ParseDocument(
        stub.get(), landscape, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    const pdfv1::TextCell* cell =
        parsed.pages.count(0) ? FindCell(parsed.pages[0], "Landscape") : nullptr;
    Check(parsed.status.ok() && cell != nullptr &&
              cell->text().find("Landscape line of text") != std::string::npos,
          "landscape text comes out as one whole line");
    if (cell != nullptr && parsed.header.pages_size() == 1) {
      Check(Near(cell->quad().x0(), cell->quad().x1(), 0.5) &&
                cell->quad().y1() - cell->quad().y0() > 100,
            "the landscape cell's quad runs up the page in user space");
      const auto box =
          ToDisplayPixels(parsed.header.pages(0), cell->bbox(), 72.0);
      Check(box[2] - box[0] > 5 * (box[3] - box[1]),
            "the client maps the landscape cell to a wide display box");
      grpc::Status status;
      std::vector<pdfv1::PageRaster> rasters = RenderDocument(
          stub.get(), landscape, 72.0, pdfv1::PIXEL_FORMAT_RGBA8,
          std::nullopt, &status);
      Check(status.ok() && rasters.size() == 1 &&
                rasters[0].width_px() == 792 && rasters[0].height_px() == 612,
            "the landscape page renders turned");
      if (rasters.size() == 1) {
        const auto ink = CountPixels(
            rasters[0], {box[0] - 2, box[1] - 2, box[2] + 2, box[3] + 2},
            IsInk);
        Check(ink.first >= 150 && ink.second == 0,
              "the landscape glyphs are where the client maps the cell");
      }
    }
  }

  // A page range decodes only its pages, and document-level families decode
  // none; the header still carries the whole inventory.
  {
    const uint64_t before = service.decoded_pages();
    ParseResult ranged = ParseDocument(
        stub.get(), frames, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::make_pair(2u, 3u));
    Check(ranged.status.ok(), "ranged Parse finished OK");
    Check(service.decoded_pages() - before == 1,
          "a one-page range decodes one page");
    Check(ranged.header.pages_size() == 8,
          "a ranged header still lists every page");
    Check(ranged.pages.size() == 1 && ranged.pages.count(2) == 1,
          "only the requested page streams");
    Check(ranged.trailer.has_value(), "ranged Parse reaches the trailer");

    const uint64_t before_outline = service.decoded_pages();
    ParseResult outline = ParseDocument(
        stub.get(), frames, {pdfv1::PDF_FAMILY_OUTLINE}, std::nullopt);
    Check(outline.status.ok(), "outline-only Parse finished OK");
    Check(service.decoded_pages() == before_outline,
          "document-level families decode no page");

    const uint64_t before_render = service.decoded_pages();
    grpc::Status status;
    std::vector<pdfv1::PageRaster> one = RenderDocument(
        stub.get(), frames, 36.0, pdfv1::PIXEL_FORMAT_RGBA8,
        std::make_pair(6u, 7u), &status);
    Check(status.ok() && one.size() == 1 && one[0].page_index() == 6,
          "a one-page Render range renders that page");
    Check(service.decoded_pages() - before_render == 1,
          "a one-page Render range decodes one page");
  }

  // PageRange is zero-based and half-open, and a set range needs end
  // greater than begin; anything else is INVALID_ARGUMENT before a message
  // streams. Every other uint32 range is valid and is clamped to the
  // document: an end past it stops at the last page, and a begin past it
  // selects no page, 2^31 and up included (those used to narrow to a
  // negative int).
  {
    struct BadRange {
      uint32_t begin;
      uint32_t end;
      const char* what;
    };
    for (const BadRange& bad :
         {BadRange{5u, 3u, "end below begin"},
          BadRange{1u, 1u, "end equal to begin"},
          BadRange{4294967295u, 1u, "begin 2^32-1, end 1"}}) {
      ParseResult parsed =
          ParseDocument(stub.get(), frames, {pdfv1::PDF_FAMILY_TEXT_CELLS},
                        std::make_pair(bad.begin, bad.end));
      Check(parsed.status.error_code() == grpc::INVALID_ARGUMENT &&
                parsed.header.pages_size() == 0 && parsed.pages.empty(),
            std::string("Parse range ") + bad.what + " is INVALID_ARGUMENT");
      grpc::Status status;
      std::vector<pdfv1::PageRaster> rasters = RenderDocument(
          stub.get(), frames, 9.0, pdfv1::PIXEL_FORMAT_RGBA8,
          std::make_pair(bad.begin, bad.end), &status);
      Check(status.error_code() == grpc::INVALID_ARGUMENT && rasters.empty(),
            std::string("Render range ") + bad.what + " is INVALID_ARGUMENT");
    }
    struct GoodRange {
      uint32_t begin;
      uint32_t end;
      std::vector<uint32_t> pages;
      const char* what;
    };
    for (const GoodRange& good :
         {GoodRange{0u, 4294967295u, {0, 1, 2, 3, 4, 5, 6, 7}, "0 to 2^32-1"},
          GoodRange{6u, 100u, {6, 7}, "an end past the last page"},
          GoodRange{9u, 12u, {}, "past the last page"},
          GoodRange{2147483648u, 2147483649u, {}, "begin 2^31"},
          GoodRange{2147483648u, 4294967295u, {}, "begin 2^31, end 2^32-1"}}) {
      const std::string range = std::string("range ") + good.what + ": ";
      const uint64_t before = service.decoded_pages();
      ParseResult parsed =
          ParseDocument(stub.get(), frames, {pdfv1::PDF_FAMILY_TEXT_CELLS},
                        std::make_pair(good.begin, good.end));
      std::vector<uint32_t> streamed;
      for (const auto& [index, chunk] : parsed.pages) streamed.push_back(index);
      Check(parsed.status.ok() && parsed.header.pages_size() == 8 &&
                parsed.trailer.has_value() &&
                parsed.trailer->warnings_size() == 0,
            range + "Parse finishes OK with no warning");
      Check(streamed == good.pages, range + "Parse streams exactly its pages");
      grpc::Status status;
      std::vector<pdfv1::PageRaster> rasters = RenderDocument(
          stub.get(), frames, 9.0, pdfv1::PIXEL_FORMAT_RGBA8,
          std::make_pair(good.begin, good.end), &status);
      std::vector<uint32_t> rendered;
      for (const auto& raster : rasters) rendered.push_back(raster.page_index());
      Check(status.ok() && rendered == good.pages,
            range + "Render rasters exactly its pages");
      Check(service.decoded_pages() - before == 2 * good.pages.size(),
            range + "only the selected pages are decoded");
    }
  }

  // A page the engine cannot decode (an operator short of operands throws
  // in the engine) still has its PageInfo, and the trailer says why its
  // chunk is missing.
  {
    const std::string broken = AssemblePdf(
        {"<< /Type /Catalog /Pages 2 0 R >>",
         "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 612 792] "
         "/Resources << /Font << /F1 7 0 R >> >> >>",
         "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>",
         "<< /Type /Page /Parent 2 0 R /Contents 6 0 R /Rotate 90 >>",
         Stream("BT /F1 12 Tf 72 700 Td (Fine) Tj ET"),
         Stream("0 0 rg 72 700 100 20 re f"),
         "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"});
    ParseResult parsed = ParseDocument(stub.get(), broken, {}, std::nullopt);
    Check(parsed.status.ok(), "Parse with an undecodable page finished OK");
    Check(parsed.header.pages_size() == 2 &&
              parsed.header.pages(1).rotation_degrees() == 90 &&
              Near(parsed.header.pages(1).width_pts(), 792, 0.01),
          "the undecodable page is still in the inventory");
    Check(parsed.pages.count(0) == 1 &&
              FindCell(parsed.pages[0], "Fine") != nullptr,
          "the good page still streams");
    Check(parsed.pages.count(1) == 0, "the undecodable page has no chunk");
    bool warned = false;
    if (parsed.trailer.has_value()) {
      for (const auto& warning : parsed.trailer->warnings()) {
        if (warning.has_page_index() && warning.page_index() == 1 &&
            !warning.message().empty()) {
          warned = true;
        }
      }
    }
    Check(warned, "the trailer names the skipped page");

    // Render has no per-page warning in the contract: the page is left out
    // of the stream and the log says which page and why.
    ErrorLog errors;
    loguru::add_callback("render-skips", &ErrorLog::Collect, &errors,
                         loguru::Verbosity_ERROR);
    grpc::Status status;
    std::vector<pdfv1::PageRaster> rasters = RenderDocument(
        stub.get(), broken, 36.0, pdfv1::PIXEL_FORMAT_RGBA8, std::nullopt,
        &status);
    loguru::remove_callback("render-skips");
    Check(status.ok() && rasters.size() == 1 && rasters[0].page_index() == 0,
          "Render streams the good page without the undecodable one");
    Check(errors.Contains("Render skips page 1:"),
          "the log names the page Render skipped");
  }

  // A cancelled call stops decoding: Parse sends its header before any page
  // is decoded, and both RPCs check the call between pages.
  {
    const std::string many = ManyPagePdf(400);
    for (bool render : {false, true}) {
      grpc_qparse::QparseServiceImpl cancel_service(
          grpc_qparse::DocumentCacheConfig{});
      int cancel_port = 0;
      std::unique_ptr<grpc::Server> cancel_server =
          StartServer(&cancel_service, &cancel_port);
      auto cancel_stub = Dial(cancel_port);
      grpc::ClientContext ctx;
      bool first = false;
      if (render) {
        pdfv1::RenderRequest request;
        request.mutable_document()->set_data(many);
        request.set_dpi(36.0);
        auto reader = cancel_stub->Render(&ctx, request);
        pdfv1::RenderResponse msg;
        first = reader->Read(&msg) && msg.has_raster();
        ctx.TryCancel();
        while (reader->Read(&msg)) {
        }
        reader->Finish();
      } else {
        pdfv1::ParseRequest request;
        request.mutable_document()->set_data(many);
        request.add_families(pdfv1::PDF_FAMILY_TEXT_CELLS);
        auto reader = cancel_stub->Parse(&ctx, request);
        pdfv1::ParseResponse msg;
        first = reader->Read(&msg) && msg.has_header();
        ctx.TryCancel();
        while (reader->Read(&msg)) {
        }
        reader->Finish();
      }
      // Shutdown waits for the handler to return.
      cancel_server->Shutdown();
      Check(first, render ? "cancelled Render sent its first raster"
                          : "cancelled Parse sent its header");
      Check(cancel_service.decoded_pages() < 200,
            render ? "a cancelled Render stops decoding"
                   : "a cancelled Parse stops decoding");
    }
  }

  // Concurrent Parse and Render calls are capped. With one slot, a Render
  // that is still streaming holds it: a Parse waits, then fails
  // RESOURCE_EXHAUSTED once the queue timeout passes, while Probe is not
  // held back. A Parse already waiting gets the slot once the Render ends.
  {
    grpc_qparse::CallLimits call_limits;
    call_limits.max_concurrent = 1;
    call_limits.queue_timeout = std::chrono::seconds(1);
    grpc_qparse::QparseServiceImpl capped_service(
        grpc_qparse::DocumentCacheConfig{}, grpc_qparse::RenderLimits{},
        call_limits);
    int capped_port = 0;
    std::unique_ptr<grpc::Server> capped_server =
        StartServer(&capped_service, &capped_port);
    auto capped_stub = Dial(capped_port);

    grpc::ClientContext holder_ctx;
    pdfv1::RenderRequest holder_request;
    holder_request.mutable_document()->set_data(ManyPagePdf(400));
    holder_request.set_dpi(36.0);
    auto holder = capped_stub->Render(&holder_ctx, holder_request);
    pdfv1::RenderResponse holder_msg;
    Check(holder->Read(&holder_msg) && holder_msg.has_raster(),
          "the Render holding the only slot streams");

    const auto started = std::chrono::steady_clock::now();
    ParseResult refused = ParseDocument(
        capped_stub.get(), hello, {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    Check(refused.status.error_code() == grpc::RESOURCE_EXHAUSTED,
          "a Parse with no free slot fails RESOURCE_EXHAUSTED");
    Check(std::chrono::steady_clock::now() - started >=
              std::chrono::milliseconds(900),
          "the refused Parse waited out the queue timeout first");
    {
      grpc::ClientContext ctx;
      pdfv1::ProbeRequest request;
      request.mutable_document()->set_data(hello);
      pdfv1::ProbeResponse response;
      Check(capped_stub->Probe(&ctx, request, &response).ok(),
            "Probe is not held back by the cap");
    }

    ParseResult admitted;
    std::thread waiter([&] {
      admitted = ParseDocument(capped_stub.get(), hello,
                               {pdfv1::PDF_FAMILY_TEXT_CELLS}, std::nullopt);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    holder_ctx.TryCancel();
    while (holder->Read(&holder_msg)) {
    }
    holder->Finish();
    waiter.join();
    Check(admitted.status.ok() && admitted.pages.size() == 1,
          "a waiting Parse runs once the slot frees");
    capped_server->Shutdown();

    setenv("GRPC_QPARSE_MAX_CONCURRENT_CALLS", "3", 1);
    setenv("GRPC_QPARSE_QUEUE_TIMEOUT_S", "0", 1);
    grpc_qparse::CallLimits from_env = grpc_qparse::CallLimitsFromEnv();
    Check(from_env.max_concurrent == 3 && from_env.queue_timeout.count() == 0,
          "GRPC_QPARSE_MAX_CONCURRENT_CALLS and GRPC_QPARSE_QUEUE_TIMEOUT_S "
          "set the call bounds");
    for (const char* bad : {"", "0", "lots", "-1"}) {
      setenv("GRPC_QPARSE_MAX_CONCURRENT_CALLS", bad, 1);
      Check(grpc_qparse::CallLimitsFromEnv().max_concurrent ==
                grpc_qparse::DefaultMaxConcurrentCalls(),
            std::string("GRPC_QPARSE_MAX_CONCURRENT_CALLS=\"") + bad +
                "\" keeps the default");
    }
    setenv("GRPC_QPARSE_QUEUE_TIMEOUT_S", "604801", 1);
    Check(grpc_qparse::CallLimitsFromEnv().queue_timeout.count() == 300,
          "a queue timeout over a week keeps the default");
    unsetenv("GRPC_QPARSE_MAX_CONCURRENT_CALLS");
    unsetenv("GRPC_QPARSE_QUEUE_TIMEOUT_S");
    Check(grpc_qparse::DefaultMaxConcurrentCalls() >= 2,
          "the default cap is at least 2");
  }

  server->Shutdown();
  if (failures == 0) {
    std::printf("qparse_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "qparse_contract: %d check(s) failed\n", failures);
  return 1;
}

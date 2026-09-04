// Contract test for grpc-qparse: start the engine-backed service in
// process, dial it through the generated stubs, and walk the tier 0
// families plus this backend's own strengths (reading-order cells, shapes,
// embedded fonts) over the hello.pdf and rich.pdf fixtures.

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "qparse_service_impl.h"

namespace pdfv1 = ai::pipestream::parse::pdf::v1;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

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

  grpc_qparse::InitEngine(argv[2]);

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
    Check(fields == 1, "form field extracted");
    Check(outline_roots == 2, "outline items extracted");
    Check(!embedded.empty() && embedded[0].program().size() > 100000,
          "embedded font program extracted");
    Check(counts[pdfv1::PDF_FAMILY_TEXT_CELLS] >= 1, "trailer counts cells");
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
    const auto& raster = msg.raster();
    Check(raster.width_px() >= 611 && raster.width_px() <= 613,
          "raster width near Letter at 72 DPI");
    Check(raster.height_px() >= 791 && raster.height_px() <= 793,
          "raster height near Letter at 72 DPI");
    Check(raster.pixel_format() == pdfv1::PIXEL_FORMAT_RGBA8,
          "raster is RGBA8");
    Check(raster.pixels().size() ==
              static_cast<size_t>(raster.stride_bytes()) * raster.height_px(),
          "raster payload matches stride * height");
    bool has_ink = false;
    for (size_t i = 0; i + 3 < raster.pixels().size(); i += 4) {
      if (static_cast<unsigned char>(raster.pixels()[i]) != 0xFF) {
        has_ink = true;
        break;
      }
    }
    Check(has_ink, "raster has non-white pixels");
    Check(!reader->Read(&msg), "one raster for the one page");
    Check(reader->Finish().ok(), "render finished OK");
  }

  // Render error mapping.
  {
    grpc::ClientContext ctx;
    pdfv1::RenderRequest request;
    request.mutable_document()->set_data("not a pdf");
    request.set_dpi(72.0);
    auto reader = stub->Render(&ctx, request);
    pdfv1::RenderResponse msg;
    Check(!reader->Read(&msg), "unloadable render is empty");
    Check(reader->Finish().error_code() == grpc::FAILED_PRECONDITION,
          "unloadable render is FAILED_PRECONDITION");
  }

  server->Shutdown();
  if (failures == 0) {
    std::printf("qparse_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "qparse_contract: %d check(s) failed\n", failures);
  return 1;
}

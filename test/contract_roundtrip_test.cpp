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

  server->Shutdown();
  if (failures == 0) {
    std::printf("qparse_contract: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "qparse_contract: %d check(s) failed\n", failures);
  return 1;
}

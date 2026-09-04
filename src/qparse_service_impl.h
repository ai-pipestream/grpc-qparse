#pragma once

#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "document_cache.h"

namespace grpc_qparse {

// One-time process setup: engine logging and the font resource directory.
void InitEngine(const std::string& resources_dir);

// PdfBackendService over the MIT qpdf-based cell parser. The engine is
// safe to use concurrently through independent per-request decoder
// instances, so the service is plain thread-per-request with no worker
// pool. The content-addressed byte cache behind PdfDocument.sha256 lives
// here in the one server process, shared across requests.
class QparseServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  // Cache bounds from the GRPC_QPARSE_CACHE_* environment.
  QparseServiceImpl() : QparseServiceImpl(DocumentCacheConfigFromEnv()) {}
  explicit QparseServiceImpl(DocumentCacheConfig cache_config)
      : cache_(cache_config) {}
  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ProbeRequest* request,
      ai::protomolt::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::RenderResponse>*
          writer) override;

 private:
  DocumentCache cache_;
};

}  // namespace grpc_qparse

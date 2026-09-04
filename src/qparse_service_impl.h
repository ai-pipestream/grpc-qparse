#pragma once

#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/pdf/v1/pdf_backend_service.grpc.pb.h"

namespace grpc_qparse {

// One-time process setup: engine logging and the font resource directory.
void InitEngine(const std::string& resources_dir);

// PdfBackendService over the MIT qpdf-based cell parser. The engine is
// safe to use concurrently through independent per-request decoder
// instances, so the service is plain thread-per-request with no worker
// pool.
class QparseServiceImpl final
    : public ai::pipestream::parse::pdf::v1::PdfBackendService::Service {
 public:
  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::ProbeRequest* request,
      ai::pipestream::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::pipestream::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::pipestream::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::pipestream::parse::pdf::v1::RenderResponse>*
          writer) override;
};

}  // namespace grpc_qparse

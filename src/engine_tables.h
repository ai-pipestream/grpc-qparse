#ifndef GRPC_QPARSE_ENGINE_TABLES_H_
#define GRPC_QPARSE_ENGINE_TABLES_H_

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"
#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

// The one untyped surface of this service: the engine hands a decoded
// page's text cells, shapes and images over as JSON, and these readers turn
// it into the contract's messages. The engine is pinned to a commit, so the
// shape read here is the shape that commit writes, and anything else is
// never taken for an empty page:
//
// - a table that is not there, or lacks a column a reader needs, is an
//   EngineShapeError, which fails the page;
// - a row or an item that does not fit is left out and counted in
//   TableRead::skipped, for a warning on the page.
//
// engine_tables_test feeds both kinds in by hand.

namespace grpc_qparse {

// The engine's output is not in the shape this build reads.
class EngineShapeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The shift that puts a coordinate into the contract frame, unrotated user
// space with the CropBox's lower-left corner at (0, 0). The engine moves
// sanitized cells, shapes and images to the origin of its page boundary
// (the MediaBox, see ParseDecodeConfig), so they move by the MediaBox
// origin less the CropBox origin.
struct PageSpaceOffset {
  double dx = 0.0;
  double dy = 0.0;
};

struct TableRead {
  // Rows or items put into the chunk.
  uint64_t items = 0;
  // Rows or items left out because they are not in the expected shape.
  uint64_t skipped = 0;
};

// The font id a cell gets, from the engine's key and name for its font.
using CellFont = std::function<uint32_t(const std::string& font_key,
                                        const std::string& font_name)>;

// Appends the cells table's rows to chunk as text cells. Widget cells are
// not text cells (they re-enter through the form-field family) and count
// as neither read nor skipped.
TableRead FillTextCells(const nlohmann::json& cells,
                        const PageSpaceOffset& offset, const CellFont& font,
                        ai::protomolt::parse::pdf::v1::PageChunk* chunk);

// Appends the shapes list to chunk as vector shapes. A shape with no point
// has nothing to carry and counts as neither.
TableRead FillShapes(const nlohmann::json& shapes,
                     const PageSpaceOffset& offset,
                     ai::protomolt::parse::pdf::v1::PageChunk* chunk);

// Appends the images table's rows to chunk as placed images.
TableRead FillImages(const nlohmann::json& images,
                     const PageSpaceOffset& offset,
                     ai::protomolt::parse::pdf::v1::PageChunk* chunk);

}  // namespace grpc_qparse

#endif  // GRPC_QPARSE_ENGINE_TABLES_H_

#ifndef GRPC_QPARSE_QPDF_READERS_H_
#define GRPC_QPARSE_QPDF_READERS_H_

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.pb.h"
#include "ai/protomolt/parse/pdf/v1/pdf_backend_types.pb.h"

// The outline, the link annotations and the form fields, read from the
// call's qpdf handle straight into the contract's messages. The engine
// offers the three only as untyped JSON, and thinner than the document: an
// outline of titles with no targets, links with a URI action and a /Rect
// only, widgets with a /Rect and a known field type only, each missing
// number read as 0. Read here, an item the contract can carry is carried,
// and what it cannot carry is counted for a warning.

namespace grpc_qparse {

// The document's pages as a destination sees them.
struct PageTargets {
  // A page object's index in the document.
  std::map<QPDFObjGen, uint32_t> index_of;
  // Each page's CropBox lower-left corner: the origin of the contract's
  // frame on that page.
  std::vector<std::array<double, 2>> crop_origin;
};

// The deepest outline level that is followed (the top level is level 1).
inline constexpr int kMaxOutlineLevels = 65;

struct OutlineRead {
  // Items listed.
  uint64_t items = 0;
  // Items whose destination names no page of the document; each keeps its
  // title and has no destination.
  uint64_t unresolved = 0;
  // Items directly below the deepest level followed; they and everything
  // under them are left out.
  uint64_t cut = 0;
};

// Whether the catalog has an outline with at least one item.
bool HasOutline(QPDF& qpdf);

// Fills out with the outline's items in document order. An item reached a
// second time (a /First or /Next loop) is listed once.
OutlineRead ReadOutline(
    QPDF& qpdf, const PageTargets& pages,
    ai::protomolt::parse::pdf::v1::OutlineChunk* out);

struct LinksRead {
  // Links listed with no bbox: their /Rect is not four finite numbers.
  uint64_t without_rect = 0;
  // Go-to links whose destination names no page of the document; not
  // listed, since a Hyperlink is a uri or a destination.
  uint64_t unresolved = 0;
  // Links whose action is neither /URI nor /GoTo, by action type as
  // written (/GoToR, /Launch, /Named, ...); not listed either.
  std::map<std::string, uint64_t> untyped;
};

// Appends the page's link annotations to chunk. origin is the page's
// CropBox lower-left corner.
LinksRead ReadLinks(QPDF& qpdf, QPDFObjectHandle page, const PageTargets& pages,
                    const std::array<double, 2>& origin,
                    ai::protomolt::parse::pdf::v1::PageChunk* chunk);

struct FieldsRead {
  // Fields listed with no rect: the widget's /Rect is not four finite
  // numbers.
  uint64_t without_rect = 0;
};

// Appends one FormField per widget annotation of the page. The field type,
// flags, name, value and /TU are resolved through the /Parent chain (qpdf's
// form field helper); /AS is the widget's own.
FieldsRead ReadFormFields(QPDFObjectHandle page,
                          const std::array<double, 2>& origin,
                          ai::protomolt::parse::pdf::v1::PageChunk* chunk);

}  // namespace grpc_qparse

#endif  // GRPC_QPARSE_QPDF_READERS_H_

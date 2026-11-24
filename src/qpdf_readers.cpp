#include "qpdf_readers.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <utility>

#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>

namespace grpc_qparse {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

QPDFObjectHandle KeyOf(QPDFObjectHandle dictionary, const std::string& key) {
  return dictionary.isDictionary() ? dictionary.getKey(key)
                                   : QPDFObjectHandle::newNull();
}

// A finite number, when the object is one.
std::optional<double> NumberOf(QPDFObjectHandle object) {
  if (!object.isNumber()) return std::nullopt;
  const double value = object.getNumericValue();
  if (!std::isfinite(value)) return std::nullopt;
  return value;
}

// An annotation /Rect: four finite numbers, any pair of opposite corners.
std::optional<std::array<double, 4>> RectOf(QPDFObjectHandle rect) {
  if (!rect.isArray() || rect.getArrayNItems() != 4) return std::nullopt;
  std::array<double, 4> v{};
  for (int i = 0; i < 4; ++i) {
    const std::optional<double> number = NumberOf(rect.getArrayItem(i));
    if (!number.has_value()) return std::nullopt;
    v[static_cast<size_t>(i)] = *number;
  }
  return v;
}

void SetBox(const std::array<double, 4>& rect,
            const std::array<double, 2>& origin, pdfv1::BoundingBox* box) {
  box->set_x0(std::min(rect[0], rect[2]) - origin[0]);
  box->set_y0(std::min(rect[1], rect[3]) - origin[1]);
  box->set_x1(std::max(rect[0], rect[2]) - origin[0]);
  box->set_y1(std::max(rect[1], rect[3]) - origin[1]);
}

// A text string as UTF-8, or a name as written; empty for anything else.
std::string TextOf(QPDFObjectHandle object) {
  if (object.isString()) return object.getUTF8Value();
  if (object.isName()) return object.getName();
  return std::string();
}

// The explicit destination a name or a byte string stands for: names live
// in the catalog's /Dests dictionary, strings in the /Names /Dests name
// tree. Either may wrap the array in a dictionary under /D. Null when the
// document defines no such destination.
QPDFObjectHandle NamedDestination(QPDF& qpdf, QPDFObjectHandle name) {
  QPDFObjectHandle found = QPDFObjectHandle::newNull();
  QPDFObjectHandle root = qpdf.getRoot();
  if (name.isName()) {
    found = KeyOf(KeyOf(root, "/Dests"), name.getName());
  } else if (name.isString()) {
    QPDFObjectHandle tree = KeyOf(KeyOf(root, "/Names"), "/Dests");
    if (tree.isDictionary()) {
      // No repair: a lookup must not rewrite the call's document.
      QPDFNameTreeObjectHelper names(tree, qpdf, /*auto_repair=*/false);
      QPDFObjectHandle value;
      if (names.findObject(name.getUTF8Value(), value)) found = value;
    }
  }
  if (found.isDictionary()) return found.getKey("/D");
  return found;
}

// Fills out from a destination: an explicit array, or the name of one.
// False, with out untouched, when it names no page of this document.
// Coordinates are moved into the target page's frame; one the destination
// leaves null (keep the current view) stays unset.
bool FillDestination(QPDF& qpdf, const PageTargets& pages,
                     QPDFObjectHandle dest, pdfv1::PageDestination* out) {
  if (dest.isName() || dest.isString()) dest = NamedDestination(qpdf, dest);
  if (dest.isDictionary()) dest = dest.getKey("/D");
  if (!dest.isArray() || dest.getArrayNItems() < 1) return false;
  QPDFObjectHandle target = dest.getArrayItem(0);
  uint32_t index = 0;
  if (target.isInteger()) {
    // A page number, as a remote destination writes one; taken as this
    // document's page when it is in range, the way PDFium reads it.
    const long long number = target.getIntValue();
    if (number < 0 ||
        number >= static_cast<long long>(pages.crop_origin.size())) {
      return false;
    }
    index = static_cast<uint32_t>(number);
  } else if (target.isIndirect() && target.isDictionary()) {
    auto it = pages.index_of.find(target.getObjGen());
    if (it == pages.index_of.end()) return false;
    index = it->second;
  } else {
    return false;
  }
  out->set_page_index(index);
  const std::array<double, 2>& origin = pages.crop_origin.at(index);
  const int items = dest.getArrayNItems();
  const std::string kind = items > 1 && dest.getArrayItem(1).isName()
                               ? dest.getArrayItem(1).getName()
                               : std::string();
  auto number_at = [&dest, items](int i) -> std::optional<double> {
    return i < items ? NumberOf(dest.getArrayItem(i)) : std::nullopt;
  };
  auto set_x = [&](int i) {
    if (const auto left = number_at(i)) out->set_x(*left - origin[0]);
  };
  auto set_y = [&](int i) {
    if (const auto top = number_at(i)) out->set_y(*top - origin[1]);
  };
  if (kind == "/XYZ") {
    set_x(2);
    set_y(3);
    // Zoom 0 means "keep the current zoom", as null does.
    if (const auto zoom = number_at(4); zoom.has_value() && *zoom > 0.0) {
      out->set_zoom(*zoom);
    }
  } else if (kind == "/FitH" || kind == "/FitBH") {
    set_y(2);
  } else if (kind == "/FitV" || kind == "/FitBV") {
    set_x(2);
  } else if (kind == "/FitR") {
    // [page /FitR left bottom right top]: the view's top-left corner.
    set_x(2);
    set_y(5);
  }
  return true;
}

// What an outline item or a link leads to.
enum class Target { kNone, kUri, kDestination, kUnresolved, kUntyped };

// Reads an item's /A action or /Dest into uri or destination. An action
// whose type the contract has no field for is kUntyped, with the type as
// written in untyped_action.
Target ReadTarget(QPDF& qpdf, const PageTargets& pages, QPDFObjectHandle item,
                  std::string* uri, pdfv1::PageDestination* destination,
                  std::string* untyped_action) {
  QPDFObjectHandle action = item.getKey("/A");
  if (action.isDictionary()) {
    const std::string type = TextOf(action.getKey("/S"));
    if (type == "/URI") {
      *uri = TextOf(action.getKey("/URI"));
      return uri->empty() ? Target::kNone : Target::kUri;
    }
    if (type == "/GoTo") {
      return FillDestination(qpdf, pages, action.getKey("/D"), destination)
                 ? Target::kDestination
                 : Target::kUnresolved;
    }
    *untyped_action = type.empty() ? "an action with no /S" : type;
    return Target::kUntyped;
  }
  QPDFObjectHandle dest = item.getKey("/Dest");
  if (dest.isNull()) return Target::kNone;
  return FillDestination(qpdf, pages, dest, destination) ? Target::kDestination
                                                         : Target::kUnresolved;
}

struct OutlineWalk {
  QPDF& qpdf;
  const PageTargets& pages;
  // Items already listed: what ends a /First or /Next loop.
  std::set<QPDFObjGen> seen;
  OutlineRead read;
};

// Lists the sibling chain that starts at first under add(), descending
// into each item's own chain. level is the chain's level, 1 at the top.
void WalkOutline(OutlineWalk* walk, QPDFObjectHandle first, int level,
                 const std::function<pdfv1::OutlineNode*()>& add) {
  for (QPDFObjectHandle item = first; item.isDictionary();
       item = item.getKey("/Next")) {
    // An item that is not an indirect object cannot be reached twice.
    if (item.isIndirect() && !walk->seen.insert(item.getObjGen()).second) {
      return;
    }
    if (level > kMaxOutlineLevels) {
      ++walk->read.cut;
      continue;
    }
    pdfv1::OutlineNode* node = add();
    ++walk->read.items;
    node->set_title(TextOf(item.getKey("/Title")));
    std::string uri;
    std::string untyped;
    pdfv1::PageDestination destination;
    switch (ReadTarget(walk->qpdf, walk->pages, item, &uri, &destination,
                       &untyped)) {
      case Target::kUri:
        node->set_uri(uri);
        break;
      case Target::kDestination:
        *node->mutable_destination() = std::move(destination);
        break;
      case Target::kUnresolved:
        ++walk->read.unresolved;
        break;
      case Target::kNone:
      case Target::kUntyped:
        // A title with no target the contract types: the title is the item.
        break;
    }
    WalkOutline(walk, item.getKey("/First"), level + 1,
                [node] { return node->add_children(); });
  }
}

// The page's annotation dictionaries of one subtype. /Type is optional in
// an annotation (ISO 32000-1, table 164), so only a /Type that says
// something else rules an entry out. Both the standard /Annots and the
// misspelled /Annot some producers write are read, as the engine does.
std::vector<QPDFObjectHandle> AnnotationsOf(QPDFObjectHandle page,
                                            const std::string& subtype) {
  std::vector<QPDFObjectHandle> found;
  for (const char* key : {"/Annots", "/Annot"}) {
    QPDFObjectHandle annots = KeyOf(page, key);
    if (!annots.isArray()) continue;
    const int count = annots.getArrayNItems();
    for (int i = 0; i < count; ++i) {
      QPDFObjectHandle annot = annots.getArrayItem(i);
      if (!annot.isDictionary()) continue;
      QPDFObjectHandle type = annot.getKey("/Type");
      if (type.isName() && type.getName() != "/Annot") continue;
      QPDFObjectHandle sub = annot.getKey("/Subtype");
      if (sub.isName() && sub.getName() == subtype) found.push_back(annot);
    }
  }
  return found;
}

// ISO 32000-1 field flag bits (tables 221, 226, 228); bit n of the spec is
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

}  // namespace

bool HasOutline(QPDF& qpdf) {
  return KeyOf(KeyOf(qpdf.getRoot(), "/Outlines"), "/First").isDictionary();
}

OutlineRead ReadOutline(QPDF& qpdf, const PageTargets& pages,
                        pdfv1::OutlineChunk* out) {
  OutlineWalk walk{qpdf, pages, {}, {}};
  WalkOutline(&walk, KeyOf(KeyOf(qpdf.getRoot(), "/Outlines"), "/First"), 1,
              [out] { return out->add_roots(); });
  return walk.read;
}

LinksRead ReadLinks(QPDF& qpdf, QPDFObjectHandle page, const PageTargets& pages,
                    const std::array<double, 2>& origin,
                    pdfv1::PageChunk* chunk) {
  LinksRead read;
  for (QPDFObjectHandle annot : AnnotationsOf(page, "/Link")) {
    std::string uri;
    std::string untyped;
    pdfv1::PageDestination destination;
    const Target target =
        ReadTarget(qpdf, pages, annot, &uri, &destination, &untyped);
    if (target == Target::kUnresolved) {
      ++read.unresolved;
      continue;
    }
    if (target == Target::kUntyped) {
      ++read.untyped[untyped];
      continue;
    }
    // A link with no action and no destination leads nowhere.
    if (target == Target::kNone) continue;
    auto* link = chunk->add_hyperlinks();
    if (target == Target::kUri) {
      link->set_uri(uri);
    } else {
      *link->mutable_destination() = std::move(destination);
    }
    if (const auto rect = RectOf(annot.getKey("/Rect"))) {
      SetBox(*rect, origin, link->mutable_bbox());
    } else {
      ++read.without_rect;
    }
  }
  return read;
}

FieldsRead ReadFormFields(QPDFObjectHandle page,
                          const std::array<double, 2>& origin,
                          pdfv1::PageChunk* chunk) {
  FieldsRead read;
  for (QPDFObjectHandle annot : AnnotationsOf(page, "/Widget")) {
    QPDFFormFieldObjectHelper helper(annot);
    auto* field = chunk->add_form_fields();
    const std::string type = helper.getFieldType();
    const auto flags = static_cast<uint32_t>(helper.getFlags());
    field->set_flags(flags);
    field->set_read_only((flags & kFieldFlagReadOnly) != 0);
    const pdfv1::FormFieldKind kind = FieldKind(type, flags);
    field->set_kind(kind);
    field->set_name(helper.getFullyQualifiedName());
    std::string value = TextOf(helper.getValue());
    // A button's /V is a name, which qpdf spells with its slash; the other
    // backends (PDFium, poppler) report the bare state name, so this one
    // does too.
    if ((kind == pdfv1::FORM_FIELD_KIND_CHECK_BOX ||
         kind == pdfv1::FORM_FIELD_KIND_RADIO_BUTTON) &&
        value.starts_with('/')) {
      value.erase(0, 1);
    }
    if (!value.empty()) field->set_value(value);
    const std::string description =
        helper.getInheritableFieldValueAsString("/TU");
    if (!description.empty()) field->set_alternate_name(description);
    QPDFObjectHandle state = annot.getKey("/AS");
    if (state.isName() && !state.getName().empty()) {
      field->set_appearance_state(state.getName());
    }
    if (const auto rect = RectOf(annot.getKey("/Rect"))) {
      SetBox(*rect, origin, field->mutable_rect());
    } else {
      ++read.without_rect;
    }
  }
  return read;
}

}  // namespace grpc_qparse

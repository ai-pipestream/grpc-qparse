#include "engine_tables.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>

namespace grpc_qparse {

namespace pdfv1 = ai::protomolt::parse::pdf::v1;

namespace {

// A count the contract stores as uint32, when the value is one.
std::optional<uint32_t> UInt32Of(const nlohmann::json& value) {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    const auto v = value.get<uint64_t>();
    if (v > UINT32_MAX) return std::nullopt;
    return static_cast<uint32_t>(v);
  }
  const auto v = value.get<int64_t>();
  if (v < 0 || v > static_cast<int64_t>(UINT32_MAX)) return std::nullopt;
  return static_cast<uint32_t>(v);
}

// A header+data table's two arrays. Throws when the value is not such a
// table: the engine writes one for every decoded page, with no rows when
// the page has none.
struct Table {
  const nlohmann::json& header;
  const nlohmann::json& data;
};

Table TableOf(const nlohmann::json& table, const char* what) {
  if (!table.is_object() || !table.contains("header") ||
      !table.contains("data") || !table["header"].is_array() ||
      !table["data"].is_array()) {
    throw EngineShapeError(std::string("the engine's ") + what +
                           " table is not a header and a list of rows");
  }
  return {table["header"], table["data"]};
}

// The index of a named column; -1 when the table has none.
int ColumnIndex(const nlohmann::json& header, const std::string& name) {
  for (size_t i = 0; i < header.size(); ++i) {
    if (header[i].is_string() &&
        header[i].get_ref<const std::string&>() == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// The index of a column a reader cannot do without, followed by `run`
// columns in all (a box is four, a quad eight). Throws when the header has
// no such column or ends before the run does.
int RequiredColumn(const nlohmann::json& header, const std::string& name,
                   size_t run, const char* what) {
  const int index = ColumnIndex(header, name);
  if (index < 0 || static_cast<size_t>(index) + run > header.size()) {
    throw EngineShapeError(std::string("the engine's ") + what +
                           " table has no \"" + name + "\" column" +
                           (run > 1 ? " with the " + std::to_string(run - 1) +
                                          " after it"
                                    : std::string()));
  }
  return index;
}

// Whether a table row has the header's width and numbers in the columns
// the caller is about to read.
bool RowHasNumbers(const nlohmann::json& row, size_t width,
                   std::initializer_list<int> columns) {
  if (!row.is_array() || row.size() != width) return false;
  for (int column : columns) {
    if (!row[static_cast<size_t>(column)].is_number()) return false;
  }
  return true;
}

}  // namespace

TableRead FillTextCells(const nlohmann::json& cells,
                        const PageSpaceOffset& offset, const CellFont& font,
                        pdfv1::PageChunk* chunk) {
  const Table table = TableOf(cells, "cells");
  const nlohmann::json& header = table.header;
  const int ix0 = RequiredColumn(header, "x0", 1, "cells");
  const int iy0 = RequiredColumn(header, "y0", 1, "cells");
  const int ix1 = RequiredColumn(header, "x1", 1, "cells");
  const int iy1 = RequiredColumn(header, "y1", 1, "cells");
  const int irx0 = RequiredColumn(header, "r_x0", 8, "cells");
  const int itext = RequiredColumn(header, "text", 1, "cells");
  const int imode = ColumnIndex(header, "rendering-mode");
  const int ispace = ColumnIndex(header, "space-width");
  const int ifont = ColumnIndex(header, "font-name");
  const int ifontkey = ColumnIndex(header, "font-key");
  const int iltr = ColumnIndex(header, "left_to_right");
  const int iwidget = ColumnIndex(header, "widget");
  TableRead read;
  for (const auto& row : table.data) {
    if (!RowHasNumbers(row, header.size(),
                       {ix0, iy0, ix1, iy1, irx0, irx0 + 1, irx0 + 2,
                        irx0 + 3, irx0 + 4, irx0 + 5, irx0 + 6, irx0 + 7}) ||
        !row[itext].is_string()) {
      ++read.skipped;
      continue;
    }
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
    if (imode >= 0 && row[imode].is_number_integer()) {
      const auto mode = row[imode].get<int64_t>();
      if (mode >= 0 && mode <= 7) {
        cell->set_rendering_mode(
            static_cast<pdfv1::TextRenderingMode>(mode + 1));
      }
    }
    if (ifont >= 0 && row[ifont].is_string()) {
      const std::string& name = row[ifont].get_ref<const std::string&>();
      const std::string key = ifontkey >= 0 && row[ifontkey].is_string()
                                  ? row[ifontkey].get<std::string>()
                                  : std::string();
      cell->set_font_id(font(key, name));
    }
    ++read.items;
  }
  return read;
}

TableRead FillShapes(const nlohmann::json& shapes,
                     const PageSpaceOffset& offset, pdfv1::PageChunk* chunk) {
  if (!shapes.is_array()) {
    throw EngineShapeError("the engine's shapes are not a list");
  }
  TableRead read;
  for (const auto& s : shapes) {
    if (!s.is_object() || !s.contains("x") || !s.contains("y")) {
      ++read.skipped;
      continue;
    }
    const auto& xs = s["x"];
    const auto& ys = s["y"];
    if (!xs.is_array() || !ys.is_array() || xs.size() != ys.size()) {
      ++read.skipped;
      continue;
    }
    if (xs.empty()) continue;
    bool numeric = true;
    for (size_t i = 0; i < xs.size(); ++i) {
      numeric = numeric && xs[i].is_number() && ys[i].is_number();
    }
    if (!numeric) {
      ++read.skipped;
      continue;
    }
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
    if (auto closing = s.find("closing-type");
        closing != s.end() && closing->is_number() &&
        static_cast<int>(closing->get<double>()) == 2) {
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
      if (!rgb.is_array() || rgb.size() != 3 || !rgb[0].is_number() ||
          !rgb[1].is_number() || !rgb[2].is_number()) {
        return;
      }
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
    ++read.items;
  }
  return read;
}

TableRead FillImages(const nlohmann::json& images,
                     const PageSpaceOffset& offset, pdfv1::PageChunk* chunk) {
  const Table table = TableOf(images, "images");
  const nlohmann::json& header = table.header;
  const int ix0 = RequiredColumn(header, "x0", 4, "images");
  const int ikey = ColumnIndex(header, "xobject_key");
  const int iw = ColumnIndex(header, "image_width");
  const int ih = ColumnIndex(header, "image_height");
  const int ibpc = ColumnIndex(header, "bits_per_component");
  const int ics = ColumnIndex(header, "color_space");
  TableRead read;
  for (const auto& row : table.data) {
    if (!RowHasNumbers(row, header.size(), {ix0, ix0 + 1, ix0 + 2, ix0 + 3})) {
      ++read.skipped;
      continue;
    }
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
    if (iw >= 0) {
      if (auto width = UInt32Of(row[iw])) image->set_source_width_px(*width);
    }
    if (ih >= 0) {
      if (auto height = UInt32Of(row[ih])) image->set_source_height_px(*height);
    }
    if (ibpc >= 0) {
      if (auto bits = UInt32Of(row[ibpc])) image->set_bits_per_component(*bits);
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
    ++read.items;
  }
  return read;
}

}  // namespace grpc_qparse

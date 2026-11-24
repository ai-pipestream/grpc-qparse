// The engine's JSON tables, fed in by hand. The pinned engine never writes
// the shapes checked here, so the contract test cannot reach them through
// the service: a table that is missing a column must fail the page, and a
// row that does not fit must be counted, never read as an empty page.

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "engine_tables.h"

namespace pdfv1 = ai::protomolt::parse::pdf::v1;
using grpc_qparse::EngineShapeError;
using grpc_qparse::PageSpaceOffset;
using grpc_qparse::TableRead;
using nlohmann::json;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// The message of the EngineShapeError a call throws; empty when it throws
// nothing.
template <typename Call>
std::string ShapeErrorOf(Call call) {
  try {
    call();
  } catch (const EngineShapeError& e) {
    return e.what();
  }
  return std::string();
}

uint32_t NoFont(const std::string&, const std::string&) { return 0; }

// The cells header the pinned engine writes, as far as this build reads it.
json CellsHeader() {
  return json::array({"x0", "y0", "x1", "y1", "r_x0", "r_y0", "r_x1", "r_y1",
                      "r_x2", "r_y2", "r_x3", "r_y3", "text", "font-key",
                      "font-name", "widget", "left_to_right"});
}

json CellRow(const std::string& text, bool widget) {
  return json::array({10.0, 20.0, 30.0, 40.0, 10.0, 20.0, 30.0, 20.0, 30.0,
                      40.0, 10.0, 40.0, text, "/F1", "/Helvetica", widget,
                      true});
}

}  // namespace

int main() {
  const PageSpaceOffset offset{-10.0, -20.0};

  // Text cells: whole rows are read, and moved into the page frame.
  {
    json cells = {{"header", CellsHeader()},
                  {"data", json::array({CellRow("Hello", false),
                                        CellRow("field", true)})}};
    pdfv1::PageChunk chunk;
    int fonts_asked = 0;
    const TableRead read = grpc_qparse::FillTextCells(
        cells, offset,
        [&fonts_asked](const std::string& key, const std::string& name) {
          ++fonts_asked;
          return key == "/F1" && name == "/Helvetica" ? 7u : 0u;
        },
        &chunk);
    Check(read.items == 1 && read.skipped == 0 && chunk.text_cells_size() == 1,
          "cells: a whole row is one cell, a widget row is neither read nor "
          "skipped");
    if (chunk.text_cells_size() == 1) {
      const auto& cell = chunk.text_cells(0);
      Check(cell.text() == "Hello" && cell.bbox().x0() == 0.0 &&
                cell.bbox().y0() == 0.0 && cell.bbox().x1() == 20.0 &&
                cell.bbox().y1() == 20.0 && cell.quad().x2() == 20.0 &&
                cell.quad().y2() == 20.0,
            "cells: text and geometry arrive, shifted by the offset");
      Check(cell.font_id() == 7 && fonts_asked == 1,
            "cells: the font id comes from the cell's key and name");
      Check(cell.direction() == pdfv1::TEXT_DIRECTION_LEFT_TO_RIGHT,
            "cells: the direction column is read");
    }
  }

  // Text cells: rows that do not fit are counted, not dropped silently.
  {
    json short_row = json::array({1.0, 2.0});
    json text_as_number = CellRow("x", false);
    text_as_number[12] = 5;
    json box_as_string = CellRow("y", false);
    box_as_string[0] = "ten";
    json cells = {{"header", CellsHeader()},
                  {"data", json::array({CellRow("kept", false), short_row,
                                        text_as_number, box_as_string,
                                        json::object()})}};
    pdfv1::PageChunk chunk;
    const TableRead read =
        grpc_qparse::FillTextCells(cells, offset, NoFont, &chunk);
    Check(read.items == 1 && read.skipped == 4 && chunk.text_cells_size() == 1,
          "cells: four rows that do not fit are counted as skipped");
  }

  // Text cells: a table this build cannot read at all fails the page.
  {
    pdfv1::PageChunk chunk;
    json renamed = CellsHeader();
    renamed[0] = "left";
    std::string error = ShapeErrorOf([&] {
      grpc_qparse::FillTextCells(
          {{"header", renamed}, {"data", json::array({CellRow("a", false)})}},
          offset, NoFont, &chunk);
    });
    Check(error.find("\"x0\"") != std::string::npos,
          "cells: a missing x0 column is an error that names it");
    json no_text = json::array({"x0", "y0", "x1", "y1", "r_x0", "r_y0",
                                "r_x1", "r_y1", "r_x2", "r_y2", "r_x3",
                                "r_y3"});
    error = ShapeErrorOf([&] {
      grpc_qparse::FillTextCells({{"header", no_text}, {"data", json::array()}},
                                 offset, NoFont, &chunk);
    });
    Check(error.find("\"text\"") != std::string::npos,
          "cells: a missing text column is an error, even with no rows");
    json short_quad = json::array({"x0", "y0", "x1", "y1", "text", "r_x0",
                                   "r_y0"});
    error = ShapeErrorOf([&] {
      grpc_qparse::FillTextCells(
          {{"header", short_quad}, {"data", json::array()}}, offset, NoFont,
          &chunk);
    });
    Check(error.find("\"r_x0\"") != std::string::npos,
          "cells: a quad column run that ends early is an error");
    for (const json& not_a_table :
         {json(nullptr), json::array(), json({{"header", CellsHeader()}}),
          json({{"header", "x0"}, {"data", json::array()}}),
          json({{"header", CellsHeader()}, {"data", json::object()}})}) {
      error = ShapeErrorOf([&] {
        grpc_qparse::FillTextCells(not_a_table, offset, NoFont, &chunk);
      });
      Check(error.find("cells table") != std::string::npos,
            "cells: a value that is not a header and rows is an error");
    }
    Check(chunk.text_cells_size() == 0,
          "cells: nothing is read from a table that fails");
  }

  // Shapes.
  {
    json good = {{"x", json::array({10.0, 30.0, 30.0})},
                 {"y", json::array({20.0, 20.0, 50.0})},
                 {"closing-type", 2},
                 {"line-width", 1.5},
                 {"rgb-stroking", json::array({255, 0, 0})}};
    json uneven = {{"x", json::array({1.0, 2.0})}, {"y", json::array({1.0})}};
    json no_y = {{"x", json::array({1.0})}};
    json words = {{"x", json::array({"a"})}, {"y", json::array({1.0})}};
    json empty = {{"x", json::array()}, {"y", json::array()}};
    pdfv1::PageChunk chunk;
    const TableRead read = grpc_qparse::FillShapes(
        json::array({good, uneven, no_y, words, empty, 7}), offset, &chunk);
    Check(read.items == 1 && read.skipped == 4 && chunk.shapes_size() == 1,
          "shapes: one whole shape, four that do not fit counted, an empty "
          "one neither");
    if (chunk.shapes_size() == 1) {
      const auto& shape = chunk.shapes(0);
      Check(shape.segments_size() == 4 && shape.segments(0).has_move_to() &&
                shape.segments(1).has_line_to() && shape.segments(3).close() &&
                shape.bbox().x0() == 0.0 && shape.bbox().y1() == 30.0 &&
                shape.line_width() == 1.5 && shape.stroke_color().red() == 1.0,
            "shapes: the path, its box, width and color arrive");
    }
    const std::string error = ShapeErrorOf(
        [&] { grpc_qparse::FillShapes(json::object(), offset, &chunk); });
    Check(error.find("shapes") != std::string::npos,
          "shapes: a value that is not a list is an error");
  }

  // Images.
  {
    json header = json::array({"x0", "y0", "x1", "y1", "xobject_key",
                               "image_width", "image_height",
                               "bits_per_component", "color_space"});
    json good =
        json::array({110.0, 120.0, 210.0, 220.0, "/Im1", 64, 32, 8,
                     "/DeviceRGB"});
    json bad = json::array({110.0, "120", 210.0, 220.0, "/Im2", 1, 1, 8, ""});
    pdfv1::PageChunk chunk;
    const TableRead read = grpc_qparse::FillImages(
        {{"header", header}, {"data", json::array({good, bad, json(3)})}},
        offset, &chunk);
    Check(read.items == 1 && read.skipped == 2 && chunk.images_size() == 1,
          "images: one whole row, two that do not fit counted");
    if (chunk.images_size() == 1) {
      const auto& image = chunk.images(0);
      Check(image.bbox().x0() == 100.0 && image.bbox().y1() == 200.0 &&
                image.resource_name() == "Im1" &&
                image.source_width_px() == 64 &&
                image.source_height_px() == 32 &&
                image.bits_per_component() == 8 &&
                image.colorspace() == "DeviceRGB",
            "images: placement and declared size arrive");
    }
    std::string error = ShapeErrorOf([&] {
      grpc_qparse::FillImages(
          {{"header", json::array({"y0", "x1", "y1"})}, {"data", json::array()}},
          offset, &chunk);
    });
    Check(error.find("\"x0\"") != std::string::npos,
          "images: a missing x0 column is an error that names it");
    error = ShapeErrorOf([&] {
      grpc_qparse::FillImages(
          {{"header", json::array({"x0", "y0"})}, {"data", json::array()}},
          offset, &chunk);
    });
    Check(error.find("\"x0\"") != std::string::npos,
          "images: a box column run that ends early is an error");
    error = ShapeErrorOf(
        [&] { grpc_qparse::FillImages(json::array(), offset, &chunk); });
    Check(error.find("images table") != std::string::npos,
          "images: a value that is not a header and rows is an error");
  }

  if (failures == 0) {
    std::printf("qparse_engine_tables: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "qparse_engine_tables: %d check(s) failed\n", failures);
  return 1;
}

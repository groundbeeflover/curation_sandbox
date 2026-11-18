// SPDX-License-Identifier: Apache-2.0

// The Tool interface and parse_typed_value (#66, M1-25). parse_typed_value is checked against the
// examples and refusals tool.hpp and the issue give: "u=2.35", "h=1.60", "4.25", units, spaces,
// case, and text a curator could type by mistake, which must be refused cleanly. The Tool
// interface is shown working without a window: a small tool written here, as M2's wall and
// hanging tools will be, takes toolkit-free events and answers with a preview, guides and, when
// an action ends, a transaction that the test commits through doc::History as one undo step. The
// tool itself commits nothing.

#include <cs/editor/tool.hpp>

#include <cs/core/random.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/history.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cs::editor;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec3;

namespace doc = cs::doc;

namespace {

// ---- parse_typed_value -----------------------------------------------------------------------

// A unit's factor to metres (0.01, 0.001, 0.0254) is not exact in binary, so a converted value
// may be an ulp or two from the double nearest the exact answer, whichever way it is computed
// (times 0.01 or divided by 100). 1e-12 relative is a picometre on a metre: far below anything a
// curator types, far above rounding.
constexpr double typed_rel = 1e-12;

// The text for a failure message: at most its first 80 bytes.
std::string shown(std::string_view text) {
    return text.size() <= 80 ? std::string(text) : std::string(text.substr(0, 80)) + "...";
}

void check_parses(std::string_view text, TypedField field, double value_m) {
    INFO("text: \"" << shown(text) << "\"");
    const std::optional<TypedValue> parsed = parse_typed_value(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->field == field);
    CHECK(std::isfinite(parsed->value_m));
    CHECK_THAT(parsed->value_m, WithinRel(value_m, typed_rel));
}

void check_refused(std::string_view text) {
    INFO("text: \"" << shown(text) << "\" (" << text.size() << " bytes)");
    CHECK_FALSE(parse_typed_value(text).has_value());
}

// ---- A tool for the test ---------------------------------------------------------------------

// New ids, the same in every run: a fixed seed and a clock stopped at one millisecond.
doc::IdGenerator fixed_ids() {
    return doc::IdGenerator(cs::hash_seed(66, 1, 0, 0, 0),
                            [] { return std::int64_t{1'800'000'000'000}; });
}

// Where a pointer's ray, coming from above the floor, meets it (z = 0); nullopt when it does not.
std::optional<dvec3> floor_hit(const PointerEvent& pointer) {
    const dvec3 origin = pointer.ray_origin_m;
    const dvec3 direction = pointer.ray_direction;
    if (!(origin.z >= 0.0) || !(direction.z < 0.0)) {
        return std::nullopt;
    }
    return origin + direction * (-origin.z / direction.z);
}

// The answer of a tool that used the event and finished no action.
ToolResponse handled(std::string warning = {}) {
    ToolResponse response;
    response.handled = true;
    response.warning = std::move(warning);
    return response;
}

KeyEvent key(Key which) {
    KeyEvent event;
    event.key = which;
    return event;
}

// Places a lux sensor where the pointer's ray meets the floor, as the wall and hanging tools of M2
// will place walls and works. A primary press starts the action, moves drag the sensor over the
// floor (snapped to a 10 cm grid unless the event's modifiers turn snapping off), a typed h= sets
// its height, and the release or Enter finishes it. Escape and cancel() abandon it. It keeps only
// the state of the action in progress, and never changes the document.
class SensorTool final : public Tool {
  public:
    static constexpr double grid_m = 0.1;

    explicit SensorTool(doc::IdGenerator& ids) : ids_(&ids) {}

    std::string_view name() const override { return "Lux sensor"; }

    ToolResponse handle(const InputEvent& event, const doc::Document& document) override {
        if (const auto* pointer = std::get_if<PointerEvent>(&event)) {
            return on_pointer(*pointer, document);
        }
        if (const auto* pressed = std::get_if<KeyEvent>(&event)) {
            return on_key(*pressed, document);
        }
        return on_typed(std::get<TypedEvent>(event));
    }

    Preview preview() const override {
        Preview preview;
        if (drag_) {
            preview.entities.emplace(drag_->id, sensor());
        }
        return preview;
    }

    // While dragging, one guide: the distance dragged over the floor, labelled in centimetres.
    std::vector<Guide> guides() const override {
        if (!drag_) {
            return {};
        }
        const double cm = glm::length(drag_->at_m - drag_->start_m) * 100.0;
        return {Guide{.kind = GuideKind::length,
                      .from_m = drag_->start_m,
                      .to_m = drag_->at_m,
                      .label = std::to_string(std::lround(cm)) + " cm"}};
    }

    void cancel() override { drag_.reset(); }

  private:
    struct Drag {
        doc::Uuid id;          // the sensor's, taken when the drag starts
        dvec3 start_m{0.0};    // where the drag started, on the floor
        dvec3 at_m{0.0};       // where the pointer is now, on the floor
        double height_m = 0.0; // the sensor's height above the floor, typed with h=
    };

    doc::IdGenerator* ids_;
    std::optional<Drag> drag_;

    static dvec3 snapped(dvec3 p, const Modifiers& modifiers) {
        if (modifiers.snap_off) {
            return p;
        }
        return {std::round(p.x / grid_m) * grid_m, std::round(p.y / grid_m) * grid_m, p.z};
    }

    doc::Entity sensor() const {
        doc::Entity entity;
        entity.name = "Lux sensor";
        entity.lux_sensor = doc::LuxSensor{
            .position_m = {.x = drag_->at_m.x, .y = drag_->at_m.y, .z = drag_->height_m},
            .normal = {.x = 0.0, .y = 0.0, .z = 1.0}};
        return entity;
    }

    ToolResponse finish(const doc::Document& document) {
        ToolResponse response;
        response.handled = true;
        response.transaction.emplace(document, "Add lux sensor");
        response.transaction->add_entity(drag_->id, sensor());
        drag_.reset();
        return response;
    }

    ToolResponse on_pointer(const PointerEvent& pointer, const doc::Document& document) {
        const std::optional<dvec3> hit = floor_hit(pointer);
        switch (pointer.action) {
        case PointerAction::press:
            if (drag_ || pointer.button != PointerButton::primary || !hit) {
                return {};
            }
            drag_ = Drag{.id = ids_->next(),
                         .start_m = snapped(*hit, pointer.modifiers),
                         .at_m = snapped(*hit, pointer.modifiers),
                         .height_m = 0.0};
            return handled();
        case PointerAction::move:
            if (!drag_ || !hit) {
                return {};
            }
            drag_->at_m = snapped(*hit, pointer.modifiers);
            return handled();
        case PointerAction::release:
            if (!drag_ || pointer.button != PointerButton::primary) {
                return {};
            }
            if (hit) {
                drag_->at_m = snapped(*hit, pointer.modifiers);
            }
            return finish(document);
        }
        return {};
    }

    ToolResponse on_key(const KeyEvent& event, const doc::Document& document) {
        if (!drag_) {
            return {};
        }
        switch (event.key) {
        case Key::enter:
            return finish(document);
        case Key::escape:
            cancel();
            return handled();
        case Key::backspace:
            return {}; // an action of one step has nothing to take back
        }
        return {};
    }

    ToolResponse on_typed(const TypedEvent& typed) {
        if (!drag_ || typed.value.field != TypedField::h) {
            return {};
        }
        // The value may be negative, for the tool to judge: a sensor stays on or above the floor.
        if (typed.value.value_m < 0.0) {
            drag_->height_m = 0.0;
            return handled("Kept on the floor");
        }
        drag_->height_m = typed.value.value_m;
        return handled();
    }
};

PointerEvent pointer(PointerAction action, PointerButton button, dvec3 origin_m, dvec3 direction,
                     Modifiers modifiers = {}) {
    return PointerEvent{.action = action,
                        .button = button,
                        .ray_origin_m = origin_m,
                        .ray_direction = direction,
                        .modifiers = modifiers};
}

// A ray straight down from 1.6 m (eye height) onto the floor point (x, y): a plan view's ray.
PointerEvent down_onto(PointerAction action, PointerButton button, double x_m, double y_m,
                       Modifiers modifiers = {}) {
    return pointer(action, button, {x_m, y_m, 1.6}, {0.0, 0.0, -1.0}, modifiers);
}

// Positions on the 10 cm grid, and the points of rays computed from them: a few ulps of rounding.
// 1e-12 m allows for it and catches any real error.
constexpr double position_tol_m = 1e-12;

void check_position(const doc::Vec3& actual, dvec3 expected) {
    CHECK_THAT(actual.x, WithinAbs(expected.x, position_tol_m));
    CHECK_THAT(actual.y, WithinAbs(expected.y, position_tol_m));
    CHECK_THAT(actual.z, WithinAbs(expected.z, position_tol_m));
}

void check_point(dvec3 actual, dvec3 expected) {
    CHECK_THAT(actual.x, WithinAbs(expected.x, position_tol_m));
    CHECK_THAT(actual.y, WithinAbs(expected.y, position_tol_m));
    CHECK_THAT(actual.z, WithinAbs(expected.z, position_tol_m));
}

// The one sensor the tool's preview holds.
std::pair<doc::Uuid, doc::LuxSensor> previewed_sensor(const Tool& tool) {
    const Preview preview = tool.preview();
    REQUIRE(preview.entities.size() == 1);
    const auto& [id, entity] = *preview.entities.begin();
    REQUIRE(entity.lux_sensor.has_value());
    return {id, *entity.lux_sensor};
}

} // namespace

// ---- parse_typed_value: what it reads --------------------------------------------------------

TEST_CASE("typed value: the issue's examples, 'u=2.35', 'h=1.60' and '4.25', are a field and a "
          "value in metres",
          "[editor][tool]") {
    check_parses("u=2.35", TypedField::u, 2.35);
    check_parses("h=1.60", TypedField::h, 1.60);
    check_parses("4.25", TypedField::length, 4.25);
}

TEST_CASE("typed value: tool.hpp's examples, with units, signs and spaces", "[editor][tool]") {
    check_parses("H = 160 cm", TypedField::h, 1.60);
    check_parses("-0.5", TypedField::length, -0.5);
    check_parses("u=2350mm", TypedField::u, 2.35);
    // The inch is 0.0254 m exactly, by definition: 36 in = 0.9144 m.
    check_parses("36in", TypedField::length, 0.9144);
}

TEST_CASE("typed value: units m, cm, mm and in, metres when there is none", "[editor][tool]") {
    check_parses("4.25m", TypedField::length, 4.25);
    check_parses("425cm", TypedField::length, 4.25);
    check_parses("4250mm", TypedField::length, 4.25);
    check_parses("100in", TypedField::length, 2.54);
    check_parses("u=2.35 m", TypedField::u, 2.35);
    check_parses("h=150cm", TypedField::h, 1.50);
    check_parses("h=1500 mm", TypedField::h, 1.50);
    check_parses("u = 12 in", TypedField::u, 0.3048);
}

TEST_CASE("typed value: field and unit in either case", "[editor][tool]") {
    check_parses("U=2.35", TypedField::u, 2.35);
    check_parses("h=1.6", TypedField::h, 1.6);
    check_parses("H=1.6", TypedField::h, 1.6);
    check_parses("4.25 M", TypedField::length, 4.25);
    check_parses("160 CM", TypedField::length, 1.60);
    check_parses("160 Cm", TypedField::length, 1.60);
    check_parses("2350 MM", TypedField::length, 2.35);
    check_parses("36 IN", TypedField::length, 0.9144);
    check_parses("36 In", TypedField::length, 0.9144);
}

TEST_CASE("typed value: spaces may surround each part", "[editor][tool]") {
    check_parses("  4.25", TypedField::length, 4.25);
    check_parses("4.25  ", TypedField::length, 4.25);
    check_parses(" 4.25 m ", TypedField::length, 4.25);
    check_parses("u =2.35", TypedField::u, 2.35);
    check_parses("u= 2.35", TypedField::u, 2.35);
    check_parses("  u  =  2.35  cm  ", TypedField::u, 0.0235);
    check_parses(" h=1.60 ", TypedField::h, 1.60);
}

TEST_CASE("typed value: a decimal number with an optional sign, '.' and exponent; zero and "
          "negative values are kept for the tool to judge",
          "[editor][tool]") {
    check_parses("+4.25", TypedField::length, 4.25);
    check_parses("u=-0.10", TypedField::u, -0.10);
    check_parses("h=-1", TypedField::h, -1.0);
    check_parses("0", TypedField::length, 0.0);
    check_parses("u=0", TypedField::u, 0.0);
    check_parses("-0", TypedField::length, 0.0);
    check_parses("2", TypedField::length, 2.0);
    check_parses("2.5e-1", TypedField::length, 0.25);
    check_parses("4.25E0 m", TypedField::length, 4.25);
    check_parses("1e3 mm", TypedField::length, 1.0);
    check_parses("1e3mm", TypedField::length, 1.0);
    check_parses("u=2.35e2 cm", TypedField::u, 2.35);
}

TEST_CASE("typed value: the largest finite double is read, and stays finite in metres",
          "[editor][tool]") {
    constexpr double max = std::numeric_limits<double>::max();
    check_parses("1.7976931348623157e308", TypedField::length, max);
    check_parses("-1.7976931348623157e308 m", TypedField::length, -max);
    // Every unit's factor is at most 1, so no unit can take a finite number to infinity: in inches
    // the largest double is max x 0.0254 m.
    check_parses("1.7976931348623157e308 in", TypedField::length, max * 0.0254);
}

TEST_CASE("typed value: text of max_typed_text_bytes is read, one byte more is refused",
          "[editor][tool]") {
    REQUIRE(max_typed_text_bytes == 64);
    const std::string at_limit = std::string(max_typed_text_bytes - 4, ' ') + "4.25";
    REQUIRE(at_limit.size() == max_typed_text_bytes);
    check_parses(at_limit, TypedField::length, 4.25);

    const std::string over_limit = std::string(max_typed_text_bytes - 3, ' ') + "4.25";
    REQUIRE(over_limit.size() == max_typed_text_bytes + 1);
    check_refused(over_limit);

    // A number that would be valid, but is longer than the limit.
    check_refused("1" + std::string(max_typed_text_bytes, '0'));
    check_refused("u=0." + std::string(max_typed_text_bytes, '1'));
    // Very long text, as pasted by mistake.
    check_refused(std::string(1'000'000, '4'));
}

// ---- parse_typed_value: what it refuses ------------------------------------------------------

TEST_CASE("typed value: empty text, or only spaces, is refused", "[editor][tool]") {
    check_refused("");
    check_refused(" ");
    check_refused("      ");
}

TEST_CASE("typed value: an unknown field, or a field without a number, is refused",
          "[editor][tool]") {
    check_refused("x=2");
    check_refused("w=2");
    check_refused("l=4.25");
    check_refused("uh=1");
    check_refused("length=4.25");
    check_refused("=2.35");
    check_refused("u=");
    check_refused("u = ");
    check_refused("u=cm");
    check_refused("u");
    check_refused("h");
    check_refused("u2.35");
    check_refused("u==2.35");
    check_refused("u=h=2");
    check_refused("u:2.35");
}

TEST_CASE("typed value: an unknown unit, or a unit without a number, is refused",
          "[editor][tool]") {
    check_refused("4.25 km");
    check_refused("4.25 ft");
    check_refused("4.25 inch");
    check_refused("4.25 metres");
    check_refused("4.25 cmm");
    check_refused("4.25 mmm");
    check_refused("4.25 \xc2\xb5m"); // micrometres, in UTF-8
    check_refused("1em");
    check_refused("m");
    check_refused("cm");
    check_refused("u=mm");
}

TEST_CASE("typed value: a ',' for the decimal point is refused", "[editor][tool]") {
    check_refused("4,25");
    check_refused("u=2,35");
    check_refused("h=1,60 m");
    check_refused("1,000");
}

TEST_CASE("typed value: numbers that are not finite are refused", "[editor][tool]") {
    check_refused("nan");
    check_refused("NaN");
    check_refused("-nan");
    check_refused("u=nan");
    check_refused("nan cm");
    check_refused("inf");
    check_refused("-inf");
    check_refused("INF");
    check_refused("infinity");
    check_refused("h=Infinity");
    check_refused("inf m");
}

TEST_CASE("typed value: a number beyond the largest double, which becomes infinite, is refused",
          "[editor][tool]") {
    check_refused("1e309");
    check_refused("-1e309");
    check_refused("1.8e308");
    check_refused("u=1e400");
    check_refused("h=-1e400 m");
    check_refused("1e99999999999999999999");
}

TEST_CASE("typed value: text before or after the value is refused", "[editor][tool]") {
    check_refused("4.25 m extra");
    check_refused("4.25x");
    check_refused("4.25 m m");
    check_refused("4.25;");
    check_refused("4.25m;");
    check_refused("u=2.35 h=1.6");
    check_refused("4.25 4.25");
    check_refused("4 25");
    check_refused("about 4.25");
    check_refused("~4.25");
    check_refused("0x10");
    check_refused("1.2.3");
    check_refused(std::string_view("4.25\0", 5)); // a NUL byte after the number
    check_refused(std::string_view("\0"
                                   "4.25",
                                   5));
}

TEST_CASE("typed value: malformed numbers are refused", "[editor][tool]") {
    check_refused("-");
    check_refused("+");
    check_refused(".");
    check_refused("--1");
    check_refused("+-1");
    check_refused("1e");
    check_refused("1e+");
    check_refused("e5");
    check_refused("u=e5");
}

// ---- The Tool interface ----------------------------------------------------------------------

TEST_CASE("tool: input events in, a preview and guides out, and one transaction that the caller "
          "commits as one undo step; the tool commits nothing",
          "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;
    const std::string before = doc::to_project_json(history.document());

    CHECK(tool.name() == "Lux sensor");
    CHECK(tool.preview().empty());
    CHECK(tool.guides().empty());

    // Press: a ray from (1, 1, 2) along (0.5, 0.25, -1) meets the floor at t = 2, at (2, 1.5, 0),
    // on the grid already.
    const ToolResponse pressed = tool.handle(
        pointer(PointerAction::press, PointerButton::primary, {1.0, 1.0, 2.0}, {0.5, 0.25, -1.0}),
        history.document());
    CHECK(pressed.handled);
    CHECK_FALSE(pressed.transaction.has_value());
    CHECK(pressed.warning.empty());

    // The preview holds the sensor the action would add: an id the document does not hold.
    const auto [id, at_press] = previewed_sensor(tool);
    CHECK(history.document().find_entity(id) == nullptr);
    check_position(at_press.position_m, {2.0, 1.5, 0.0});
    {
        const std::vector<Guide> guides = tool.guides();
        REQUIRE(guides.size() == 1);
        CHECK(guides[0].kind == GuideKind::length);
        check_point(guides[0].from_m, {2.0, 1.5, 0.0});
        check_point(guides[0].to_m, {2.0, 1.5, 0.0});
        CHECK(guides[0].label == "0 cm");
    }

    // Move: straight down onto (3, 1.5). The preview and the guide follow; the document does not.
    const ToolResponse moved = tool.handle(
        down_onto(PointerAction::move, PointerButton::none, 3.0, 1.5), history.document());
    CHECK(moved.handled);
    CHECK_FALSE(moved.transaction.has_value());
    {
        const auto [moved_id, at_move] = previewed_sensor(tool);
        CHECK(moved_id == id);
        check_position(at_move.position_m, {3.0, 1.5, 0.0});
        const std::vector<Guide> guides = tool.guides();
        REQUIRE(guides.size() == 1);
        check_point(guides[0].from_m, {2.0, 1.5, 0.0});
        check_point(guides[0].to_m, {3.0, 1.5, 0.0});
        CHECK(guides[0].label == "100 cm");
    }
    CHECK(history.document().revision() == 0);
    CHECK(history.document().entities().empty());

    // Release at (3.5, 1.5): the action ends with a transaction, made for the document as it is.
    ToolResponse released = tool.handle(
        down_onto(PointerAction::release, PointerButton::primary, 3.5, 1.5), history.document());
    CHECK(released.handled);
    REQUIRE(released.transaction.has_value());
    CHECK(released.transaction->name() == "Add lux sensor");
    CHECK(&released.transaction->document() == &history.document());
    CHECK(tool.preview().empty());
    CHECK(tool.guides().empty());

    // Nothing has been committed by the tool.
    CHECK(history.document().revision() == 0);
    CHECK(history.undo_count() == 0);
    CHECK(doc::to_project_json(history.document()) == before);

    // The caller commits it: one undo step, which adds the sensor where the pointer was released.
    const doc::ChangeList changes = history.commit(std::move(*released.transaction));
    CHECK_FALSE(changes.empty());
    CHECK(history.undo_count() == 1);
    CHECK(history.undo_name().value_or("") == "Add lux sensor");
    const doc::Entity* added = history.document().find_entity(id);
    REQUIRE(added != nullptr);
    REQUIRE(added->lux_sensor.has_value());
    check_position(added->lux_sensor->position_m, {3.5, 1.5, 0.0});
    check_position(added->lux_sensor->normal, {0.0, 0.0, 1.0});
    const std::string after = doc::to_project_json(history.document());

    history.undo();
    CHECK(history.undo_count() == 0);
    CHECK(doc::to_project_json(history.document()) == before);
    history.redo();
    CHECK(doc::to_project_json(history.document()) == after);
}

TEST_CASE("tool: Escape abandons the action, and nothing is committed", "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;
    const std::string before = doc::to_project_json(history.document());

    REQUIRE(tool.handle(down_onto(PointerAction::press, PointerButton::primary, 1.0, 1.0),
                        history.document())
                .handled);
    REQUIRE_FALSE(tool.preview().empty());

    const ToolResponse escaped = tool.handle(key(Key::escape), history.document());
    CHECK(escaped.handled);
    CHECK_FALSE(escaped.transaction.has_value());
    CHECK(tool.preview().empty());
    CHECK(tool.guides().empty());

    // The release that follows ends nothing: there is no action left.
    const ToolResponse released = tool.handle(
        down_onto(PointerAction::release, PointerButton::primary, 2.0, 1.0), history.document());
    CHECK_FALSE(released.handled);
    CHECK_FALSE(released.transaction.has_value());

    CHECK(history.undo_count() == 0);
    CHECK(doc::to_project_json(history.document()) == before);
}

TEST_CASE("tool: cancel abandons the action, as when another tool is chosen or the document "
          "changes under a drag",
          "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;
    const std::string before = doc::to_project_json(history.document());

    REQUIRE(tool.handle(down_onto(PointerAction::press, PointerButton::primary, 1.0, 1.0),
                        history.document())
                .handled);
    REQUIRE(tool.handle(down_onto(PointerAction::move, PointerButton::none, 2.0, 2.0),
                        history.document())
                .handled);
    REQUIRE_FALSE(tool.preview().empty());
    REQUIRE_FALSE(tool.guides().empty());

    tool.cancel();
    CHECK(tool.preview().empty());
    CHECK(tool.guides().empty());
    const ToolResponse entered = tool.handle(key(Key::enter), history.document());
    CHECK_FALSE(entered.handled);
    CHECK_FALSE(entered.transaction.has_value());

    // Cancelling with no action in progress is harmless.
    tool.cancel();
    CHECK(tool.preview().empty());

    CHECK(history.undo_count() == 0);
    CHECK(doc::to_project_json(history.document()) == before);
}

TEST_CASE("tool: a typed value from parse_typed_value goes in as an event, and Enter finishes the "
          "action as a click would",
          "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;

    REQUIRE(tool.handle(down_onto(PointerAction::press, PointerButton::primary, 2.0, 1.0),
                        history.document())
                .handled);

    // "h = 85 cm": a sensor at desk height.
    const std::optional<TypedValue> typed = parse_typed_value("h = 85 cm");
    REQUIRE(typed.has_value());
    const ToolResponse set = tool.handle(TypedEvent{.value = *typed}, history.document());
    CHECK(set.handled);
    CHECK_FALSE(set.transaction.has_value());
    CHECK(set.warning.empty());
    check_position(previewed_sensor(tool).second.position_m, {2.0, 1.0, 0.85});

    // A value the tool judges wrong comes back with a warning for the status bar.
    const std::optional<TypedValue> below = parse_typed_value("h=-0.2");
    REQUIRE(below.has_value());
    const ToolResponse kept = tool.handle(TypedEvent{.value = *below}, history.document());
    CHECK(kept.handled);
    CHECK(kept.warning == "Kept on the floor");
    check_position(previewed_sensor(tool).second.position_m, {2.0, 1.0, 0.0});

    REQUIRE(tool.handle(TypedEvent{.value = *typed}, history.document()).handled);

    // A field this tool does not use goes elsewhere.
    const std::optional<TypedValue> along = parse_typed_value("u=2.35");
    REQUIRE(along.has_value());
    CHECK_FALSE(tool.handle(TypedEvent{.value = *along}, history.document()).handled);

    ToolResponse entered = tool.handle(key(Key::enter), history.document());
    CHECK(entered.handled);
    REQUIRE(entered.transaction.has_value());
    CHECK(history.undo_count() == 0);

    history.commit(std::move(*entered.transaction));
    CHECK(history.undo_count() == 1);
    REQUIRE(history.document().entities().size() == 1);
    const doc::Entity& added = history.document().entities().begin()->second;
    REQUIRE(added.lux_sensor.has_value());
    check_position(added.lux_sensor->position_m, {2.0, 1.0, 0.85});
}

TEST_CASE("tool: the modifiers come with each event: snap_off places the sensor off the grid",
          "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;

    REQUIRE(tool.handle(down_onto(PointerAction::press, PointerButton::primary, 2.03, 1.48),
                        history.document())
                .handled);
    check_position(previewed_sensor(tool).second.position_m, {2.0, 1.5, 0.0});

    REQUIRE(tool.handle(down_onto(PointerAction::move, PointerButton::none, 2.03, 1.48,
                                  Modifiers{.snap_off = true}),
                        history.document())
                .handled);
    check_position(previewed_sensor(tool).second.position_m, {2.03, 1.48, 0.0});
}

TEST_CASE("tool: events the tool does not use are not handled, so the viewport gives them to the "
          "camera",
          "[editor][tool]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    SensorTool sensor_tool(ids);
    Tool& tool = sensor_tool;
    const doc::Document& document = history.document();

    // A move with no action in progress: orbiting.
    CHECK_FALSE(tool.handle(down_onto(PointerAction::move, PointerButton::none, 1.0, 1.0), document)
                    .handled);
    // A press whose ray never meets the floor: looking up at the ceiling, or level.
    CHECK_FALSE(tool.handle(pointer(PointerAction::press, PointerButton::primary, {1.0, 1.0, 1.6},
                                    {0.0, 1.0, 0.5}),
                            document)
                    .handled);
    CHECK_FALSE(tool.handle(pointer(PointerAction::press, PointerButton::primary, {1.0, 1.0, 1.6},
                                    {1.0, 0.0, 0.0}),
                            document)
                    .handled);
    // Other buttons, and keys with nothing to finish or abandon.
    CHECK_FALSE(
        tool.handle(down_onto(PointerAction::press, PointerButton::secondary, 1.0, 1.0), document)
            .handled);
    CHECK_FALSE(
        tool.handle(down_onto(PointerAction::press, PointerButton::middle, 1.0, 1.0), document)
            .handled);
    CHECK_FALSE(tool.handle(key(Key::escape), document).handled);
    CHECK_FALSE(tool.handle(key(Key::enter), document).handled);
    CHECK_FALSE(tool.handle(key(Key::backspace), document).handled);
    CHECK_FALSE(tool.handle(TypedEvent{.value = {.field = TypedField::h, .value_m = 1.0}}, document)
                    .handled);

    CHECK(tool.preview().empty());
    CHECK(tool.guides().empty());
    CHECK(history.undo_count() == 0);
}

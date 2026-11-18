// SPDX-License-Identifier: Apache-2.0

#pragma once

// Tools (ADR 0006): the interactive part of editing, such as drawing walls (M2-02) or dragging a
// work along its wall (M2-15), as state machines without a toolkit. The viewport turns its
// toolkit's events into InputEvents and hands each to the active tool with the document as it is
// now. The tool answers with a ToolResponse, which holds a transaction when the event finishes an
// action, such as the click that ends a wall or the release that ends a drag. Between events it
// offers a preview, the entities as the action in progress would leave them, and guides, the
// snapping lines and labels to draw. A tool never changes the document itself: the caller commits
// what it returns with doc::History::commit, so a whole drag is one transaction (ADR 0007), and
// tools are tested without a window.
//
// Conventions: world coordinates in metres, z up, as everywhere in the document.

#include <cs/doc/document.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cs::editor {

// ---- Input -----------------------------------------------------------------------------------

// The keys and buttons held with an event.
struct Modifiers {
    bool snap_off = false; // the modifier that turns snapping off (ADR 0006, Hanging)
    bool operator==(const Modifiers&) const = default;
};

enum class PointerAction { press, move, release };
enum class PointerButton { none, primary, secondary, middle };

// The pointer, as the ray from the eye through the point under it, in world coordinates, so that
// one event serves every view: orbit, walk, plan and elevation.
struct PointerEvent {
    PointerAction action = PointerAction::move;
    PointerButton button = PointerButton::none; // pressed or released; none for a move
    glm::dvec3 ray_origin_m{0.0};
    glm::dvec3 ray_direction{0.0, 0.0, -1.0}; // need not be of unit length; never zero
    Modifiers modifiers;
};

enum class Key {
    enter,     // finishes the action, as a click would
    escape,    // abandons the action
    backspace, // takes back the last step of an action of several, such as one wall of a chain
};

struct KeyEvent {
    Key key = Key::escape;
    Modifiers modifiers;
};

// Which quantity a typed value sets: a bare number is the tool's own length (a wall's), u= the
// distance along a face from its left end, h= the height of a centre (ADR 0006, Hanging).
enum class TypedField { length, u, h };

struct TypedValue {
    TypedField field = TypedField::length;
    double value_m = 0.0; // finite; may be negative or zero, for the tool to judge
    bool operator==(const TypedValue&) const = default;
};

// The longest text parse_typed_value reads; anything longer is refused.
inline constexpr std::size_t max_typed_text_bytes = 64;

// Reads what a curator typed in the measurements box (as in SketchUp): an optional field, "u" or
// "h" in either case followed by '=', then a number with an optional unit, "m", "cm", "mm" or
// "in" (case-insensitive), metres when there is none. Spaces may surround each part. The number
// is decimal, with an optional sign, a '.' as the decimal point and an optional exponent.
//
//   "4.25"     -> length 4.25 m     "u=2.35"    -> u 2.35 m       "H = 160 cm" -> h 1.60 m
//   "-0.5"     -> length -0.5 m     "u=2350mm"  -> u 2.35 m       "36in"       -> length 0.9144 m
//
// nullopt for anything else: empty text, an unknown field or unit, a ',' for the decimal point,
// a number that is not finite (or becomes infinite in metres), text after the number, or more
// than max_typed_text_bytes.
std::optional<TypedValue> parse_typed_value(std::string_view text);

// The value confirmed in the measurements box with Enter, after parse_typed_value.
struct TypedEvent {
    TypedValue value;
};

using InputEvent = std::variant<PointerEvent, KeyEvent, TypedEvent>;

// ---- Output ----------------------------------------------------------------------------------

// What a guide shows, so the viewport can style it (M2-19, M2-20).
enum class GuideKind {
    centre_line, // the height works are hung at
    alignment,   // the centre, top or bottom of another work on the face
    equal_gap,   // equal gaps to the left and right neighbours
    face_centre, // the middle of a wall face
    margin,      // a distance from a corner, an opening or another work
    endpoint,    // a wall's end, snapped to
    direction,   // a right angle, or a direction snapped to
    length,      // the length of what is being drawn
};

// A line to draw in the viewport while a tool works, with an optional label in the project's
// display units: "150 cm", "= 42 cm = 42 cm".
struct Guide {
    GuideKind kind = GuideKind::length;
    glm::dvec3 from_m{0.0};
    glm::dvec3 to_m{0.0};
    std::string label; // empty: none
    bool operator==(const Guide&) const = default;
};

// The action in progress as it would leave the document, for the viewport to draw in place of
// what it changes: each entity it would add or change, by id, as it would be. An id the document
// holds stands for that entity, drawn where the action would put it; any other id is an entity
// the action would add. Empty when no action is in progress.
struct Preview {
    std::map<doc::Uuid, doc::Entity> entities;
    bool empty() const { return entities.empty(); }
};

// A tool's answer to one event.
struct ToolResponse {
    // Whether the tool used the event. The viewport gives the events a tool does not use to the
    // camera controller.
    bool handled = false;
    // The action the event finished, made for the document the event came with, for the caller to
    // commit; nullopt when the event finished none.
    std::optional<doc::Transaction> transaction;
    // For the status bar, such as "Kept on the face"; empty for none.
    std::string warning;
};

// A tool that edits the document through input events. Implementations keep only the state of
// the action in progress (where a drag started, the points clicked so far); the document comes
// with every event, and what else a tool needs, such as the doc::IdGenerator for the ids of what
// it adds, it is given when it is made.
class Tool {
  public:
    virtual ~Tool() = default;

    // The tool's name, as the toolbar shows it: "Wall", "Hang".
    virtual std::string_view name() const = 0;

    // Handles one event, given the document as it is now. A transaction in the response is made
    // for `document` at its present revision, and is committed before the next event comes, or
    // not at all.
    virtual ToolResponse handle(const InputEvent& event, const doc::Document& document) = 0;

    // The action in progress, as it would leave the document; empty when there is none.
    virtual Preview preview() const = 0;

    // The guides to draw now, nearest first; empty when there are none.
    virtual std::vector<Guide> guides() const = 0;

    // Abandons the action in progress without committing anything, as Escape does: when another
    // tool is chosen, or the document changes under a drag (an undo, a load).
    virtual void cancel() = 0;

  protected:
    Tool() = default;
    Tool(const Tool&) = default;
    Tool& operator=(const Tool&) = default;
};

} // namespace cs::editor

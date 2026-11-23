// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/commands/hanging.hpp>

#include <cs/editor/commands/room.hpp>

#include "commands/common.hpp"

#include <cs/doc/work.hpp>
#include <cs/geom/box_frame.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/world/compile.hpp>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace cs::editor {

namespace {

// How far a row may reach into the clearance at the face's ends, for rounding: a nanometre, so
// that a row that fits exactly is not refused for the last bit of a sum. [C]
constexpr double row_fit_tolerance_m = 1e-9;

// A length for the curator: "6.20 m".
std::string metres(double value_m) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << value_m << " m";
    return text.str();
}

} // namespace

AddedWork add_work(const doc::Document& document, doc::IdGenerator& ids, const doc::AssetRef& image,
                   double width_m, double height_m, std::string title) {
    detail::check_length("add_work", "the width", width_m);
    detail::check_length("add_work", "the height", height_m);
    detail::check_asset("add_work", image);
    // The entity's name is the title, and the transaction's "Add " and the title.
    detail::check_text("add_work", "the title", title);
    const std::optional<doc::Uuid> wall_id = find_wall_labelled(document, default_wall_label);
    if (!wall_id) {
        detail::refuse("add_work", "no wall is labelled " + std::string(default_wall_label));
    }
    const geom::FaceFrame face =
        detail::face_frame(*document.find_entity(*wall_id)->wall, doc::WallFace::a);

    doc::Entity exhibit;
    exhibit.name = title.empty() ? "Work" : title;
    exhibit.framing.emplace().style = default_frame_style;
    exhibit.placement = doc::OnWall{.wall = *wall_id,
                                    .face = doc::WallFace::a,
                                    .distance_m = face.length_m / 2.0,
                                    .centre_height_m = document.settings().centre_line_m,
                                    .stand_off_m = default_stand_off_m};
    doc::Transaction transaction(document, title.empty() ? "Add work" : "Add " + title);

    doc::Work work;
    work.title = std::move(title);
    work.form.width_m = width_m;
    work.form.height_m = height_m;
    work.image = image;
    work.material.preset = default_material_preset;

    const doc::Uuid work_id = ids.next();
    const doc::Uuid exhibit_id = ids.next();
    exhibit.exhibit = doc::Exhibit{work_id};
    transaction.set_work(work_id, std::move(work));
    transaction.add_entity(exhibit_id, std::move(exhibit));
    return {std::move(transaction), work_id, exhibit_id};
}

double hung_width_m(const doc::Work& work, const std::optional<doc::Framing>& framing) {
    if (!framing) {
        return work.form.width_m;
    }
    // As world::compile builds the frame (compile.cpp, add_frame).
    const world::DefaultFrame& frame = world::default_frame;
    const geom::BoxFrame box{
        work.form.width_m, work.form.height_m, framing->width_m.value_or(frame.member_width_m),
        std::max(frame.depth_m, work.form.depth_m.value_or(0.0) + frame.front_lip_m),
        frame.rebate_m};
    try {
        return geom::outer_size(box).x;
    } catch (const std::logic_error&) {
        // A frame geom cannot build: compile shows the work unframed.
        return work.form.width_m;
    }
}

AddedWork add_work_in_row(const doc::Document& document, doc::IdGenerator& ids,
                          const doc::AssetRef& image, double width_m, double height_m,
                          std::string title) {
    // add_work checks the arguments, finds the wall and takes the ids; the row then moves its work.
    AddedWork added = add_work(document, ids, image, width_m, height_m, std::move(title));
    const doc::Uuid wall_id = *find_wall_labelled(document, default_wall_label);
    const geom::FaceFrame face =
        detail::face_frame(*document.find_entity(wall_id)->wall, doc::WallFace::a);

    struct Member {
        doc::Uuid entity;
        doc::OnWall placement;
        double width_m = 0.0; // hung_width_m
    };
    std::vector<Member> row;
    // Entities come in order of id, which a stable sort keeps for equal distances.
    for (const auto& [id, entity] : document.entities()) {
        if (!entity.exhibit || !entity.placement) {
            continue;
        }
        const auto* on_wall = std::get_if<doc::OnWall>(&*entity.placement);
        const doc::Work* work = document.find_work(entity.exhibit->work);
        if (on_wall == nullptr || on_wall->wall != wall_id || on_wall->face != doc::WallFace::a ||
            work == nullptr) {
            continue;
        }
        row.push_back({id, *on_wall, hung_width_m(*work, entity.framing)});
    }
    std::ranges::stable_sort(row, {}, [](const Member& m) { return m.placement.distance_m; });
    const doc::Entity& made = *added.transaction.find_entity(added.exhibit);
    row.push_back({added.exhibit, std::get<doc::OnWall>(*made.placement),
                   hung_width_m(*added.transaction.find_work(added.work), made.framing)});

    double length_m = row_gap_m * static_cast<double>(row.size() - 1);
    for (const Member& member : row) {
        length_m += member.width_m;
    }
    const double clearance_m = (face.length_m - length_m) / 2.0;
    if (clearance_m < row_end_clearance_m - row_fit_tolerance_m) {
        // Said to the curator as it is (Works > Add painting), so it does not start with the
        // command's name as other refusals do.
        throw std::invalid_argument(
            "Wall " + std::string(default_wall_label) + " is full: a row of " +
            std::to_string(row.size()) + " works with " + metres(row_gap_m) + " gaps is " +
            metres(length_m) + " long, and with " + metres(row_end_clearance_m) +
            " clear at each end it needs a face of " +
            metres(length_m + 2.0 * row_end_clearance_m) + "; face A is " + metres(face.length_m) +
            " long");
    }

    double left_m = clearance_m;
    for (Member& member : row) {
        const double distance_m = left_m + member.width_m / 2.0;
        left_m += member.width_m + row_gap_m;
        if (member.placement.distance_m != distance_m) {
            member.placement.distance_m = distance_m;
            added.transaction.set(member.entity, doc::Placement{member.placement});
        }
    }
    return added;
}

} // namespace cs::editor

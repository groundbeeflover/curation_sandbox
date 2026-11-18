// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/commands/hanging.hpp>

#include <cs/editor/commands/room.hpp>

#include "commands/common.hpp"

#include <cs/doc/work.hpp>
#include <cs/geom/wall_face.hpp>

#include <optional>
#include <utility>

namespace cs::editor {

AddedWork add_work(const doc::Document& document, doc::IdGenerator& ids, const doc::AssetRef& image,
                   double width_m, double height_m, std::string title) {
    detail::check_length("add_work", "the width", width_m);
    detail::check_length("add_work", "the height", height_m);
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

} // namespace cs::editor

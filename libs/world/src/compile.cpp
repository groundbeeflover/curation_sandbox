// SPDX-License-Identifier: Apache-2.0

#include <cs/world/compile.hpp>

#include "room_walls.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/work.hpp>
#include <cs/geom/box_frame.hpp>
#include <cs/geom/painting.hpp>
#include <cs/geom/room.hpp>
#include <cs/geom/wall_face.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace cs::world {

namespace {

Id to_id(const doc::Uuid& id) {
    return Id{id.bytes()};
}

glm::dvec2 to_glm(const doc::Vec2& v) {
    return {v.x, v.y};
}
glm::dvec3 to_glm(const doc::Vec3& v) {
    return {v.x, v.y, v.z};
}
glm::dvec3 to_glm(const doc::LinearRgb& c) {
    return {c.r, c.g, c.b};
}

geom::WallFace to_geom(doc::WallFace face) {
    return face == doc::WallFace::a ? geom::WallFace::a : geom::WallFace::b;
}

// A face whose frame is the identity, to build a hung thing in its own coordinates.
geom::FaceFrame own_coordinates() {
    geom::FaceFrame face;
    face.u = {1.0, 0.0, 0.0};
    face.v = {0.0, 1.0, 0.0};
    face.normal = {0.0, 0.0, 1.0};
    return face;
}

// The outline's corners, (min, max), when it is an axis-aligned rectangle of positive area:
// four distinct corners, each a corner of the bounding box, with sides along x or y.
std::optional<std::pair<glm::dvec2, glm::dvec2>>
axis_aligned_rectangle(const std::vector<doc::Vec2>& outline) {
    if (outline.size() != 4) {
        return std::nullopt;
    }
    glm::dvec2 lo = to_glm(outline[0]);
    glm::dvec2 hi = lo;
    for (const doc::Vec2& p : outline) {
        lo = glm::min(lo, to_glm(p));
        hi = glm::max(hi, to_glm(p));
    }
    if (!(hi.x > lo.x && hi.y > lo.y)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const doc::Vec2& p = outline[i];
        const doc::Vec2& q = outline[(i + 1) % 4];
        const bool at_corner = (p.x == lo.x || p.x == hi.x) && (p.y == lo.y || p.y == hi.y);
        const bool along_one_axis = (p.x == q.x) != (p.y == q.y);
        if (!at_corner || !along_one_axis) {
            return std::nullopt;
        }
    }
    // Opposite corners differ in both coordinates, so the four are distinct.
    const bool distinct = outline[0].x != outline[2].x && outline[0].y != outline[2].y &&
                          outline[1].x != outline[3].x && outline[1].y != outline[3].y;
    if (!distinct) {
        return std::nullopt;
    }
    return std::pair{lo, hi};
}

// The point distance_m along a polyline from its first point, and whether the polyline was
// long enough; beyond its end, the end.
std::pair<glm::dvec2, bool> along_polyline(const std::vector<doc::Vec2>& points,
                                           double distance_m) {
    double left_m = distance_m;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const glm::dvec2 a = to_glm(points[i]);
        const glm::dvec2 b = to_glm(points[i + 1]);
        const double length_m = glm::distance(a, b);
        if (left_m <= length_m && length_m > 0.0) {
            return {a + (b - a) * (left_m / length_m), true};
        }
        left_m -= length_m;
    }
    return {to_glm(points.back()), left_m <= 0.0};
}

class Compiler {
  public:
    explicit Compiler(const doc::Document& document) : document_(document) {}

    World run() {
        // Lights aim at exhibits, so exhibits come first; each kind is in the order of ids.
        for (const auto& [id, entity] : document_.entities()) {
            if (entity.room) {
                add_room(id, *entity.room);
            }
            if (entity.exhibit) {
                add_exhibit(id, entity);
            }
        }
        for (const auto& [id, entity] : document_.entities()) {
            if (entity.luminaire) {
                add_light(id, entity);
            }
        }
        for (const auto& [id, entity] : document_.entities()) {
            if (entity.viewpoint) {
                add_camera(id, *entity.viewpoint);
            }
        }
        // Each pass adds problems in id order; together, they are put in it too.
        std::ranges::stable_sort(world_.problems, {}, &Problem::entity);
        return std::move(world_);
    }

  private:
    void problem(const doc::Uuid& id, std::string reason) {
        world_.problems.push_back({to_id(id), std::move(reason)});
    }

    std::uint32_t add_instance(const doc::Uuid& id, Part part, geom::Mesh mesh, Material material,
                               const glm::dmat4& transform) {
        world_.meshes.push_back(std::move(mesh));
        world_.materials.push_back(std::move(material));
        world_.instances.push_back(
            {to_id(id), part, static_cast<std::uint32_t>(world_.meshes.size() - 1),
             static_cast<std::uint32_t>(world_.materials.size() - 1), transform});
        return static_cast<std::uint32_t>(world_.instances.size() - 1);
    }

    void add_room(const doc::Uuid& id, const doc::Room& room) {
        const LambertMaterial walls{glm::dvec3(default_wall_albedo)};
        const double height_m = room.ceiling_height_m - room.floor_height_m;
        try {
            if (const auto rectangle = axis_aligned_rectangle(room.outline_m)) {
                const auto [lo, hi] = *rectangle;
                const glm::dmat4 transform =
                    glm::translate(glm::dmat4(1.0), glm::dvec3(lo.x, lo.y, room.floor_height_m));
                add_instance(id, Part::room, geom::room_mesh(hi.x - lo.x, hi.y - lo.y, height_m),
                             walls, transform);
                return;
            }
            std::vector<glm::dvec2> outline;
            outline.reserve(room.outline_m.size());
            for (const doc::Vec2& p : room.outline_m) {
                outline.push_back(to_glm(p));
            }
            geom::Mesh mesh =
                detail::room_walls(outline, room.floor_height_m, room.ceiling_height_m);
            add_instance(id, Part::room, std::move(mesh), walls, glm::dmat4(1.0));
            problem(id,
                    "the room's outline is not an axis-aligned rectangle: it is shown with its "
                    "walls but no floor or ceiling until rooms of any shape arrive (M2-01, M2-31)");
        } catch (const std::logic_error& error) {
            problem(id, std::string("the room cannot be built: ") + error.what());
        }
    }

    // The transform of a thing hung on a wall: its own coordinates to the world. Throws
    // std::invalid_argument for a wall or placement geom refuses.
    static glm::dmat4 hang(const doc::Wall& wall, const doc::OnWall& on_wall) {
        const geom::FaceFrame face = geom::wall_face_frame(to_glm(wall.start_m), to_glm(wall.end_m),
                                                           wall.thickness_m, to_geom(on_wall.face));
        return geom::on_wall_transform(
            face, {on_wall.distance_m, on_wall.centre_height_m, on_wall.stand_off_m});
    }

    void add_exhibit(const doc::Uuid& id, const doc::Entity& entity) {
        if (!entity.placement) {
            return; // on the checklist, not hung
        }
        const auto* on_wall = std::get_if<doc::OnWall>(&*entity.placement);
        if (on_wall == nullptr) {
            problem(id, "only works hung on a wall are shown yet");
            return;
        }
        const doc::Work* work = document_.find_work(entity.exhibit->work);
        const doc::Entity* wall_entity = document_.find_entity(on_wall->wall);
        if (work == nullptr || wall_entity == nullptr || !wall_entity->wall) {
            problem(id, "the exhibit's work or wall is not in the document");
            return;
        }
        // A work flat on its wall stands out by min_face_offset_m, out of the wall's plane.
        const double depth_m =
            std::max(work->form.depth_m.value_or(0.0), min_face_offset_m - on_wall->stand_off_m);
        const geom::FlatWork flat{work->form.width_m, work->form.height_m, depth_m};
        glm::dmat4 transform;
        geom::Mesh face_mesh;
        try {
            transform = hang(*wall_entity->wall, *on_wall);
            face_mesh = geom::painting_quad(own_coordinates(), flat, {});
        } catch (const std::logic_error& error) {
            problem(id, std::string("the work cannot be hung: ") + error.what());
            return;
        }

        Exhibit exhibit;
        exhibit.entity = to_id(id);
        exhibit.work = to_id(entity.exhibit->work);
        exhibit.work_size_m = {flat.width_m, flat.height_m};
        exhibit.face_centre_m = glm::dvec3(transform * glm::dvec4(0.0, 0.0, flat.depth_m, 1.0));
        exhibit.face_u = glm::dvec3(transform[0]);
        exhibit.face_v = glm::dvec3(transform[1]);
        exhibit.face_normal = glm::dvec3(transform[2]);
        exhibit.visible = {-exhibit.work_size_m / 2.0, exhibit.work_size_m / 2.0};
        exhibit.painting_instance = add_instance(id, Part::painting, std::move(face_mesh),
                                                 painting_surface(*work), transform);

        if (entity.framing) {
            add_frame(id, *entity.framing, flat, transform, exhibit);
        }
        world_.exhibits.push_back(std::move(exhibit));
    }

    static PaintingSurface painting_surface(const doc::Work& work) {
        PaintingSurface surface;
        // Material presets are not defined yet (M2-10): the defaults, with the work's overrides.
        material::PaintingMaterial& parameters = surface.parameters;
        parameters.base.roughness =
            work.material.base_roughness.value_or(parameters.base.roughness);
        parameters.coat.weight = work.material.coat_weight.value_or(parameters.coat.weight);
        if (parameters.coat.weight > 0.0) {
            // Under varnish the paint's own highlight is off (painting_material.hpp).
            parameters.base.specular_weight = 0.0;
        }
        parameters.coat.roughness =
            work.material.coat_roughness.value_or(parameters.coat.roughness);
        if (work.image) {
            surface.image = AssetRef{work.image->sha256, work.image->extension};
        }
        return surface;
    }

    void add_frame(const doc::Uuid& id, const doc::Framing& framing, const geom::FlatWork& work,
                   const glm::dmat4& transform, Exhibit& exhibit) {
        const geom::BoxFrame frame{
            work.width_m, work.height_m, framing.width_m.value_or(default_frame.member_width_m),
            std::max(default_frame.depth_m, work.depth_m + default_frame.front_lip_m),
            default_frame.rebate_m};
        geom::Mesh mesh;
        glm::dvec2 sight_m;
        try {
            mesh = geom::box_frame(frame);
            sight_m = geom::sight_opening(frame);
        } catch (const std::logic_error& error) {
            problem(id, std::string("the frame cannot be built, so the work is shown unframed: ") +
                            error.what());
            return;
        }
        LambertMaterial finish{glm::dvec3(default_frame.albedo)};
        if (framing.colour) {
            // The document allows any colour of at least 0; a surface reflects at most all the
            // light it receives.
            finish.albedo = glm::min(to_glm(*framing.colour), glm::dvec3(1.0));
            if (finish.albedo != to_glm(*framing.colour)) {
                problem(id, "the frame's colour is brighter than white; it is shown white");
            }
        }
        exhibit.frame_instance = add_instance(id, Part::frame, std::move(mesh), finish, transform);
        exhibit.visible = {-sight_m / 2.0, sight_m / 2.0};
    }

    struct FixturePosition {
        glm::dvec3 point_m;
        bool beyond_track = false; // placed at the end of a track it runs past
    };

    // Where a luminaire's placement puts it; nullopt for placements luminaires cannot have yet
    // (on the floor, on a host). Throws std::invalid_argument for a wall geom refuses.
    std::optional<FixturePosition> fixture_position(const doc::Placement& placement) const {
        if (const auto* on_track = std::get_if<doc::OnTrack>(&placement)) {
            const doc::Entity* track = document_.find_entity(on_track->track);
            if (track == nullptr || !track->track || track->track->points_m.empty()) {
                return std::nullopt;
            }
            const auto [point, on] = along_polyline(track->track->points_m, on_track->distance_m);
            return FixturePosition{glm::dvec3(point, track->track->height_m), !on};
        }
        if (const auto* on_ceiling = std::get_if<doc::OnCeiling>(&placement)) {
            const doc::Entity* room = document_.find_entity(on_ceiling->room);
            if (room == nullptr || !room->room) {
                return std::nullopt;
            }
            return FixturePosition{
                glm::dvec3(to_glm(on_ceiling->position_m), room->room->ceiling_height_m)};
        }
        if (const auto* on_wall = std::get_if<doc::OnWall>(&placement)) {
            const doc::Entity* wall = document_.find_entity(on_wall->wall);
            if (wall == nullptr || !wall->wall) {
                return std::nullopt;
            }
            // The origin of the thing's own coordinates: the centre of its back.
            return FixturePosition{glm::dvec3(hang(*wall->wall, *on_wall)[3])};
        }
        return std::nullopt;
    }

    // The point a luminaire aimed at the entity points to: the centre of a hung exhibit's face,
    // or the position of a luminaire, viewpoint or lux sensor.
    std::optional<glm::dvec3> target_point(const doc::Uuid& target) const {
        const Id target_id = to_id(target);
        // Exhibits are in id order.
        const auto exhibit =
            std::ranges::lower_bound(world_.exhibits, target_id, {}, &Exhibit::entity);
        if (exhibit != world_.exhibits.end() && exhibit->entity == target_id) {
            return exhibit->face_centre_m;
        }
        const doc::Entity* entity = document_.find_entity(target);
        if (entity == nullptr) {
            return std::nullopt;
        }
        if (entity->luminaire && entity->placement) {
            if (const auto position = fixture_position(*entity->placement)) {
                return position->point_m;
            }
        }
        if (entity->viewpoint) {
            return to_glm(entity->viewpoint->position_m);
        }
        if (entity->lux_sensor) {
            return to_glm(entity->lux_sensor->position_m);
        }
        return std::nullopt;
    }

    std::optional<glm::dvec3> aim_point(const doc::Aim& aim) const {
        if (const auto* point = std::get_if<doc::AimAtPoint>(&aim)) {
            return to_glm(point->point_m);
        }
        return target_point(std::get<doc::AimAtEntity>(aim).target);
    }

    void add_light(const doc::Uuid& id, const doc::Entity& entity) {
        const doc::Luminaire& luminaire = *entity.luminaire;
        const doc::FixtureType* type =
            document_.library().find_fixture_type(luminaire.fixture_type);
        if (type == nullptr) {
            problem(id, "the luminaire's fixture type is not in the project's library");
            return;
        }
        if (luminaire.dimming <= 0.0) {
            return; // switched off: it emits nothing
        }
        try {
            const auto position =
                entity.placement ? fixture_position(*entity.placement) : std::nullopt;
            if (!position) {
                problem(id, "only luminaires on a track, on a wall or in a ceiling are shown yet");
                return;
            }
            const auto aim = aim_point(luminaire.aim);
            if (!aim) {
                problem(id, "the luminaire aims at an entity that has no position in the world");
                return;
            }
            auto spot = std::make_shared<const light::GenericSpot>(
                type->flux_lm * luminaire.dimming, type->beam_angle_rad, type->field_angle_rad);
            world_.lights.push_back(
                {to_id(id), light::Luminaire(position->point_m, *aim, spot), spot,
                 luminaire.colour_temperature_k.value_or(type->colour_temperature_k)});
            if (position->beyond_track) {
                problem(id, "the luminaire is beyond its track's end; it is shown at the end");
            }
        } catch (const std::logic_error& error) {
            problem(id, std::string("the luminaire cannot be built: ") + error.what());
        }
    }

    void add_camera(const doc::Uuid& id, const doc::Viewpoint& viewpoint) {
        const double cos_pitch = std::cos(viewpoint.pitch_rad);
        const double sin_pitch = std::sin(viewpoint.pitch_rad);
        const double cos_yaw = std::cos(viewpoint.yaw_rad);
        const double sin_yaw = std::sin(viewpoint.yaw_rad);
        Camera camera;
        camera.entity = to_id(id);
        camera.projection = Projection::perspective;
        camera.position_m = to_glm(viewpoint.position_m);
        camera.forward = {cos_pitch * cos_yaw, cos_pitch * sin_yaw, sin_pitch};
        camera.up = {-sin_pitch * cos_yaw, -sin_pitch * sin_yaw, cos_pitch};
        camera.vertical_fov_rad = viewpoint.field_of_view_rad;
        camera.ev100 = viewpoint.ev100;
        world_.cameras.push_back(camera);
    }

    const doc::Document& document_;
    World world_;
};

} // namespace

World compile(const doc::Document& document) {
    return Compiler(document).run();
}

} // namespace cs::world

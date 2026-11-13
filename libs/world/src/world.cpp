// SPDX-License-Identifier: Apache-2.0

#include <cs/world/world.hpp>

#include <algorithm>

namespace cs::world {

namespace {

bool same_mesh(const geom::Mesh& a, const geom::Mesh& b) {
    return a.positions == b.positions && a.normals == b.normals && a.uvs == b.uvs &&
           a.indices == b.indices;
}

bool same_parameters(const material::PaintingMaterial& a, const material::PaintingMaterial& b) {
    return a.base.color == b.base.color && a.base.roughness == b.base.roughness &&
           a.base.specular_weight == b.base.specular_weight && a.coat.weight == b.coat.weight &&
           a.coat.roughness == b.coat.roughness && a.coat.ior == b.coat.ior &&
           a.metal.weight == b.metal.weight && a.metal.f0 == b.metal.f0 &&
           a.metal.edge_tint == b.metal.edge_tint && a.metal.roughness == b.metal.roughness &&
           a.fuzz.weight == b.fuzz.weight && a.fuzz.color == b.fuzz.color &&
           a.fuzz.roughness == b.fuzz.roughness && a.glazing.present == b.glazing.present &&
           a.glazing.ior == b.glazing.ior &&
           a.glazing.reflectance_scale == b.glazing.reflectance_scale &&
           a.glazing.transmittance == b.glazing.transmittance;
}

bool same_material(const Material& a, const Material& b) {
    if (a.index() != b.index()) {
        return false;
    }
    if (const auto* lambert = std::get_if<LambertMaterial>(&a)) {
        return *lambert == std::get<LambertMaterial>(b);
    }
    const auto& pa = std::get<PaintingSurface>(a);
    const auto& pb = std::get<PaintingSurface>(b);
    return pa.image == pb.image && same_parameters(pa.parameters, pb.parameters);
}

bool same_spot(const light::GenericSpot* a, const light::GenericSpot* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return a->total_lumens() == b->total_lumens() && a->beam_angle_rad() == b->beam_angle_rad() &&
           a->field_angle_rad() == b->field_angle_rad();
}

bool same_light(const Light& a, const Light& b) {
    // The fixture's x axis fixes its roll, which with the beam axis fixes its orientation.
    const glm::dvec3 x{1.0, 0.0, 0.0};
    return a.entity == b.entity && a.luminaire.position_m() == b.luminaire.position_m() &&
           a.luminaire.axis() == b.luminaire.axis() &&
           a.luminaire.to_fixture(x) == b.luminaire.to_fixture(x) &&
           same_spot(a.spot.get(), b.spot.get()) &&
           a.colour_temperature_k == b.colour_temperature_k;
}

} // namespace

bool operator==(const World& a, const World& b) {
    return std::ranges::equal(a.meshes, b.meshes, same_mesh) &&
           std::ranges::equal(a.materials, b.materials, same_material) &&
           a.instances == b.instances && a.exhibits == b.exhibits &&
           std::ranges::equal(a.lights, b.lights, same_light) && a.cameras == b.cameras &&
           a.problems == b.problems;
}

} // namespace cs::world

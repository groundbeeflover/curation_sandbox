# 0008. The data model: works, entities and placements

- Status: proposed
- Date: 2026-11-10
- Decides: question 7 of the planning brief (a data model in which a painting is one kind of
  exhibit, so that sculpture and furniture fit later without a rewrite)

## Context

The MVP handles paintings and drawings only. Sculpture, plinths, vitrines, benches, labels and
partition walls must fit later without changing what already exists. The model must also support
undo by recorded patches (ADR 0007), stable references, and incremental updates of the renderer.

## Options

1. **A class per kind of thing** (`Painting`, `Sculpture`, `Bench`), each with its own placement
   fields. Simple at first; every new kind duplicates placement, selection and serialisation
   code, and "a painting on an easel" needs a new class.
2. **One generic node tree with untyped properties.** Flexible, but nothing is checked and every
   consumer re-validates.
3. **Entities with typed components, and placement as its own component.** What a thing *is*
   (a work, a wall, a fixture) is separate from *how it is shown* (framing) and *where it is*
   (on a wall, on the floor, on another object). **Chosen.**

## Decision

### Works are records, separate from the space

A **Work** is what is exhibited, independent of where it hangs: one record can be placed, removed
and placed again without losing its data.

```text
Work {
  id, number (checklist number), title, artist, date, medium_text,
  accession_number?, lender?, credit_line, licence?,
  form: Flat { width_m, height_m, depth_m? }          # paintings, drawings, prints
      | Object { bounds_m, mesh: AssetRef }           # sculpture, after 1.0
  image: AssetRef?                                    # absent: shown as a labelled placeholder
  material: { preset, overrides }                     # ADR 0003
  sensitivity: { class, max_lux?, max_lux_hours? }    # ADR 0003
  source: Catalogue { id, image_hash } | Import { original_name } | Checklist { row }
  external_ids: [{ system, record_id }]
}
```

### The space is entities with components

Every entity has a UUID, a display name and any subset of these components. The MVP ships the
first group; the others are reserved names that later milestones add.

| Component | Fields (SI units) | When |
| --- | --- | --- |
| `Wall` | start and end (x, y), height, thickness, finish of each face, label | MVP |
| `Opening` | wall, offset along it, width, height, sill, kind (door, passage, window) | MVP |
| `Room` | outline, floor and ceiling heights and finishes | MVP |
| `Exhibit` | work id | MVP |
| `Framing` | frame style id, overrides (width, colour), mat, glazing | MVP |
| `Placement` | one of the kinds below | MVP |
| `Track` | polyline, height | MVP |
| `Luminaire` | fixture type id, dimming 0 to 1, aim (a target entity or a point), colour temperature override | MVP |
| `LuxSensor` | position, normal (a persistent lux probe) | MVP |
| `Viewpoint` | position, yaw, pitch, field of view, exposure | MVP |
| `Dimension` | two anchors | MVP |
| `Furniture` | furniture type id, parameters (plinth width, depth, height) | 1.0 |
| `Label` | exhibit, text, size | 1.0 |
| `Window`, `Skylight` | geometry, glazing | 1.0 |

**Placement** is one of:

| Kind | Fields | Used by |
| --- | --- | --- |
| `OnWall` | wall, face (A or B), distance from the face's left end to the centre, centre height, stand-off | paintings, drawings, labels, wall-mounted fixtures |
| `OnFloor` | room, x, y, rotation | plinths, benches, vitrines, sculpture (1.0) |
| `OnHost` | host entity, local transform | a sculpture on a plinth, an object in a vitrine (1.0) |
| `OnTrack` | track, distance along it | luminaires |
| `OnCeiling` | room, x, y | recessed luminaires |

So a painting is `Exhibit + Framing + Placement::OnWall`; a sculpture on a plinth will be
`Exhibit + Placement::OnHost(plinth)`, where the plinth is `Furniture + Placement::OnFloor`. New
kinds of thing add a component or a placement kind; nothing existing changes.

### Library definitions

Reusable definitions referenced by id: **frame styles** (ADR 0003), **fixture types** (an IES
asset or lumens and beam angle, colour temperature, a model), **surface finishes** (wall paints,
floors), later **furniture types**. The built-in library ships with the app; a project stores
copies of the definitions it uses, so that it opens identically on another machine.

### Derived, never stored

World transforms, framed sizes, meshes, lux statistics per work (mean, minimum, maximum,
uniformity), yearly exposure, snapping guides, warnings. They are recomputed from the document,
so they cannot go stale.

### Code shape

- The document lives in the `doc` library: a normalised store (a map from id to entity, each
  entity a fixed set of optional components), changed only through the mutation API (ADR 0007).
  It depends only on `core`.
- `world` compiles the document into a renderer-neutral **render world** (meshes, instances,
  materials, lights in SI units, cameras) and updates it incrementally from each transaction's
  change list. The engine and the GPU tiers read only the render world, never the document, so
  every tier sees identical input.

## Consequences

- Sculpture, furniture and labels arrive as new components and placement kinds, not rewrites;
  the first non-painting exhibit in 1.0 (a plinth) is the proof.
- A wall-hung work's position is stored as a curator thinks of it (distance along the wall,
  centre height), which makes snapping, elevations and hanging lists direct.
- The render world is a second representation to keep in sync; the change list from each
  transaction does that, and a test compares an incremental update with a full rebuild.

## Revisit

When the first non-painting exhibit is decomposed (1.0), and before any change to `Placement`.

## Sources

- Research notes for this record (editor research, section 6), which compared entity-component
  layouts in editors and the sparse-delta undo approach.

# The skeleton scene

The small scene most of milestone 1 is tested on: one room, one framed work showing the test
chart (`tools/test-chart/`), one spotlight and one viewpoint. It is a project folder:
`project.json` holds the document, and `assets/` holds the chart, named by its SHA-256.
`libs/doc/tests/test_skeleton_scene.cpp` checks that it loads and saves back byte-identical,
and that it holds the numbers below. It also checks that the chart's SHA-256 matches its name,
and that `assets/` holds nothing the document does not name.

Units are SI. Plan positions are (x, y) in metres, and z is up, with the floor at z = 0.

| Entity | What it is |
| --- | --- |
| Gallery | A room 6 m along x, 4 m along y and 3.5 m high: outline (0, 0), (6, 0), (6, 4), (0, 4) |
| Wall A to D | 0.1 m thick and 3.5 m high, lettered anticlockwise from wall A along y = 0. Each wall's face A is one edge of the outline and faces into the room |
| Test chart | A work 0.73 m wide and 0.92 m high in a box frame, on face A of wall A at u = 3.0 m (the middle), centre height 1.50 m (the centre line), stand-off 0.04 m |
| Track A | On the ceiling (z = 3.5 m), parallel to wall A and 1.097 m from its face, from x = 1 m to x = 5 m |
| Spot 1 | A generic spot, 800 lm, beam 15 degrees and field 25 degrees (full angles), 3000 K, at full output. It sits on the track at x = 3.465 m and aims at the chart |
| Opposite the chart | A viewpoint at (3, 3, 1.6): eye height, 3 m from wall A, looking straight at it. It has a 50 degree vertical field of view and an exposure of EV100 8 |

## The numbers behind it

- **The track's distance from the wall** is the Canadian Conservation Institute's
  (ADR 0006): (ceiling height − 1.6 m) × tan 30° = 1.9 × 0.5774 = 1.0970 m, written to the
  millimetre.
- **The spot slides 0.465 m from the point opposite the work**, so that its aim is 30 degrees from
  vertical. The aim runs from the spot at (3.465, 1.097, 3.5) to the centre of the work's face at
  (3.0, 0.04, 1.5): it drops 2.0 m over a horizontal 1.1548 m.
  - The horizontal distance is √(1.057² + 0.465²), where 1.057 m is the track's distance from the
    face less the stand-off.
  - So the aim is atan(1.1548 / 2.0) = 30.001 degrees from vertical.
  - The exact slide is 0.4648 m. The editor's `add_spot_for` (M1-25) must land within 1 mm of it.
- **On the work's centre**, the light arrives from 2.309 m away, at 62.8 degrees from the work's
  normal (cos 0.4577), so illuminance = I × 0.4577 / 2.309². That is about 460 lux if the spot
  spread its flux evenly over its 25 degree field (5,370 cd), and more with a real peak.
- **EV100 8** puts the white patch, lit at 450 to 900 lux, below the top of the exposure range:
  1.2 × 2⁸ ≈ 307 nits. It is an estimate, made before cs-render (M1-17) can render the scene.

## Choices the task left open

- **The walls stand outside the outline**, each with face A on the outline. So the room's
  surfaces (`geom::room_mesh`) and the faces works hang on (`geom::wall_face_frame`) are the same
  planes, and the chart's back is 0.04 m from the visible wall. Each wall is as long as the
  room's side. Their corners therefore leave 0.1 x 0.1 m gaps outside the room in plan, which
  the room's closed surfaces hide. Joined corners arrive with M2-01.
- **The work is a print on paper**: preset `print-on-paper`, preset 11 of ADR 0003 ("Ink,
  graphite or print on paper"), responsivity medium. Preset ids are not defined yet; M2-10 adds
  them as data files.
- **No glazing:** the framing says `"glazing": "none"`. Preset 11 defaults to anti-reflective
  glass, and the lux tests of milestone 1 expect the bare work.
- **Ids** are version 7 UUIDs from 12 November 2026, numbered in the order a curator would make
  the things: the work, then the room, walls, chart, track, spot and viewpoint.

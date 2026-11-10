# 0003. Painting materials: a layered model, twelve presets, a four-question import

- Status: proposed
- Date: 2026-11-10
- Decides: question 3 of the planning brief (which properties change how light reflects off a
  painting, how to model them, where the values come from, what a curator is asked on import);
  also the conservation classes and the default frames

## Context

The brief asks for "extremely accurate" reflection off paintings, and suggests asking for "the
type of paint used". For most works the only data available is one photograph (taken under
diffuse, often cross-polarised light, so it approximates the diffuse colour), the medium line
from the museum record, and the size.

What changes the appearance under gallery light, from most to least:

1. **Glazing.** Plain glass reflects about 8% of the light at normal incidence (two surfaces).
   A mid-tone work at 200 lux facing a white wall at 200 lux gains a reflected veil of about 30%
   of its own luminance; anti-reflective museum glass (under 1%) cuts that to about 4%.
2. **Finish (gloss).** From satin to gloss varnish, 60° gloss moves from about 10 to about 60
   gloss units: this sets where glare appears for a given fixture and viewer.
3. **Gilding** on gold-ground panels and in frames.
4. **Relief** (impasto, canvas weave) under raking light.
5. **Support** (canvas, panel, paper, copper).
6. **Medium.** Almost nothing optically: the surface reflectance of every plausible medium is
   3.4 to 4.7%, and a varnish's refractive index changes appearance by about 1 ΔE00 at most
   (Berns and de la Rie, 2003). The colour is already in the photograph.

So the "type of paint" matters mostly through the finish it usually has, and pigment-level data
would add nothing a curator could see.

## Options

1. **Ask for detailed technical data** (binder, varnish resin, pigments). Curators rarely have
   it for loans, it slows import, and it barely changes the image. Rejected.
2. **Measure each work** (gloss meter, RTI, 3D scan). Not available for catalogue works, and
   open scan data of famous paintings was not found. For 1.0, accept measured maps when a curator
   has them.
3. **A layered physically based model with presets by medium and finish**, defaults derived
   from the medium line. **Chosen.**

## Decision

### The model

One parameter set, `PaintingMaterial`, evaluated by both the calculation engine and the GPU
tiers. The engine implements the relevant subset of **OpenPBR Surface** (v1.1); the real-time
tiers implement the glTF-equivalent layers (base, clearcoat, sheen, specular, IOR). A test
checks that the two agree on sampled directions.

| Layer | Meaning | Parameters |
| --- | --- | --- |
| Base | The paint, colour from the photograph converted to linear albedo (clamped to 0.02 to 0.90) | roughness; specular weight (0 when varnished: the paint/varnish boundary is index-matched and reflects almost nothing, and leaving it on adds a false second highlight) |
| Coat | Varnish | weight; roughness; IOR 1.52 (1.5 in the real-time tier, as glTF fixes it); darkening off (the photograph already shows the varnish's saturation and yellowing); normal is a smoothed copy of the base normal |
| Metal | Gilding, inside a mask (1.0) | gold F0 (1.059, 0.773, 0.307), edge tint (1.001, 0.985, 0.523); roughness 0.21 burnished, 0.29 oil-gilt |
| Fuzz | Pastel and charcoal (optional) | sheen colour and roughness |
| Glazing | A separate thin-walled glass sheet in front of the work, part of the frame | IOR; reflectance scale (0.12 for anti-reflective); transmittance colour |

Roughness is GGX perceptual roughness *r* (α = *r*²). Finish words map to roughness through a
simulated ISO 2813 gloss meter (60° geometry, *n* = 1.5): *r* 0.15 ≈ 59 GU, 0.20 ≈ 39,
0.30 ≈ 17, 0.40 ≈ 8, 0.50 ≈ 4. The engine reproduces this table in a test, so the mapping is
checked rather than copied.

**Relief:** not inferred from photographs (museum photographs are lit to remove shading, and
single-image methods are qualitative). In 1.0, a generic canvas or paper weave by support,
labelled "generic", and measured height or normal maps where a curator has them.

### Presets

Basis: **[B]** derived from measurements through a model; **[C]** an educated guess, to be
replaced when measurements are found.

| # | Preset | Base *r* | Coat *r* | Glazing default | Sensitivity | Basis |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Oil on canvas, gloss varnish | 0.45 | 0.18 | none | low | B |
| 2 | Oil on canvas, satin varnish | 0.45 | 0.30 | none | low | B |
| 3 | Oil on canvas, matte varnish | 0.45 | 0.45 | none | low | C |
| 4 | Oil, unvarnished | 0.45 | none | none | low | B |
| 5 | Oil on panel or copper, varnished | 0.45 | 0.15 (copper 0.12) | none | low | C |
| 6 | Egg tempera on panel (gold ground in 1.0) | 0.65 | none (0.30 if varnished) | none | low | B |
| 7 | Acrylic, unvarnished | 0.40 | none | none | low | C |
| 8 | Watercolour on paper, mat | 0.85 | none | anti-reflective glass | medium | B/C |
| 9 | Gouache on paper | 0.85 | none | anti-reflective glass | medium | B/C |
| 10 | Pastel or charcoal | 0.95, fuzz | none | anti-reflective glass | medium | C |
| 11 | Ink, graphite or print on paper | 0.80 | none | anti-reflective glass | medium (high for some inks) | C |
| 12 | Fresco (detached) | 0.85 | none | none | low | C |

Glazing kinds: none; float glass (8.2% reflectance, about 89% transmittance, slightly green);
low-iron glass (8.2%, 91%); acrylic (7.5%); anti-reflective glass (under 1%, about 97%);
anti-reflective acrylic (under 1.5%); non-glare etched glass (diffusing, roughness about 0.25).
Transmittances are vendor figures. **The lux meter reads at the work's surface, behind the
glazing,** because that is the light that causes damage.

### Import: four questions, all prefilled

1. **True size** (width × height, cm or inches; depth for stretched canvases). Parsed from text
   such as "73 × 92 cm" when pasted or imported from a checklist.
2. **Medium preset**, picked automatically from the medium line ("oil on canvas" → preset 1).
3. **Finish**: gloss, satin, matte or unvarnished.
4. **Glazing**, defaulted by the preset.

Collapsed and prefilled: sensitivity class, frame style, mat. Never asked: binder, varnish
chemistry, pigments.

### Conservation classes

Defaults from CIE 157:2004 (taken from secondary summaries; the standard is paywalled):

| Class | Maximum illuminance | Yearly exposure | Examples |
| --- | --- | --- | --- |
| Irresponsive | no limit | no limit | stone, metal, glass |
| Low | 200 lux | 600,000 lux-hours | oil and tempera paintings, fresco |
| Medium | 50 lux | 150,000 lux-hours | watercolours, drawings, prints |
| High | 50 lux | 15,000 lux-hours | silk, newsprint, felt-tip ink |

Guidance differs between institutions (Museums Galleries Scotland: 50 lux and 100,000 lux-hours;
200 lux and 450,000 lux-hours, assuming 2,184 opening hours a year). The project chooses a
**policy** (CIE 157 by default, or a named alternative, or custom values), and each work can
override its own limits. Yearly exposure = mean illuminance × the project's opening hours.

### Fixtures

The library ships real museum fixtures with their manufacturers' IES files (licences checked per
file, recorded in `docs/dependencies.md`), plus generic fixtures defined by lumens and beam
angle. Real spotlights are strong (a 6° spot of about 680 lm reaches about 40,000 cd, 2,500 lux
at 4 m), so a placed fixture starts dimmed, and **"dim to target"** sets its dimming so the mean
illuminance on its work equals the work's target. Defaults: 3000 K; colour rendering and UV are
recorded for the schedule but not rendered (ADR 0002).

### Frames

Parametric frames: a 2D profile swept around the work with mitred corners; member width, depth,
rebate (5 to 10 mm overlap onto the work); a finish material; optional mat (border 4 to 10 cm,
bottom 0.6 to 2.5 cm wider, window overlapping the work by 3 to 6 mm) and glazing. MVP styles
(dimensions are typical, not verified against a frame history):

1. Gilt cassetta, width 10 cm, depth 5 cm
2. Dutch ebonised reverse, 9 cm, 5 cm
3. Nineteenth-century hollow gilt, 10 cm, 6 cm
4. Modern oak strip, 2 cm, 4 cm
5. Floater, 1 cm gap
6. Works on paper: aluminium strip 2 cm, 8 cm mat (bottom +1.5 cm), anti-reflective glass

Carved ornament (Louis XV) is a 1.0 style, as normal-map decals rather than geometry.

### Shipped data

Only data under CC0 or similar ships with the app: physicallybased.info values and parts of the
EPFL RGL database. MERL (CC BY-SA, with older research-only notices) and UTIA (research only)
are used to fit parameters, never shipped.

## Consequences

- Import stays under a minute, and the answers change what a curator sees (gloss, glare, the
  veil on glass) rather than invisible details.
- Accuracy is honest: colour is as good as the photograph; reflection is physically based with
  stated, partly estimated parameters, marked [B] or [C] in the code next to each number.
- Varnish yellowing, retouching and relief are only as visible as the photograph shows them.

## Revisit

When measured gloss for museum paintings or open surface scans are found; at the 1.0
decomposition for gilding masks, relief and the frame library.

## Sources

- Berns and de la Rie (2003), varnish refractive index and appearance; de la Rie (1987); Elias
  et al. (2010): https://www.nmr.mgh.harvard.edu/~cmagnain/pdf/Elias2010.pdf
- ISO 2813:1994 sample (gloss geometry):
  https://cdn.standards.iteh.ai/samples/7810/2ac0b6e5b66043eaa1e251f26637a756/ISO-2813-1994.pdf
- OpenPBR Surface: https://academysoftwarefoundation.github.io/OpenPBR/
- glTF clearcoat: https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_clearcoat/README.md
- CCI, LED lighting for museums: https://www.canada.ca/en/conservation-institute/services/conservation-preservation-publications/technical-bulletins/led-lighting-museums.html
- CCI, light as an agent of deterioration: https://www.canada.ca/en/conservation-institute/services/agents-deterioration/light.html
- Museums Galleries Scotland, conservation and lighting:
  https://www.museumsgalleriesscotland.org.uk/advice-article/conservation-and-lighting/
- Tru Vue glazing figures: https://tru-vue.com/wp-content/uploads/sites/5/2025/11/Custom_Framing_Product_Portfolio_Flyer-English.pdf
- EPFL RGL material database: https://rgl.epfl.ch/pages/lab/material-database

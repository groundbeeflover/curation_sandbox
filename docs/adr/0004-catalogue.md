# 0004. The catalogue: five open-access museums, a bundled starter set, downloads on demand

- Status: proposed
- Date: 2026-11-10
- Decides: question 4 of the planning brief (sources, licences, resolution, metadata, storage
  and delivery)

## Context

The app ships with a catalogue of famous paintings and drawings that curators can hang at their
true size. The rules already set: images are public domain or CC0, each with a credit line. The
app is Apache-2.0 and free, so there is no budget for hosting beyond free tiers, and everything
shipped must be redistributable for any use, including commercial use by museums. Each work
needs its true size (to hang at scale), its medium (to choose a material, ADR 0003), and artist,
title, date, institution and credit line. Integrated GPUs share system memory, so texture memory
is limited.

## Sources

Ranked by fitness for this purpose. None needs an API key. Resolutions were measured on sample
records.

| Rank | Source | Licence of images | Access | Resolution seen | Size and medium fields |
| --- | --- | --- | --- | --- | --- |
| 1 | Cleveland Museum of Art | CC0 (`share_license_status`) | REST API; data dump on GitHub | full TIFF, 14.6 px/mm | framed and unframed, in metres; `technique`; `creditline` |
| 2 | The Metropolitan Museum of Art | CC0 (`isPublicDomain`) | REST API (80 requests/s); CSV on GitHub | 8.7 px/mm | `measurements[]`, overall and framed, cm; `medium`; `creditLine` |
| 3 | National Gallery of Art, Washington | CC0 | daily CSVs on GitHub; IIIF at full resolution | 3365 × 4332 px sample | painted surface against overall; medium; credit line |
| 4 | Rijksmuseum | Public Domain Mark | Linked Art API; IIIF | up to 14,645 px (tiles must be stitched for full size) | typed dimensions and materials |
| 5 | SMK (National Gallery of Denmark) | Public Domain Mark | REST API; IIIF up to 50k px | 7.8 px/mm | unframed and framed (*netto*, *brutto*) |

Also usable: the J. Paul Getty Museum (CC0, IIIF to 30k px; rights are per image and image IDs
change), and Wikimedia Commons for gap-filling (for example Google Art Project gigapixel files
and Nationalmuseum Sweden), each file reviewed by hand.

Excluded: the National Gallery, London (images CC BY-NC-ND); the Van Gogh Museum and the
Mauritshuis (non-commercial on their own sites); the Art Institute of Chicago (public-domain
images capped at 1686 px); Italian state museums (their heritage code restricts commercial
reproduction). Europeana is used only to discover works, not as an image source.

## Legal rule

Faithful photographs of two-dimensional public-domain works carry no new copyright in the United
States (*Bridgeman v. Corel*) or the EU (Article 14 of the 2019 Digital Single Market
Directive). The UK did not implement Article 14 and its position remains uncertain; relying on
the source's own CC0 or Public Domain Mark covers that.

A work enters the catalogue only if **all** of these hold, checked by the pipeline:

1. The source's own record marks the image CC0 or Public Domain (the raw rights field is stored).
2. The artist died at least 81 full years ago (life + 80 in Spain, the longest common term in
   Europe; in 2026, died in 1945 or earlier).
3. The work was made at least 96 years ago (the United States' 95-year term; in 2026, 1930 or
   earlier).

Both year limits move forward every 1 January. This is common practice, not legal advice.

## Resolution

A viewer standing 0.3 to 0.5 m from a work, on a 1440p or 4K display with a 60° vertical field
of view, needs about 2.5 to 6.2 texels per millimetre for each screen pixel to show at least one
texel. A 100 × 80 cm painting therefore needs about 4200 × 3360 px at 1440p and 0.5 m; the
sources give 3 to 15 px/mm, which covers it for most works.

| Tier | Rule | When |
| --- | --- | --- |
| Standard | about 4 texels/mm, long edge capped at 8192 px | MVP |
| HD | up to 8 texels/mm; works above 8192 px split into tiles with virtual texturing | 1.0 |

## Storage and delivery

- **Format:** KTX2 containers with Basis Universal UASTC and Zstandard, transcoded to BC7 when
  loaded. Every work has a full mip chain. GPU memory per work at BC7 with mips: 5.3 MiB at
  2048², 21 MiB at 4096², 85 MiB at 8192².
- **Residency:** the renderer keeps each work at the mip level its on-screen size needs, within
  the tier's texture budget (ADR 0002). Whole mip levels in the MVP; tiles in 1.0 for HD.
- **Starter set:** 40 works (about 150 to 250 MB) bundled in the installer, so the app is useful
  offline from the first launch.
- **The rest:** downloaded on demand, one work at a time, from **GitHub Releases** (one asset per
  work, no total-size or bandwidth cap, files under 2 GiB) and checked against the manifest's
  hash. A copy of each catalogue release is archived on Zenodo with a DOI. The app never loads
  images from museum servers at runtime.
- **In the repository:** the selection list, the pipeline and the manifest are committed; the
  images are not (they live in releases). Tests use a few tiny committed images.

Sizes are estimates until the pipeline measures them: 300 works at the Standard tier about
2.4 GB, the HD tier about 8 to 10 GB.

## Pipeline

A scripted, reproducible pipeline in `tools/catalogue/` (Python, with its dependencies locked):

1. `catalogue/selection.toml`, written by hand: which works, from which source, and why.
2. A source adapter per museum fetches the record and the master image.
3. **Rights check:** reject anything not explicitly cleared by the rule above.
4. **Dimensions:** normalise units, choose the unframed painted surface (or the sheet for works
   on paper), check the aspect ratio against the image (within 2%), record a confidence.
5. Convert to linear colour from the embedded ICC profile, crop to the painted surface, resize
   to the tier, encode KTX2, make thumbnails.
6. Write `catalogue/manifest.json` and the credits file.

**Manifest fields:** id; title; artist, with birth and death years; date; institution;
accession number; source URLs and retrieval time; licence URI, the raw rights field and how
rights were established; credit line; width, height and depth in mm, which surface was measured,
framed size, the original dimension text and a confidence; medium text, support and material
preset (ADR 0003); hashes of master and derivatives; texels per mm.

Known traps in museum data: framed and unframed sizes mixed in one field; inches first (Met),
metres (Cleveland), mm (SMK notes); height and width order not always kept; notes that change
the meaning (a canvas cut down after it was measured); works on paper with sheet, image and
plate sizes.

## Selection

- **MVP, 40 works** from the Met, Cleveland and NGA: 25 paintings, 10 works on paper, 5 from
  outside Europe; including one work over 3 m, a tondo, a triptych and a miniature, so that every
  hanging and framing case is exercised.
- **1.0, about 300 works:** roughly 60% paintings, 30% works on paper, 10% scrolls and
  miniatures; at least 20% from outside Europe; sizes from under 20 cm to over 2 m.

## Consequences

- Every image in the catalogue can be redistributed for any purpose, and its provenance is
  recorded and checked by a script, not by memory.
- Twentieth-century modernism is mostly absent; curators bring those works in as imports.
- A second, smaller repository of release assets grows with the catalogue; GitHub Releases has
  no total-size cap, but a mirror (Cloudflare R2 or Hugging Face) is planned if it ever imposes
  one.

## Revisit

Every 1 January (the year limits), and when the 1.0 catalogue is decomposed.

## Sources

- The Met Open Access API: https://metmuseum.github.io/
- Cleveland Museum of Art Open Access API: https://openaccess-api.clevelandart.org/
- NGA open data: https://github.com/NationalGalleryOfArt/opendata
- Rijksmuseum data services: https://data.rijksmuseum.nl/docs/
- Getty Museum collection API: https://data.getty.edu/museum/collection/docs/
- Art Institute of Chicago API: https://api.artic.edu/docs/
- National Gallery terms of use: https://www.nationalgallery.org.uk/terms-of-use
- Wikimedia Commons on PD-Art: https://commons.wikimedia.org/wiki/Commons:When_to_use_the_PD-Art_tag
- GitHub Releases limits: https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
- KTX artist guide: https://github.com/KhronosGroup/3D-Formats-Guidelines/blob/main/KTXArtistGuide.md

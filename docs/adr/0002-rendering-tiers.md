# 0002. Rendering strategy: a calculation engine and three display tiers

- Status: proposed
- Date: 2026-11-10
- Decides: question 2 of the planning brief (reconcile "everything ray traced" with "smooth on
  low-end machines"; tiers, frame-rate targets, minimum specifications)
- Depends on: [0001](0001-engine.md) for the implementation

## Context

The brief wants physically accurate, ray-traced light and also high frame rates on modest
machines. Those conflict: real-time path tracing needs a fast ray-tracing GPU, and curators'
office laptops mostly have Intel integrated graphics without it. The Steam survey of October
2026 puts hardware ray tracing at roughly 69% of gamers' GPUs; museum laptops will be lower.

What a curator actually checks is mostly **direct light**: how many lux a spotlight puts on a
work, the shape of its beam, the shadow of the frame. Bounced light from walls adds a modest
fraction in a gallery with mid-grey walls. Ray tracing matters most for soft shadows, for
reflections in glazing and varnish, and for the final stills.

Lighting-design tools (DIALux, Relux, AGi32) separate the **calculation** of illuminance from
the **display** of the scene. That separation is what makes their numbers trustworthy while
their viewports stay fast.

## Options

1. **One real-time path tracer for everything.** Accurate, but needs a ray-tracing GPU for every
   user, and noise in the live view. Rejected: it excludes the first users' hardware.
2. **Rasterisation only, with baked lighting.** Fast everywhere, but no reference to test
   against, and the lux numbers are only as good as the bake.
3. **A calculation engine plus display tiers.** A path tracer computes lux and is the reference;
   the display tiers trade quality for speed and are tested against it. **Chosen.**

## Decision

### The calculation engine (always present)

A **CPU Monte Carlo path tracer built on Embree** (Apache-2.0) with next-event estimation and
multiple importance sampling. It is the ground truth, and it:

- computes every **lux reading** (the virtual lux meter, the per-work readings, the hanging
  list's lux column) from the scene description, never from pixels on screen;
- renders **reference images** for the golden-image tests of every other tier;
- **bakes indirect light** (lightmaps for walls, floors and ceilings; probes for everything
  else) for the Standard tier;
- renders **stills** on machines without hardware ray tracing.

It runs on any x86-64 or ARM64 CPU, deterministically for a given seed, and in CI without a
GPU. The lux meter shows the live tier's estimate at once and replaces it with the engine's
value, with its statistical uncertainty, within about a second.

### Display tiers

| Tier | Technique | Minimum specification | Reference hardware | Target |
| --- | --- | --- | --- | --- |
| **Standard** (default, MVP) | Rasterised. Direct light from every fixture evaluated exactly per pixel from its IES distribution; soft shadow maps cached while lights are still; indirect light from the engine's bake (lightmaps on architecture, probes on works and frames); reflection probes for glazing and varnish. Jittered frames accumulate while the camera is still. | Vulkan 1.3; 2 GB of video memory, shared allowed; no ray tracing needed (Intel Iris Xe, AMD integrated since Vega, GTX 1050) | Radeon 780M; an Intel Iris Xe laptop | 780M: 60 fps at 1080p on the 50-work gallery. Iris Xe: 30 fps at 1080p. |
| **Ray-traced** (1.0) | Hybrid. Ray-traced soft shadows from every fixture, ray-traced reflections in glazing and varnish, many-light sampling, cached indirect light, a temporal denoiser written in-house. Converges to the Path-traced result when the camera stops. | `VK_KHR_ray_query`; 6 GB of video memory (RTX 2060, RX 6600, Arc A-series) | RTX 2060; an RTX 40-series card | RTX 2060: 60 fps at 1080p. RTX 40: 60 fps at 1440p. |
| **Path-traced** (stills; GPU version in 1.0) | Progressive path tracer: the calculation engine on the CPU in the MVP; a GPU version using ray queries in 1.0. Open Image Denoise for the final image (ADR 0005). | Any x86-64 CPU; for the GPU version, `VK_KHR_ray_query` | Ryzen 7 8840U (8 cores); RTX 2060 | See the still budgets below. |

The Radeon 780M supports ray queries but cannot sustain per-frame ray tracing at 1080p; it uses
the Standard tier, and its ray tracing speeds up bakes and the GPU path tracer.

### Accuracy, stated as tolerances

| Check | Tier | Tolerance |
| --- | --- | --- |
| Analytic cases: inverse-square, cosine law, a uniform-cone fixture, an IES fixture's total lumens | Engine | within 1% (and within 3 standard errors) |
| White furnace: a white diffuse object in uniform light | Engine | within 1% |
| Lux on a grid over each work, against the engine | Standard | direct light within 2%; mean within 5%; 95th percentile within 10% |
| Lux on a grid over each work, against the engine | Ray-traced | within 5% while moving; within 1% once converged |
| Image against the engine's reference image (NVIDIA FLIP, mean) | Standard | at most 0.10 |
| Image against the engine's reference image (NVIDIA FLIP, mean) | Ray-traced | at most 0.05 |
| GPU path tracer against the CPU engine, same scene and seed policy | Path-traced | lux within 1%; FLIP at most 0.02 |
| The engine against Radiance on CIE 171 test scenes we can reconstruct | Engine | within 3% (a spike decides which scenes; the standard is paywalled) |

Published context: CIE 171's measured test rooms carry about ±13% measurement uncertainty, and
DIALux evo reports within 1% on point-source cases. Our tolerances are tighter than what users
can measure, so that regressions show up before users notice them.

### Performance budgets

Each budget has a benchmark scene and a benchmark that measures it ([roadmap](../roadmap.md)).
"Reference CPU" is the Ryzen 7 8840U; "50-work gallery" is the benchmark scene
`gallery-50`, generated by `cs-bench generate gallery-50`: two rooms, 50 framed works with 4k
textures, 40 spotlights.

| Budget | Measure | Standard tier | Ray-traced tier |
| --- | --- | --- | --- |
| Frame time, 50-work gallery, 1080p | 95th percentile over a scripted 20 s walk | ≤ 16.7 ms on the 780M; ≤ 33 ms on Iris Xe | ≤ 16.7 ms on the RTX 2060 |
| Frame time, empty room, 1080p | same | ≤ 8 ms on the 780M | ≤ 8 ms on the RTX 2060 |
| GPU memory, 50-work gallery | allocator budget in use | ≤ 1.5 GB | ≤ 3 GB |
| Process memory, 50-work gallery | peak resident set | ≤ 2 GB | ≤ 2.5 GB |
| Open the 50-work gallery | to first frame, textures already cached | ≤ 3 s | ≤ 3 s |
| Start the app | to first frame, shader cache warm | ≤ 2 s | ≤ 2 s |
| Indirect light after an edit | rebake on the reference CPU | preview ≤ 2 s; full ≤ 30 s, in the background, with no frame over budget | n/a (dynamic) |
| Lux on every work after an edit | engine value with ≤ 2% standard error | ≤ 1 s | ≤ 1 s |
| Still, 1080p, 50-work gallery | 64 samples per pixel + denoise | ≤ 60 s on the reference CPU | ≤ 10 s on the RTX 2060 (GPU tracer) |
| Still, 4K, 50-work gallery | 256 samples per pixel + denoise | ≤ 15 min on the reference CPU | ≤ 2 min on the RTX 2060 |

Frame rates are hardware-specific and are measured locally through the `/perf-check` skill on
the owner's machines. CI measures memory and runs every benchmark scene for correctness on
software Vulkan, but does not time it.

### Colour and exposure

Render in linear RGB (Rec. 709 primaries) with luminance in nits. A physical camera exposure
(EV100) with an automatic mode; tone mapping with Khronos PBR Neutral, which keeps the colours
of products (here, artworks) as close as possible; AgX as an optional look for stills.
Fixtures carry a colour temperature, converted to RGB; each may store an emission spectrum, so
that colour rendering (CRI, TM-30) and damage weighting can be computed later without spectral
rendering. Spectral rendering is deferred: museum LEDs are broadband, and pigment metamerism
would need spectral scans of the works, which curators do not have.

### Testing in CI

- **Numeric tests on the CPU**, no GPU: the engine's analytic, furnace and lux tests, with
  fixed seeds and statistical tolerances.
- **Image tests on Mesa lavapipe** (software Vulkan, which supports ray queries) on the Linux
  runner, with the Mesa version pinned so references do not drift; images of 128 to 256 pixels
  and low sample counts, because lavapipe is slow. Windows CI builds and runs the CPU tests; it
  runs image tests only if a pinned lavapipe build proves reliable there (a spike decides).
- **Golden images** are compared with NVIDIA FLIP (BSD-3) and a mean-error check, with
  references kept per driver where drivers legitimately differ.
- **Real-GPU checks** (frame times, the 780M and the RTX 2060) run locally through
  `/perf-check`. A nightly self-hosted runner on the owner's laptop is optional, never triggered
  by pull requests from forks.
- External renderers (Radiance, Mitsuba 3, pbrt-v4) are used for cross-checks at test time
  only, never shipped.

## Consequences

- Lux readings are as accurate on an office laptop as on a gaming PC; only the picture differs.
- Two renderers must be kept in agreement (engine and Standard tier), and later three. The
  golden-image and lux-grid tests make disagreement visible in every pull request.
- The Standard tier is a first-class product, not a fallback: most first users will see only it.
- NVIDIA's RTX SDKs (RTXGI, RTXDI, NRD) are licensed in a way that forbids use in open-source
  projects, so ReSTIR, probe GI and the real-time denoiser are written in-house for the
  Ray-traced tier. That is why the tier is in 1.0, not the MVP.
- Errors the engine shares with every tier (treating fixtures as point sources, the albedo of a
  photographed painting) are invisible to these tests. A spike compares the engine with a real
  lux meter in a real gallery.

## Revisit

When the Ray-traced tier is decomposed (end of milestone 4), with frame times measured on the
RTX 2060 and the 780M; and if curators' machines turn out to have ray-tracing GPUs more often
than assumed.

## Sources

- Steam Hardware Survey, GPUs: https://store.steampowered.com/hwsurvey/videocard/
- DIALux evo calculation method and validation:
  https://www.dialux.com/fileadmin/documents/DIALux_evo-_New_calculation_method.pdf
- Radiance validated against CIE 171:
  https://content.gitbook.com/content/fKJnzF2TwHA3APPbiCpF/blobs/bxotm0zgXgVtLVOBOx7w/CIE171_Testing.pdf
- Lavapipe status (ray queries, ray tracing pipelines):
  https://www.slideshare.net/slideshow/current-state-of-lavapipe-mesa-s-software-renderer-for-vulkan/276099337
- GitHub runner images (no Mesa installed by default):
  https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md
- GitHub Actions GPU runner pricing: https://docs.github.com/en/billing/reference/actions-runner-pricing
- NVIDIA FLIP: https://github.com/NVlabs/flip
- NVIDIA RTX SDKs licence:
  https://developer.download.nvidia.com/gameworks/NVIDIA-RTX-SDKs-License-23Jan2023.pdf
- Khronos PBR Neutral tone mapper: https://github.com/KhronosGroup/ToneMapping
- Spectral against RGB rendering: https://momentsingraphics.de/SpectralRendering3Results.html

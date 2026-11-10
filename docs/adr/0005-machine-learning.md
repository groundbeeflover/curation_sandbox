# 0005. Machine learning: a denoiser only, no generative upscaling

- Status: proposed
- Date: 2026-11-10
- Decides: question 5 of the planning brief (super-resolution, denoising, frame upscaling,
  local inference)

## Context

The brief asks whether machine learning could "improve the quality of uploaded images". Three
uses of ML are relevant to this product:

1. **Super-resolution** of imported images, which are often small.
2. **Denoising** the path tracer's stills, so a still converges in seconds rather than minutes.
3. **Frame upscaling** (render fewer pixels, reconstruct more), so integrated GPUs keep their
   frame rate.

Constraints: the app is Apache-2.0, so code, runtimes and trained weights must allow
redistribution and commercial use (museums using the app is commercial use). It runs on AMD,
Intel and NVIDIA GPUs, including integrated ones, on Windows and Linux. Curators need faithful
reproductions: the US digitisation guidelines (FADGI, 3rd edition, 2023) call adding, removing
or changing image detail "not acceptable" and require any processing to be documented.

## Options

### Super-resolution

| Option | Code / weights licence | Fidelity | Verdict |
| --- | --- | --- | --- |
| Diffusion upscalers (SUPIR, StableSR, SeeSR) | SUPIR and StableSR non-commercial; SeeSR Apache-2.0 but needs a Stable Diffusion base model | Invents brushwork, craquelure and weave | Ruled out |
| GAN upscalers (Real-ESRGAN) | BSD-3; weight licence not stated separately | Invents texture | Ruled out for the MVP |
| Transformer regressors (SwinIR, HAT, DAT, Swin2SR) | Apache-2.0 code; only Swin2SR states Apache-2.0 weights | Least invention, slow on integrated GPUs | Candidate after 1.0, opt-in |
| No super-resolution; faithful resampling and an effective-resolution warning | n/a | Exact | **Chosen** |

### Denoising

| Option | Licence | Hardware | Verdict |
| --- | --- | --- | --- |
| Intel Open Image Denoise 2.5 | Apache-2.0 code and weights | Every CPU; CUDA, HIP and SYCL back ends | **Chosen**, CPU in the MVP |
| NVIDIA OptiX denoiser, NRD | NVIDIA licences forbid use that subjects them to an open-source licence | NVIDIA only | Ruled out |
| Classical filters (SVGF, A-trous) | Our own code | Any GPU | For the real-time ray-traced tier, if needed (ADR 0002) |

### Frame upscaling

| Option | Licence | Hardware | Verdict |
| --- | --- | --- | --- |
| AMD FSR 3.1 upscaler | MIT (only this part of the FidelityFX SDK) | Any vendor, Vulkan | Optional in 1.0 for integrated GPUs |
| AMD FSR 4 | Binary only, no reverse engineering | RX 7000/9000 only | Ruled out |
| Intel XeSS 3 | Binary, Intel licence | Windows only | Ruled out |
| NVIDIA DLSS | NVIDIA RTX SDK licence | RTX only | Ruled out |
| Accumulating jittered frames while the camera is still; MSAA or TAA while moving | Our own code | Any | **Chosen** for the MVP |

A gallery is mostly still: the camera stops in front of a work far more than it moves. Averaging
jittered frames whenever the camera is still converges to a clean, aliasing-free image without
any ML.

## Decision

**MVP**

- Ship **Open Image Denoise on the CPU** for path-traced stills, with albedo and normal guide
  images. Adds an estimated 15 to 35 MB.
- **No super-resolution.** Keep every imported image byte for byte, with its hash. Downsample
  in linear light into mip chains; magnify with a bicubic filter; never sharpen.
- **Report effective resolution.** At true size, an image of *w* pixels across a work *W* mm wide
  has *w / W* pixels per millimetre. A viewer with normal acuity (one arcminute) at distance *d*
  mm needs about 3438 / *d* pixels per millimetre: 6.9 at 0.5 m, 3.4 at 1 m. The import dialog
  and the work's inspector say how close a visitor can stand before the image looks soft, and
  warn below 1 m.
- **Anti-aliasing** by accumulating jittered frames while the camera is still, and MSAA while it
  moves.
- **No ML runtime** (ONNX Runtime, ncnn and so on) in the app.

**Rules that follow**

- The lux meter and every accuracy test read the **un-denoised** radiance. A test checks that a
  lux reading is identical with the denoiser on and off, and that the denoised image's mean over
  each work is within 2% of a high-sample reference.

**1.0**

- OIDN's GPU back ends, probing the device and falling back to the CPU (Blender disabled
  OIDN on HIP for a while after crashes on old integrated-GPU drivers).
- The FSR 3.1 upscaler as an optional mode for integrated GPUs.
- Classical image tools: colour correction from a colour target in the photograph, a
  four-corner perspective and crop tool.

**After 1.0, only if curators ask:** opt-in super-resolution with a conservative regression
model whose weights are Apache-2.0 or MIT, labelled "enhanced, not original" on screen, in the
project file and in every export, and never used for measurement.

## Consequences

- Imports stay faithful, and the app never shows detail that is not in the source.
- Low-resolution imports look soft up close. The effective-resolution warning makes that a
  known, explained limit rather than a surprise.
- One ML dependency (OIDN) with its own runtime; no model downloads, no GPU-vendor ML stacks.
- Checks: the denoiser has a golden-image test and the lux-invariance test above; the
  effective-resolution calculation has a unit test against the worked numbers above.

## Revisit

At the 1.0 planning, with curator feedback on imported-image quality; and if a
conservative super-resolution model with redistributable weights appears.

## Sources

- Open Image Denoise: https://github.com/RenderKit/oidn and weights
  https://github.com/RenderKit/oidn-weights
- FidelityFX SDK licence (FSR 3.1 MIT, FSR 4 binary):
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/main/docs/license.md
- DLSS licence: https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
- XeSS licence and platform support: https://raw.githubusercontent.com/intel/xess/main/LICENSE.txt
- FADGI Technical Guidelines, 3rd edition (2023):
  https://www.digitizationguidelines.gov/guidelines/FADGITechnicalGuidelinesforDigitizingCulturalHeritageMaterials_ThirdEdition_05092023.pdf
- SUPIR licence: https://github.com/Fanghua-Yu/SUPIR
- ONNX Runtime execution providers: https://onnxruntime.ai/docs/execution-providers/
- Full research notes were made for this record; their sources are listed above.

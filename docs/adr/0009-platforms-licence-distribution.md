# 0009. Platforms, licence and distribution

- Status: accepted (the owner decided platforms and licence on 2026-11-10); distribution proposed
- Date: 2026-11-10
- Decides: question 8 of the planning brief (operating systems, open source and licence) and how
  builds reach curators

## Decisions made by the owner

- **Operating systems:** Windows and Linux for the MVP and 1.0; macOS later. The engine choice
  keeps macOS possible (Vulkan through MoltenVK or KosmicKrisp, Qt on macOS), but no macOS work
  happens before 1.0.
- **Licence:** open source under **Apache-2.0**: permissive, with an explicit patent grant, and
  the licence most of the graphics libraries we depend on use. The repository stays private
  until the owner chooses to publish it.

## Consequences for the code

- Every dependency must allow redistribution under Apache-2.0: MIT, BSD, zlib, BSL, Apache-2.0,
  or LGPL when dynamically linked (Qt). No GPL, no non-commercial or research-only data, no
  NVIDIA RTX SDKs (their licence forbids use that subjects them to an open-source licence).
  `docs/dependencies.md` lists each one with its version and licence, and CI checks that the list
  matches the vcpkg manifest.
- Catalogue images and other shipped data follow ADR 0004 and ADR 0003.
- Source files carry an SPDX header (`// SPDX-License-Identifier: Apache-2.0`).

## Supported systems

| System | MVP | Notes |
| --- | --- | --- |
| Windows 10 22H2 and Windows 11, x86-64 | yes | MSVC build; Vulkan 1.3 driver required |
| Linux x86-64 (glibc 2.35 or newer, X11 or Wayland) | yes | GCC or Clang build; Mesa 24 or a vendor driver |
| Windows on ARM, Linux ARM64 | no | Revisit after 1.0 |
| macOS | no | A spike before 1.0 planning decides whether 1.x adds it |

## Distribution (proposed)

Institutional laptops are often managed by IT departments that block unsigned installers or
installers that need administrator rights. The first builds must run without either.

| Stage | Windows | Linux |
| --- | --- | --- |
| Curator alpha (MVP) | A portable zip that runs from the user's folder without administrator rights; signed if free open-source signing (SignPath Foundation) is granted in time | AppImage |
| 1.0 | A signed installer (per-user, no administrator rights), plus the portable zip | AppImage and a Flatpak on Flathub |

Builds are produced by CI from a version tag, with the catalogue's starter set included and the
licences of all dependencies (with Qt's LGPL source offer) in the package.

## Consequences

- Two platforms keep CI and testing manageable; the RTX 2060 desktop (Windows or Linux) and the
  780M laptop (Linux) cover both GPU families that matter most.
- Mac-using curators and designers cannot try the MVP; this is the main cost of the decision.
- Signing is a dependency on an outside organisation; applying early (milestone 3) protects the
  alpha date.

## Revisit

macOS at the 1.0 planning; signing when applying to SignPath; ARM after 1.0.

## Sources

- SignPath Foundation (free code signing for open source): https://signpath.org/
- Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0
- Qt LGPL obligations: https://www.qt.io/development/open-source-lgpl-obligations

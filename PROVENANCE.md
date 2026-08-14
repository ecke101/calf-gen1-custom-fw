# Provenance and project boundaries

This repository is intended to contain only code, documentation, configuration,
and assets that can be redistributed independently of the camera-vendor
firmware. The release audit enforces that boundary for the current tree and all
reachable Git history.

## Project-authored material

Files identified as Apache-2.0 in `REUSE.toml` are project-authored unless a
more specific notice in the file says otherwise. This includes the replacement
backend, replacement UI, build and deployment tools, tests, documentation, and
the following compatibility material:

- `ngcd/profiles/vr180_5120x2560.pto` is a newly authored, symmetric VR180
  projection model. It does not contain a copied factory projection or
  camera-specific calibration coefficients.
- `ngcd/target_include/`, `ui/src/target_abi.h`, and related declarations
  describe interfaces needed to compile independently written replacements.
  They do not contain or claim ownership of camera-vendor implementations.
- Constants that identify file locations, process names, protocol fields,
  hardware registers, or binary interfaces document facts required for
  interoperability.

Anyone contributing new material must have the right to submit it under its
declared license and must record the source, version, modifications, and
license of any imported third-party material.

## Third-party material

The complete component list and redistribution requirements are in
`THIRD_PARTY_LICENSES.md`. Provenance records are retained alongside each
component:

- `ngcd/vendor/libxaac/ORIGIN.md`
- `ngcd/vendor/vo-aacenc/README.calf`
- `ui/assets/fonts/README.md`
- `ui/third_party/README.md`

The project does not relicense these components. Their upstream copyright,
license, and notice files remain controlling.

## External firmware and local build inputs

Official CALF and ViewPT update archives are user-supplied local build inputs.
The build verifies the complete archive hash, temporarily extracts the target
libraries needed for linking, and does not copy those libraries into the
release package. Stock executables, shared libraries, firmware images,
calibration data, projection assets, and other camera-vendor content are not
included in this repository or its release package.

The installer preserves the camera's existing stock backend, UI, and relevant
calibration file on that camera. Those files remain external works and are not
covered by this project's Apache-2.0 license.

## Specifications, patents, and names

The DNG writer implements Adobe's published DNG specification under the
separate patent-license notice reproduced in `NOTICE` and the source file.
Nothing in this repository grants rights to third-party trademarks. CALF,
ViewPT, Rockchip, Adobe, Android, Noto, and other names belong to their
respective owners; their use identifies compatibility or provenance and does
not imply endorsement or affiliation.

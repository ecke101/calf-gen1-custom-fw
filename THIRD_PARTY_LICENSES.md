# Third-party components

Except where noted below, CALF-authored source code and documentation are
licensed under Apache-2.0. Third-party components retain their own licenses.
No CALF/ViewPT or Rockchip firmware image, executable, shared library,
calibration file, projection asset, or other camera-vendor file is licensed or
distributed by this repository.

| Component | Location | License and attribution |
|---|---|---|
| Android libxaac decoder | `ngcd/vendor/libxaac/` | Apache-2.0; retain `LICENSE`, `NOTICE`, and `ORIGIN.md` |
| VisualOn vo-aacenc | `ngcd/vendor/vo-aacenc/` | Apache-2.0; retain `COPYING`, `NOTICE`, and `README.calf` |
| stb_truetype | `ui/third_party/stb/` | MIT; retain `LICENSE` and the notice in the header |
| Noto fonts | `ui/assets/fonts/` | SIL Open Font License 1.1; retain `OFL.txt` and `README.md` |

The DNG writer uses Adobe's public DNG specification and patent license. The
required notice appears in the source, project NOTICE, and release package.

The official CALF and ViewPT archives are local build inputs only. Full camera
firmware artifacts produced elsewhere are not covered by this repository's
Apache-2.0 license and must not be redistributed without all necessary
permissions.

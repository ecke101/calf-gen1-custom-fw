# CALF GEN1 custom firmware

This repository contains an independently implemented backend and touchscreen
UI for the CALF VR180 GEN1 camera, plus an incremental installer. CALF
firmware 2.1.6 is the primary target. ViewPT 2.2.1 is an optional, separately
identified target.

The release package is deliberately not a firmware image. It contains no `vpupdate.bin`,
partition image, stock executable, shared library, calibration file, or other
file copied from the camera firmware. The camera's installed runtime remains
in place; the package adds the replacement processes and preserves the stock
backend and UI as a matched fallback pair.

## Local prerequisite

Obtain the applicable official update archive yourself and keep it at the
repository root or pass its path explicitly. These files are ignored and must
not be redistributed by this project.

Primary target:

```text
CALF-VR180-Camera-V2.1.6-20250102-UPDATE.tar.gz
SHA-256 387e7899b82bee861e2f3b997b83b1dee4493d860092007942629dacba8ae8c4
```

Optional target:

```text
VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz
SHA-256 1cdf3ed5236cc6686c8581c64c5fb93afe2d2054e1a3c8832068a5674965e635
```

The builder verifies the complete archive identity before extracting link-time
libraries into a temporary directory. Those libraries are never copied into
the output package.

## Build

Requirements are Python 3.11 or newer, `clang`, `ld.lld`, `llvm-ar`,
`debugfs`, `readelf`, and `sha256sum`.

```sh
python -m pip install -e '.[dev]'
calf-fw build
sha256sum -c build/release/calf-custom-fw-2.1.6.tar.gz.sha256
tar -tzf build/release/calf-custom-fw-2.1.6.tar.gz
```

For the optional target:

```sh
calf-fw build VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz --force
```

## Test and audit

```sh
make test
make lint
reuse lint
python tools/audit_release.py
```

The release audit rejects firmware archives, partition images, executables,
libraries, camera captures, unapproved top-level paths, and forbidden objects
anywhere in Git history. Embedded Noto font files are the only approved binary
source assets.

The REUSE check verifies machine-readable copyright and license coverage for
every source file without modifying the imported third-party trees.

## Install and rollback

Installation is persistent and requires Telnet access. Keep UART and a tested
recovery SD available, use external power, and stop recording, streaming, and
playback first.

For a normal installation, download the native installer for your computer
from the GitHub release. The clean firmware payload is embedded; Python and the
official camera update archive are not required.

Linux:

```sh
chmod +x calf-installer-linux-x86_64
./calf-installer-linux-x86_64 CAMERA_IP
```

Windows PowerShell:

```powershell
.\calf-installer-windows-x86_64.exe CAMERA_IP
```

The stock camera login is `root` with an empty password, which the installer
uses by default. Pass `--ask-password` only if the camera has been configured
with a non-empty Telnet password. On Windows, allow the installer on private
networks when Windows Firewall asks; the camera must briefly download the
embedded payload from the installer. Either executable can verify its embedded
payload without contacting a camera:

```sh
calf-installer-linux-x86_64 --verify
```

Developers can run the source entry point instead:

```sh
./scripts/install CAMERA_IP build/release/calf-custom-fw-2.1.6.tar.gz
```

The host and camera both verify the package. The installer refuses a package
whose target version does not match the camera, stages all changes before
pausing `ngmonitor`, and restores the stock pair if the replacement pair does
not become stable.

Settings > General > Stock UI selects both preserved stock processes for the
remainder of the boot. A crash in either replacement process makes the same
paired transition. To remove the custom firmware persistently:

Captured media can be browsed and downloaded without authentication at
`http://CAMERA_IP/download/` while the camera is reachable on the network.
The route exposes the active SD-card root, so disable Wi-Fi when it is not
needed on untrusted networks.

```sh
./scripts/install --rollback CAMERA_IP
```

See [docs/installation.md](docs/installation.md) for the transaction and
payload details.

## Licensing

CALF-authored source and documentation are licensed under Apache-2.0. Bundled
third-party source, fonts, and notices retain their respective licenses. See
[LICENSE](LICENSE), [NOTICE](NOTICE), and
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Machine-readable file
coverage is recorded in [REUSE.toml](REUSE.toml), and the clean-source and
interoperability boundaries are documented in [PROVENANCE.md](PROVENANCE.md).

The official camera archives and camera-vendor software are not part of this
repository and are not licensed by it. Creating a clean source repository does
not grant rights in any external firmware or trademark.

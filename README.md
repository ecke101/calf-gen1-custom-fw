# CALF GEN1 custom firmware

An independently implemented backend and touchscreen UI for the CALF VR180
GEN1 camera. It installs alongside the camera's existing software and keeps
the original backend and UI available as a fallback.

CALF firmware 2.1.6 is the primary target. ViewPT 2.2.1 is also recognized as
a separate, optional target. The installer checks the camera before changing
anything and refuses an unsupported firmware version.

The installation is persistent across reboots but reversible. It does not
flash a partition image or replace the camera's existing libraries. The
installer adds the CALF-built runtime, preserves the original backend and UI,
and applies only exact, hash-checked configuration changes.

## Compatibility and safety

This project is for the CALF VR180 GEN1 camera. The supported bases are:

- CALF 2.1.6 — primary and best-tested target.
- ViewPT 2.2.1 — optional secondary target.

Before installing:

1. Connect the camera and computer to the same network.
2. Find the camera's IP address. Telnet is enabled by default on the supported
   stock firmware.
3. Stop recording, streaming, and media playback.
4. Connect external power and use a reliable network connection.
5. Keep tested recovery media available. UART access is strongly recommended
   when experimenting with camera firmware.

The stock login is `root` with an empty password. The installer uses that by
default. If you configured a password yourself, add `--ask-password`.

## Install

Download the installer for your computer from
[GitHub Releases](https://github.com/ecke101/calf-gen1-custom-fw/releases).
The firmware payload is embedded in the installer, so ordinary users do not
need Python, build tools, or an official firmware archive.

### Linux

Open a terminal in the download directory and run:

```sh
chmod +x calf-installer-linux-x86_64
./calf-installer-linux-x86_64 CAMERA_IP
```

For example:

```sh
./calf-installer-linux-x86_64 192.168.1.67
```

### Windows

Open PowerShell in the download directory and run:

```powershell
.\calf-installer-windows-x86_64.exe CAMERA_IP
```

For example:

```powershell
.\calf-installer-windows-x86_64.exe 192.168.1.67
```

Allow the installer on private networks if Windows Firewall asks. During
installation, the camera briefly downloads the embedded package directly from
your computer.

### macOS

Download `calf-installer-macos-arm64.command.zip` for an Apple Silicon Mac or
`calf-installer-macos-x86_64.command.zip` for an Intel Mac. Double-click the
ZIP file, then Control-click the extracted `.command` file and choose
**Open**. The installer opens in Terminal and asks for the camera IP address.

These installers are not Apple-notarized. If macOS still blocks the installer,
approve it under **System Settings > Privacy & Security**, then open it again.

You can alternatively run it from Terminal:

```sh
chmod +x calf-installer-macos-arm64.command
./calf-installer-macos-arm64.command CAMERA_IP
```

For an Intel Mac, substitute `calf-installer-macos-x86_64.command`.

The installer verifies the package, camera model, firmware version, hardware
revision, and idle state before asking for confirmation. It stages and checks
all files before activating them. If the replacement process pair does not
start successfully, it automatically restores the stock pair.

You can verify a downloaded installer without contacting a camera:

```sh
./calf-installer-linux-x86_64 --verify
```

## Using the fallback and removing the firmware

Choose **Settings > General > Stock UI** on the camera to run the preserved
stock UI and backend for the rest of the current boot. Rebooting returns to
the custom pair. A crash in either custom process activates the same paired
fallback automatically.

To remove the custom firmware persistently, run the installer with
`--rollback`.

Linux:

```sh
./calf-installer-linux-x86_64 CAMERA_IP --rollback
```

Windows PowerShell:

```powershell
.\calf-installer-windows-x86_64.exe CAMERA_IP --rollback
```

macOS:

```sh
./calf-installer-macos-arm64.command CAMERA_IP --rollback
```

The equivalent command on the camera is:

```sh
/local/calf-custom-fw-uninstall --rollback
```

Rollback restores the preserved stock backend, UI, sensor policy, and nginx
configuration, then removes the CALF runtime files.

## Downloading media

While the camera is connected, open the following address in a web browser:

```text
http://CAMERA_IP/download/
```

This route exposes the active SD-card filesystem without authentication. Use
the camera only on a trusted network, and disable Wi-Fi when it is not needed.

See [docs/installation.md](docs/installation.md) for detailed package,
transaction, fallback, and rollback behavior.

## Building from source

Most users can skip this section and use a release installer.

The developer build uses an exact official firmware archive as a verified ABI
and stock-file reference. Libraries extracted into temporary build directories
are used only for linking and never enter the release package.

Place the applicable archive in the repository root, or pass its path to the
build command.

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

The official archives are ignored by Git and must not be redistributed by
this project.

Build requirements are Python 3.11 or newer, `clang`, `ld.lld`, `llvm-ar`,
`debugfs`, `readelf`, and `sha256sum`.

```sh
python -m pip install -e '.[dev]'
calf-fw build
sha256sum -c build/release/calf-custom-fw-2.1.6.tar.gz.sha256
tar -tzf build/release/calf-custom-fw-2.1.6.tar.gz
```

Build the optional ViewPT target with:

```sh
calf-fw build VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz --force
```

To build a native installer for the current operating system:

```sh
python -m pip install -e '.[installer]'
python tools/build_installer.py \
    build/release/calf-custom-fw-2.1.6.tar.gz
```

Developers can also install a built package directly:

```sh
./scripts/install CAMERA_IP build/release/calf-custom-fw-2.1.6.tar.gz
```

## Testing and release audit

```sh
make test
make lint
reuse lint
python tools/audit_release.py
```

The release audit rejects firmware archives, partition images, executables,
libraries, camera captures, unapproved top-level paths, and forbidden objects
anywhere in Git history. Embedded Noto fonts are the only approved binary
source assets. REUSE verifies machine-readable copyright and license coverage
without modifying imported third-party trees.

## Packaging and licensing boundary

The release is an incremental package, not a `vpupdate.bin`. It contains no
partition image, stock executable, camera-vendor shared library, calibration
file, or other file copied from the camera firmware. The installed camera
continues using its existing Rockchip and system libraries in place.

CALF-authored source and documentation are licensed under Apache-2.0. Bundled
third-party source, fonts, and notices retain their respective licenses. See
[LICENSE](LICENSE), [NOTICE](NOTICE), and
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Machine-readable coverage
is recorded in [REUSE.toml](REUSE.toml), and the clean-source and
interoperability boundaries are documented in [PROVENANCE.md](PROVENANCE.md).

Official camera archives and camera-vendor software are not part of this
repository and are not licensed by it.

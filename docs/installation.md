# Incremental replacement package

The CALF custom firmware uses an incremental release package. It is not a
`vpupdate.bin` and does not contain `app.img`, stock executables, shared
libraries, calibration data, projection assets, or any other file copied from
camera-vendor firmware.

The verified official CALF 2.1.6 archive is the default local build input. The
backend and UI are linked against that exact ABI, but the archive and extracted
libraries never enter the release package. ViewPT 2.2.1 is supported as a separate,
explicit secondary target; it produces a different package with independent
stock identities and a strict version guard.

## Payload

The package contains only CALF-built or CALF-authored runtime files:

- `calf-ngcd` and `calf-ui`;
- the small supervised `ngcd` and `ngui` selectors;
- snapshot coordination, sensor timing, RAW-to-DNG, and persistent-Wi-Fi
  helpers; and
- a manifest, transactional installer, Apache-2.0 terms, and all applicable
  third-party notices.

The UI sends snapshot requests directly to the CALF coordinator on localhost
port 8990. Consequently the installer does not patch the installed nginx
configuration, `camservice`, or any stock executable.

Half-second exposure requires the installed IMX577 policy's minimum rate to be
2 fps instead of the factory 5 fps. The installer performs that verified
one-byte transformation on the camera; the vendor JSON is never put in the
package. It preserves the exact factory form as a hard link when starting from
stock, or reconstructs it by reversing that same verified byte when starting
from the known patched form. Rollback restores the factory hash.

## Build and inspect

Keep the official archive at the ignored repository-root path, then run:

```sh
PYTHONPATH=src python -m calf_fw_tool build
tar -tzf build/release/calf-custom-fw-2.1.6.tar.gz
sha256sum -c build/release/calf-custom-fw-2.1.6.tar.gz.sha256
```

To build the optional 2.2.1 target alongside it, run:

```sh
PYTHONPATH=src python -m calf_fw_tool build \
    VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz --force
```

The builder selects a target only by the archive's exact SHA-256; a renamed or
unknown archive is not accepted.

The expected package tree is rooted at `calf-custom-fw/`. `manifest.json`
lists every runtime member, destination, size, and SHA-256. The builder rejects
an unexpected payload name and the deployment tool rejects extra, missing,
duplicate, non-regular, or identity-mismatched archive members.

## Persistent installation

The installer writes files into the camera's existing writable `/app/bin`
filesystem. It does not invoke the firmware upgrader or write a partition
image.

Before installation, keep UART and tested recovery media available, connect
external power, stop recording/streaming/playback, and verify that no temporary
backend or UI bind mount remains. Then run:

```sh
./scripts/install CAMERA_IP build/release/calf-custom-fw-2.1.6.tar.gz
```

The host verifies the camera identity and idle state, validates the complete
package locally, transfers it over a temporary single-file HTTP server, and
runs its installer through Telnet only after explicit confirmation.

The on-camera transaction:

1. verifies every packaged file again with SHA-256;
2. accepts only the exact stock or known previously patched fallback binaries
   for the package's declared 2.1.6 or 2.2.1 target;
3. preserves the installed `/app/bin/ngcd` and `/app/bin/ngui` as
   `ngcd-stock` and `ngui-stock` hard links, without copying their data;
4. locally generates and verifies the 2 fps sensor-policy variant while
   preserving the exact factory policy;
5. stages every CALF file on the application filesystem and calls `sync`;
6. pauses `ngmonitor`, stops both supervised children, and atomically renames
   the staged files into place;
7. resumes the supervisor and requires stable `calf-ngcd` and `calf-ui`
   processes; and
8. restores the stock pair and factory sensor policy automatically if the
   replacement pair is unstable.

The existing camera/Rockchip libraries remain installed and are loaded in
place. They are neither copied into the package nor relicensed by this project.

## Fallback and removal

The Settings > General > Stock UI action creates a temporary marker and
restarts both children into the preserved stock pair for the rest of the boot.
A replacement crash makes the same paired transition. Reboot clears the marker
and returns to the replacement pair.

To remove the custom firmware persistently and restore the original directory
entries:

```sh
./scripts/install --rollback CAMERA_IP
```

The equivalent on-camera command is:

```sh
/local/calf-custom-fw-uninstall --rollback
```

Removal atomically restores the preserved stock hard links and factory sensor
policy, resumes the stock pair, removes CALF runtime files, and leaves all
unrelated camera files alone.

The installer also accepts known previously patched stock binaries for both
supported bases. It does not attempt to reverse unrelated changes made by
another custom firmware. Restore the official application image first when
the installed state does not match one of the identities in the package
manifest.

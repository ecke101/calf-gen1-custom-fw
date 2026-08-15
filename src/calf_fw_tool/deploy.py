from __future__ import annotations

import argparse
import getpass
import gzip
import hashlib
import ipaddress
import json
import os
import re
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Sequence

from .deploy_common import (
    DeployError,
    TelnetSession,
    camera_preflight,
    host_address_for,
    start_file_server,
)
from .package_builder import PACKAGE_NAME, PACKAGE_ROOT
from .target_spec import TARGETS_BY_VERSION


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _md5(path: Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def materialize_camera_tar(package: Path, destination: Path) -> None:
    """Decompress the package for stock BusyBox tar, which lacks gzip."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with gzip.open(package, "rb") as source, destination.open("wb") as target:
            while chunk := source.read(1024 * 1024):
                target.write(chunk)
        with tarfile.open(destination, "r:") as archive:
            if not archive.getmembers():
                raise DeployError("decompressed camera package is empty")
    except (OSError, EOFError, tarfile.TarError) as error:
        raise DeployError(f"cannot prepare stock-compatible camera tar: {error}") from error


def validate_package(package: Path) -> dict[str, object]:
    if not package.is_file() or package.is_symlink():
        raise DeployError(f"package is missing or unsafe: {package}")
    try:
        with tarfile.open(package, "r:gz") as archive:
            members = archive.getmembers()
            by_name = {member.name: member for member in members}
            if len(by_name) != len(members):
                raise DeployError("package contains duplicate member names")
            for member in members:
                path = PurePosixPath(member.name)
                if (
                    path.is_absolute()
                    or ".." in path.parts
                    or not path.parts
                    or path.parts[0] != PACKAGE_ROOT
                    or not (member.isfile() or member.isdir())
                ):
                    raise DeployError(f"unsafe package member: {member.name!r}")
            manifest_name = f"{PACKAGE_ROOT}/manifest.json"
            install_name = f"{PACKAGE_ROOT}/install.sh"
            if (
                manifest_name not in by_name
                or install_name not in by_name
                or not by_name[manifest_name].isfile()
                or not by_name[install_name].isfile()
            ):
                raise DeployError("package is missing its manifest or installer")
            manifest_stream = archive.extractfile(by_name[manifest_name])
            if manifest_stream is None:
                raise DeployError("cannot read package manifest")
            manifest = json.load(manifest_stream)
            if not isinstance(manifest, dict) or manifest.get("profile") != "calf-custom-fw":
                raise DeployError("package manifest has the wrong profile")
            target_version = manifest.get("target_firmware")
            target = (
                TARGETS_BY_VERSION.get(target_version)
                if isinstance(target_version, str)
                else None
            )
            if (
                target is None
                or manifest.get("target_archive_sha256")
                != target.firmware.source_archive_sha256
            ):
                raise DeployError("package manifest has an unsupported target")
            payload = manifest.get("payload")
            if not isinstance(payload, dict) or not payload:
                raise DeployError("package manifest has no payload")
            if "calf-sha256" not in payload:
                raise DeployError(
                    "package predates the stock-compatible SHA-256 helper"
                )
            licenses = manifest.get("licenses")
            if not isinstance(licenses, list) or any(
                not isinstance(name, str) for name in licenses
            ):
                raise DeployError("package manifest has an invalid license list")
            for name in (*payload, *licenses):
                path = PurePosixPath(name)
                if (
                    not name
                    or path.is_absolute()
                    or ".." in path.parts
                    or str(path) != name
                ):
                    raise DeployError(f"unsafe package manifest path: {name!r}")
            for name in payload:
                if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
                    raise DeployError(f"unsafe package payload name: {name!r}")
            expected_bins = {f"{PACKAGE_ROOT}/bin/{name}" for name in payload}
            actual_bins = {
                name
                for name, member in by_name.items()
                if member.isfile() and name.startswith(f"{PACKAGE_ROOT}/bin/")
            }
            if actual_bins != expected_bins:
                raise DeployError("package binary members do not match its manifest")
            expected_files = {
                manifest_name,
                install_name,
                *expected_bins,
                *(f"{PACKAGE_ROOT}/{name}" for name in licenses),
            }
            actual_files = {
                name for name, member in by_name.items() if member.isfile()
            }
            if actual_files != expected_files:
                raise DeployError("package file members do not match its manifest")
            for name, entry in payload.items():
                if not isinstance(entry, dict):
                    raise DeployError(f"invalid manifest payload entry: {name}")
                size = entry.get("size")
                digest_expected = entry.get("sha256")
                if (
                    entry.get("destination") != f"/app/bin/{name}"
                    or not isinstance(size, int)
                    or isinstance(size, bool)
                    or size < 0
                    or not isinstance(digest_expected, str)
                    or re.fullmatch(r"[0-9a-f]{64}", digest_expected) is None
                ):
                    raise DeployError(f"invalid manifest payload entry: {name}")
                member = by_name[f"{PACKAGE_ROOT}/bin/{name}"]
                stream = archive.extractfile(member)
                if stream is None:
                    raise DeployError(f"cannot read package payload: {name}")
                digest = hashlib.sha256(stream.read()).hexdigest()
                if digest != digest_expected or member.size != size:
                    raise DeployError(f"package payload identity mismatch: {name}")
    except (OSError, tarfile.TarError, json.JSONDecodeError) as error:
        raise DeployError(f"invalid package: {error}") from error
    return manifest


def install_script(
    url: str,
    size: int,
    digest: str,
    md5_digest: str,
    helper_digest: str,
) -> str:
    return f"""set -u
url='{url}'
expected_size='{size}'
expected_sha256='{digest}'
expected_md5='{md5_digest}'
expected_helper_sha256='{helper_digest}'
archive=/tmp/calf-custom-fw-package-$$.tar
directory=/tmp/calf-custom-fw-install-$$

cleanup()
{{
    rm -f "$archive"
    case "$directory" in
        /tmp/calf-custom-fw-install-[0-9]*)
            if [ -d "$directory" ] && [ ! -L "$directory" ]; then
                rm -rf -- "$directory"
            fi
            ;;
    esac
}}
trap cleanup EXIT
trap 'cleanup; exit 1' HUP INT TERM

[ ! -e "$archive" ] || exit 1
[ ! -e "$directory" ] || exit 1
mkdir "$directory" || exit 1
if command -v curl >/dev/null 2>&1; then
    curl -f "$url" -o "$archive" || exit 1
elif command -v wget >/dev/null 2>&1; then
    wget -O "$archive" "$url" || exit 1
else
    echo "Camera has neither curl nor wget for package download" >&2
    exit 1
fi
actual_size=$(wc -c < "$archive") || exit 1
[ "$actual_size" = "$expected_size" ] || {{
    echo "Package download size mismatch" >&2
    exit 1
}}
command -v md5sum >/dev/null 2>&1 || {{
    echo "Camera md5sum is unavailable" >&2
    exit 1
}}
actual_md5=$(md5sum "$archive" | awk '{{print $1}}') || exit 1
[ "$actual_md5" = "$expected_md5" ] || {{
    echo "Package download checksum mismatch" >&2
    exit 1
}}
tar -xf "$archive" -C "$directory" || exit 1
[ -x "$directory/{PACKAGE_ROOT}/install.sh" ] || exit 1
hash_tool="$directory/{PACKAGE_ROOT}/bin/calf-sha256"
[ -x "$hash_tool" ] || exit 1
actual_helper_sha256=$("$hash_tool" "$hash_tool") || exit 1
[ "$actual_helper_sha256" = "$expected_helper_sha256" ] || {{
    echo "Package SHA-256 helper mismatch" >&2
    exit 1
}}
"$directory/{PACKAGE_ROOT}/install.sh" --preflight || exit 1
"$directory/{PACKAGE_ROOT}/install.sh"
"""


def _connect(args: argparse.Namespace) -> TelnetSession:
    password = (
        getpass.getpass(
            f"Telnet password for {args.username}@{args.camera}: "
        )
        if args.ask_password
        else ""
    )
    session = TelnetSession(args.camera, args.telnet_port, args.timeout)
    try:
        session.login(args.username, password)
    except BaseException:
        session.close()
        raise
    return session


def _default_package(repository_root: Path) -> Path:
    bundle_root_value = getattr(sys, "_MEIPASS", None)
    candidates = []
    if isinstance(bundle_root_value, str):
        candidates.append(
            Path(bundle_root_value) / "calf_installer_payload" / PACKAGE_NAME
        )
    candidates.extend(
        (
            Path(sys.executable).resolve().parent / PACKAGE_NAME,
            repository_root / "build/release" / PACKAGE_NAME,
        )
    )
    for candidate in candidates:
        if candidate.is_file() and not candidate.is_symlink():
            return candidate
    raise DeployError(
        f"bundled package {PACKAGE_NAME} is missing; pass an explicit package path"
    )


def _run(args: argparse.Namespace, repository_root: Path) -> None:
    product = camera_preflight(
        args.camera, timeout=args.timeout, allow_version_mismatch=False
    )
    print(
        f"Camera preflight passed: {product['product']}, firmware "
        f"{product['version']}, hardware {product['hardware']}, recording idle.",
        flush=True,
    )

    if args.rollback:
        with _connect(args) as session:
            output, status = session.run_script(
                "test -x /local/calf-custom-fw-uninstall && "
                "/local/calf-custom-fw-uninstall --rollback",
                timeout=150.0,
            )
        if output.strip():
            print(output.strip())
        if status != 0:
            raise DeployError("rollback did not complete")
        print("Rollback complete.")
        return

    package = args.package or _default_package(repository_root)
    package = package.resolve()
    manifest = validate_package(package)
    if product["version"] != manifest["target_firmware"]:
        raise DeployError(
            f"Package targets firmware {manifest['target_firmware']}; "
            f"camera reports {product['version']}"
        )
    digest = _sha256(package)
    print(
        f"Package: {package} ({package.stat().st_size} bytes, SHA-256 {digest})",
        flush=True,
    )
    print(
        f"Payload files: {len(manifest['payload'])}; "
        "camera-vendor firmware files: none",
        flush=True,
    )

    if not args.yes:
        answer = input(
            f"Persistently install custom firmware on {args.camera}? "
            "Type 'install' to continue: "
        )
        if answer != "install":
            print("Cancelled.")
            return

    host_address = args.host_address or host_address_for(args.camera)
    try:
        ipaddress.IPv4Address(host_address)
    except ipaddress.AddressValueError as error:
        raise DeployError(
            f"--host-address must be an IPv4 address: {host_address}"
        ) from error
    url = f"http://{host_address}:{args.http_port}/calf-custom-fw.tar"
    with tempfile.TemporaryDirectory(prefix="calf-camera-package-") as name:
        camera_package = Path(name) / "calf-custom-fw.tar"
        materialize_camera_tar(package, camera_package)
        camera_digest = _sha256(camera_package)
        camera_md5 = _md5(camera_package)
        camera_size = camera_package.stat().st_size
        server, thread = start_file_server(
            camera_package, args.http_port, url_path="/calf-custom-fw.tar"
        )
        try:
            print(
                "Serving the verified stock-compatible package to the camera "
                f"at {url}",
                flush=True,
            )
            with _connect(args) as session:
                output, status = session.run_script(
                    install_script(
                        url,
                        camera_size,
                        camera_digest,
                        camera_md5,
                        str(manifest["payload"]["calf-sha256"]["sha256"]),
                    ),
                    timeout=240.0,
                )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2.0)
    if output.strip():
        print(output.strip())
    if status != 0:
        raise DeployError(
            "Installation failed. Failures before activation leave stock untouched; "
            "failures after activation trigger automatic stock rollback. Review the "
            "camera output above for the exact stage."
        )
    print("Installation complete.")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="calf-custom-fw-install",
        description=(
            "Install the vendor-file-free package over Telnet with checked stock "
            "preservation and automatic rollback."
        ),
    )
    parser.add_argument(
        "camera",
        nargs="?",
        default=os.environ.get("CALF_CAMERA_IP"),
        help="camera IPv4 address or hostname (or set CALF_CAMERA_IP)",
    )
    parser.add_argument(
        "package",
        nargs="?",
        type=Path,
        help="package tar.gz (default: build/release package)",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="validate the embedded package and exit without contacting a camera",
    )
    parser.add_argument("--rollback", action="store_true")
    parser.add_argument("--yes", action="store_true", help="skip the install confirmation")
    parser.add_argument("--username", default="root")
    parser.add_argument(
        "--ask-password",
        action="store_true",
        help="prompt for a non-empty Telnet password (stock default is empty)",
    )
    parser.add_argument("--telnet-port", type=int, default=23)
    parser.add_argument("--http-port", type=int, default=8000)
    parser.add_argument("--host-address")
    parser.add_argument("--timeout", type=float, default=8.0)
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    parser = _parser()
    args = parser.parse_args(argv)
    repository_root = Path(__file__).resolve().parents[2]
    if args.verify:
        try:
            package = (args.package or _default_package(repository_root)).resolve()
            manifest = validate_package(package)
        except DeployError as error:
            print(f"error: {error}", file=sys.stderr)
            raise SystemExit(2) from error
        print(
            f"Embedded package verified: firmware {manifest['target_firmware']}, "
            f"{len(manifest['payload'])} payload files, SHA-256 {_sha256(package)}"
        )
        return
    if not args.camera:
        if sys.stdin.isatty():
            args.camera = input("Camera IP address: ").strip()
        if not args.camera:
            parser.error("camera is required")
    if not (1 <= args.telnet_port <= 65535 and 1 <= args.http_port <= 65535):
        parser.error("port numbers must be between 1 and 65535")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        _run(args, repository_root)
    except DeployError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        raise SystemExit(130) from None


if __name__ == "__main__":
    main()

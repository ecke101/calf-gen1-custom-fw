from __future__ import annotations

import argparse
import getpass
import hashlib
import ipaddress
import json
import os
import re
import sys
import tarfile
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


def install_script(url: str, size: int, digest: str) -> str:
    return f"""set -u
url='{url}'
expected_size='{size}'
expected_sha256='{digest}'
archive=/tmp/calf-custom-fw-package-$$.tar.gz
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
/usr/bin/wget -O "$archive" "$url" || exit 1
actual_size=$(wc -c < "$archive") || exit 1
[ "$actual_size" = "$expected_size" ] || {{
    echo "Package download size mismatch" >&2
    exit 1
}}
actual_sha256=$(sha256sum "$archive" | awk '{{print $1}}') || exit 1
[ "$actual_sha256" = "$expected_sha256" ] || {{
    echo "Package download SHA-256 mismatch" >&2
    exit 1
}}
tar -xzf "$archive" -C "$directory" || exit 1
[ -x "$directory/{PACKAGE_ROOT}/install.sh" ] || exit 1
"$directory/{PACKAGE_ROOT}/install.sh"
"""


def _connect(args: argparse.Namespace) -> TelnetSession:
    password = getpass.getpass(f"Telnet password for {args.username}@{args.camera}: ")
    session = TelnetSession(args.camera, args.telnet_port, args.timeout)
    try:
        session.login(args.username, password)
    except BaseException:
        session.close()
        raise
    return session


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

    package = args.package or repository_root / "build/release" / PACKAGE_NAME
    package = package.resolve()
    manifest = validate_package(package)
    if product["version"] != manifest["target_firmware"]:
        raise DeployError(
            f"Package targets firmware {manifest['target_firmware']}; "
            f"camera reports {product['version']}"
        )
    digest = _sha256(package)
    size = package.stat().st_size
    print(f"Package: {package} ({size} bytes, SHA-256 {digest})", flush=True)
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
    url = f"http://{host_address}:{args.http_port}/calf-custom-fw.tar.gz"
    server, thread = start_file_server(
        package, args.http_port, url_path="/calf-custom-fw.tar.gz"
    )
    try:
        print(f"Serving the verified package to the camera at {url}", flush=True)
        with _connect(args) as session:
            output, status = session.run_script(
                install_script(url, size, digest), timeout=240.0
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)
    if output.strip():
        print(output.strip())
    if status != 0:
        raise DeployError(
            "Installation failed; the on-camera installer attempted stock rollback"
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
    parser.add_argument("--rollback", action="store_true")
    parser.add_argument("--yes", action="store_true", help="skip the install confirmation")
    parser.add_argument("--username", default="root")
    parser.add_argument("--telnet-port", type=int, default=23)
    parser.add_argument("--http-port", type=int, default=8000)
    parser.add_argument("--host-address")
    parser.add_argument("--timeout", type=float, default=8.0)
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    parser = _parser()
    args = parser.parse_args(argv)
    if not args.camera:
        parser.error("camera is required")
    if not (1 <= args.telnet_port <= 65535 and 1 <= args.http_port <= 65535):
        parser.error("port numbers must be between 1 and 65535")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        _run(args, Path(__file__).resolve().parents[2])
    except DeployError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        raise SystemExit(130) from None


if __name__ == "__main__":
    main()

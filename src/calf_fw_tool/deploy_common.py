from __future__ import annotations

import http.server
import ipaddress
import json
import os
import re
import socket
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Sequence

SUPPORTED_CAMERA_VERSIONS = frozenset(("2.1.6", "2.2.1"))
EXPECTED_HARDWARE = "4.0"


class DeployError(RuntimeError):
    """A deployment failure with a user-actionable message."""


def _post_json(url: str, body: dict[str, object], timeout: float) -> dict:
    request = urllib.request.Request(
        url,
        data=json.dumps(body, separators=(",", ":")).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            result = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
        raise DeployError(
            f"camera HTTP preflight failed for {url}: {error}"
        ) from error
    if not isinstance(result, dict) or result.get("code") != 0:
        raise DeployError(
            f"camera returned an unsuccessful response for {url}: {result!r}"
        )
    return result


def _get_json(url: str, timeout: float) -> dict:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            result = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
        raise DeployError(
            f"camera HTTP preflight failed for {url}: {error}"
        ) from error
    if not isinstance(result, dict) or result.get("code") != 0:
        raise DeployError(
            f"camera returned an unsuccessful response for {url}: {result!r}"
        )
    return result


def camera_preflight(
    camera: str,
    *,
    timeout: float,
    allow_version_mismatch: bool = False,
) -> dict[str, str]:
    base = f"http://{camera}"
    product = _get_json(f"{base}/camera/v2/productinfo", timeout)
    product_body = product.get("body")
    if not isinstance(product_body, dict):
        raise DeployError("productinfo response has no object body")

    version = str(product_body.get("version", ""))
    hardware = str(product_body.get("hardware", ""))
    if version not in SUPPORTED_CAMERA_VERSIONS and not allow_version_mismatch:
        supported = ", ".join(sorted(SUPPORTED_CAMERA_VERSIONS))
        raise DeployError(
            f"camera reports unsupported firmware {version or 'unknown'}; "
            f"supported versions are {supported}"
        )
    if hardware != EXPECTED_HARDWARE:
        raise DeployError(
            f"camera reports hardware {hardware or 'unknown'}, "
            f"expected {EXPECTED_HARDWARE}"
        )

    status = _post_json(
        f"{base}/camera/v2/systemstatus", {"ssids": 2}, timeout
    )
    try:
        is_running = int(status["body"]["rs"]["is_running"])
    except (KeyError, TypeError, ValueError) as error:
        raise DeployError("systemstatus response has no recording state") from error
    if is_running != 0:
        raise DeployError("the camera is recording; stop recording before deployment")

    return {
        "version": version,
        "hardware": hardware,
        "product": str(product_body.get("product", "camera")),
    }


class _SingleFileHandler(http.server.BaseHTTPRequestHandler):
    file_path: Path
    url_path = "/calf-custom-fw.tar.gz"

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path != self.url_path:
            self.send_error(404)
            return
        try:
            size = self.file_path.stat().st_size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with self.file_path.open("rb") as source:
                while chunk := source.read(1024 * 1024):
                    self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, format: str, *args: object) -> None:
        return


def start_file_server(
    file_path: Path, port: int, *, url_path: str = "/calf-custom-fw.tar.gz"
) -> tuple[http.server.ThreadingHTTPServer, threading.Thread]:
    if not re.fullmatch(r"/[A-Za-z0-9._-]+", url_path):
        raise DeployError(f"unsafe file-server URL path: {url_path!r}")
    handler = type(
        "CalfSingleFileHandler",
        (_SingleFileHandler,),
        {"file_path": file_path, "url_path": url_path},
    )
    try:
        server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    except OSError as error:
        raise DeployError(
            f"could not listen on host TCP port {port}: {error}"
        ) from error
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def host_address_for(camera: str) -> str:
    try:
        camera_ip = socket.gethostbyname(camera)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((camera_ip, 9))
            address = route.getsockname()[0]
        ipaddress.IPv4Address(address)
    except (OSError, ipaddress.AddressValueError) as error:
        raise DeployError(
            f"could not determine the host address used for {camera}: {error}"
        ) from error
    if address.startswith("127.") or address == "0.0.0.0":
        raise DeployError(f"refusing unusable host address {address}")
    return address


class TelnetSession:
    IAC = 255
    DONT = 254
    DO = 253
    WONT = 252
    WILL = 251
    SB = 250
    SE = 240
    ECHO = 1
    SUPPRESS_GO_AHEAD = 3

    def __init__(self, host: str, port: int, timeout: float):
        try:
            self.socket = socket.create_connection((host, port), timeout=timeout)
        except OSError as error:
            raise DeployError(
                f"could not connect to Telnet at {host}:{port}: {error}"
            ) from error
        self.socket.settimeout(min(timeout, 1.0))
        self.timeout = timeout
        self._state = "data"
        self._option_command = 0

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass

    def __enter__(self) -> TelnetSession:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _send_negotiation(self, command: int, option: int) -> None:
        self.socket.sendall(bytes((self.IAC, command, option)))

    def _decode(self, raw: bytes) -> bytes:
        plain = bytearray()
        for byte in raw:
            if self._state == "data":
                if byte == self.IAC:
                    self._state = "iac"
                else:
                    plain.append(byte)
            elif self._state == "iac":
                if byte == self.IAC:
                    plain.append(byte)
                    self._state = "data"
                elif byte in (self.DO, self.DONT, self.WILL, self.WONT):
                    self._option_command = byte
                    self._state = "option"
                elif byte == self.SB:
                    self._state = "subnegotiation"
                else:
                    self._state = "data"
            elif self._state == "option":
                if self._option_command == self.WILL:
                    reply = (
                        self.DO
                        if byte in (self.ECHO, self.SUPPRESS_GO_AHEAD)
                        else self.DONT
                    )
                    self._send_negotiation(reply, byte)
                elif self._option_command == self.DO:
                    reply = (
                        self.WILL if byte == self.SUPPRESS_GO_AHEAD else self.WONT
                    )
                    self._send_negotiation(reply, byte)
                self._state = "data"
            elif self._state == "subnegotiation":
                if byte == self.IAC:
                    self._state = "subnegotiation_iac"
            elif self._state == "subnegotiation_iac":
                self._state = "data" if byte == self.SE else "subnegotiation"
        return bytes(plain)

    def send_line(self, line: str) -> None:
        try:
            self.socket.sendall(line.encode() + b"\r\n")
        except OSError as error:
            raise DeployError(f"Telnet write failed: {error}") from error

    def expect(
        self, patterns: Sequence[bytes], *, timeout: float | None = None
    ) -> tuple[bytes, int]:
        deadline = time.monotonic() + (
            self.timeout if timeout is None else timeout
        )
        data = bytearray()
        while time.monotonic() < deadline:
            for index, pattern in enumerate(patterns):
                if re.search(pattern, data, re.IGNORECASE):
                    return bytes(data), index
            try:
                raw = self.socket.recv(4096)
            except socket.timeout:
                continue
            except OSError as error:
                raise DeployError(f"Telnet read failed: {error}") from error
            if not raw:
                raise DeployError("Telnet connection closed unexpectedly")
            data.extend(self._decode(raw))
        preview = bytes(data[-500:]).decode(errors="replace").strip()
        raise DeployError(
            f"Telnet response timed out; last output: {preview!r}"
        )

    def login(self, username: str, password: str) -> None:
        shell_prompt = rb"(?:^|\r?\n)[^\r\n]{0,100}[#$]\s"
        _, match = self.expect((rb"login:\s*$", rb"password:\s*$", shell_prompt))
        password_needed = match == 1
        if match == 0:
            self.send_line(username)
            _, next_match = self.expect((rb"password:\s*$", shell_prompt))
            password_needed = next_match == 0
        if password_needed:
            self.send_line(password)
            try:
                _, match = self.expect(
                    (shell_prompt, rb"login incorrect", rb"login:\s*$"),
                    timeout=12.0,
                )
            except DeployError as error:
                raise DeployError(
                    "Telnet login did not reach a shell prompt; "
                    "credentials may be wrong"
                ) from error
            if match != 0:
                raise DeployError("Telnet login failed: credentials rejected")
        self.send_line("stty -echo")
        self.expect((shell_prompt,))

    def run_script(self, script: str, *, timeout: float) -> tuple[str, int]:
        marker = f"__CALF_DEPLOY_RC_{os.getpid()}_{time.monotonic_ns()}__"
        tag = f"CALF_DEPLOY_SCRIPT_{os.getpid()}_{time.monotonic_ns()}"
        self.send_line(f"sh <<'{tag}'")
        for line in script.splitlines():
            self.send_line(line)
        self.send_line(tag)
        self.send_line(f"calf_rc=$?; printf '\\n{marker}:%s\\n' \"$calf_rc\"")
        pattern = rb"\r?\n" + re.escape(marker.encode()) + rb":([0-9]+)\r?\n"
        response, _ = self.expect((pattern,), timeout=timeout)
        match = re.search(pattern, response)
        if match is None:
            raise DeployError("Telnet command completed without a status marker")
        output = response[: match.start()].decode(errors="replace")
        return output, int(match.group(1))

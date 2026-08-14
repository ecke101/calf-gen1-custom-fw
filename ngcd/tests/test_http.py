from __future__ import annotations

import json
import signal
import socket
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT.parent / "build" / "ngcd" / "ngcd"


def free_port() -> int:
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        return int(server.getsockname()[1])


def request_raw(port: int, raw: bytes) -> tuple[int, bytes, bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.sendall(raw)
        client.shutdown(socket.SHUT_WR)
        data = bytearray()
        while chunk := client.recv(65536):
            data.extend(chunk)
    header, body = bytes(data).split(b"\r\n\r\n", 1)
    status = int(header.split(b" ", 2)[1])
    return status, header, body


def request(port: int, raw: bytes) -> tuple[int, dict]:
    status, _, body = request_raw(port, raw)
    return status, json.loads(body) if body else {}


def wait_ready(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"ngcd exited early with {process.returncode}")
        try:
            status, _ = request(
                port,
                b"GET /camera/v2/productinfo HTTP/1.1\r\nHost: test\r\n\r\n",
            )
            if status == 200:
                return
        except OSError:
            time.sleep(0.02)
    raise AssertionError("ngcd did not start")


def main() -> None:
    port = free_port()
    process = subprocess.Popen(
        [str(BINARY), "--mock", "--bind", "127.0.0.1", "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        wait_ready(process, port)
        body = b'{"type":"iso","value":"iso400"}'
        status, response = request(
            port,
            b"POST /camera/v2/imgparams HTTP/1.1\r\n"
            b"Host: test\r\nContent-Type: application/json\r\n"
            + f"Content-Length: {len(body)}\r\n\r\n".encode()
            + body,
        )
        assert status == 200 and response["code"] == 0

        status, response = request(
            port,
            b"GET /camera/v2/imgparams HTTP/1.1\r\nHost: test\r\n\r\n",
        )
        assert status == 200 and response["body"]["iso"] == "iso400"

        status, response = request(
            port,
            b"GET /camera/v2/wifi HTTP/1.1\r\nHost: test\r\n\r\n",
        )
        assert status == 200 and response["body"]["essid"] == "CALF-MOCK"

        status, response = request(
            port,
            b"GET /camera/v2/scanwifi HTTP/1.1\r\nHost: test\r\n\r\n",
        )
        assert status == 200 and response["body"]["list"][1] == {
            "essid": 'Lab "Guest"',
            "qual": 2,
            "level": -78,
        }

        status, header, screenshot = request_raw(
            port,
            b"GET /camera/v2/lcdscreenshot HTTP/1.1\r\nHost: test\r\n\r\n",
        )
        assert status == 200
        assert b"Content-Type: image/bmp" in header
        assert b"Access-Control-Allow-Origin" not in header
        assert screenshot[:2] == b"BM" and len(screenshot) == 58

        status, _ = request(
            port,
            b"POST /camera/v2/imgparams HTTP/1.1\r\nHost: test\r\n"
            b"Content-Length: 1\r\nContent-Length: 2\r\n\r\n{}",
        )
        assert status == 400

        status, _ = request(
            port,
            b"POST /camera/v2/imgparams HTTP/1.1\r\nHost: test\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        )
        assert status == 400

        status, _ = request(
            port,
            b"OPTIONS /camera/v2/imgparams HTTP/1.1\r\nHost: test\r\n\r\n",
        )
        assert status == 204

        # An incomplete client must not pin the media/service loop or delay a
        # supervisor shutdown for the old five-second socket timeout.
        slow_client = socket.create_connection(("127.0.0.1", port), timeout=2)
        slow_client.sendall(b"GET /camera/v2/productinfo HTTP/1.1\r\n")
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=1)
        slow_client.close()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
    assert process.returncode == 0
    # ngmonitor restarts both children as a pair.  Exercise repeated clean
    # termination on the same listening port to catch leaked sockets, stuck
    # accept loops, and non-zero supervisor-visible exit statuses.
    for signal_number in (signal.SIGTERM, signal.SIGINT) * 3:
        process = subprocess.Popen(
            [str(BINARY), "--mock", "--bind", "127.0.0.1", "--port", str(port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        wait_ready(process, port)
        process.send_signal(signal_number)
        process.wait(timeout=2)
        assert process.returncode == 0
    print("ngcd HTTP tests passed")


if __name__ == "__main__":
    main()

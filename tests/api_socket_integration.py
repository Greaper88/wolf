#!/usr/bin/env python3
"""Run inside a disposable Wolf container with a private fake Docker socket.

Set WOLF_GPU_DISPOSABLE_TEST=1 and
WOLF_DOCKER_SOCKET=/tmp/wolf-api-test-docker.sock when starting Wolf.
The fake daemon exercises image-pull responses without downloading images.
"""

import concurrent.futures
import json
import os
import socket
import sys
import threading
import time


assert os.environ.get("WOLF_GPU_DISPOSABLE_TEST") == "1", "Disposable test only"
FAKE_DOCKER = "/tmp/wolf-api-test-docker.sock"
assert os.environ.get("WOLF_DOCKER_SOCKET") == FAKE_DOCKER
API_SOCKET = os.environ["WOLF_SOCKET_PATH"]
COUNT = 1800
RELEASE = threading.Event()


def connect_api(path, body=None):
    conn = socket.socket(socket.AF_UNIX)
    conn.settimeout(15)
    conn.connect(API_SOCKET)
    method = "POST" if body is not None else "GET"
    payload = json.dumps(body).encode() if body is not None else b""
    conn.sendall(
        f"{method} {path} HTTP/1.0\r\nContent-Length: {len(payload)}\r\n\r\n".encode()
        + payload
    )
    return conn


def read_headers(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        block = conn.recv(4096)
        assert block, "Connection closed before HTTP headers"
        data += block
    headers, rest = data.split(b"\r\n\r\n", 1)
    assert headers.startswith(b"HTTP/1.0 200"), headers
    return rest


def serve_pull(listener, disconnect):
    while True:
        conn, _ = listener.accept()
        conn.settimeout(15)
        request = b""
        while b"\r\n\r\n" not in request:
            block = conn.recv(4096)
            assert block, "Docker request closed before headers"
            request += block
        if request.startswith(b"GET /version "):
            version = b'{"ApiVersion":"1.40"}'
            conn.sendall(f"HTTP/1.0 200 OK\r\nContent-Length: {len(version)}\r\n\r\n".encode() + version)
            conn.close()
        else:
            break
    with conn:
        assert b"/images/create?fromImage=wolf-api-test" in request.split(b"\r\n")[0]
        conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n")
        for i in range(COUNT):
            record = {
                "id": f"layer-{i:05d}-" + "x" * 400,
                "progressDetail": {"current": i, "total": COUNT},
            }
            conn.sendall(json.dumps(record).encode() + b"\n")
            if i == 0 and disconnect:
                assert RELEASE.wait(10), "Client did not disconnect"
            if disconnect:
                time.sleep(0.001)


def check_pull(listener, pool, disconnect):
    RELEASE.clear()
    result = pool.submit(serve_pull, listener, disconnect)
    conn = connect_api("/api/v1/docker/images/pull", {"image_name": "wolf-api-test"})
    initial = read_headers(conn)
    if disconnect:
        conn.close()
        RELEASE.set()
    else:
        # Delay reading so multiple progress writes are pending simultaneously.
        time.sleep(0.2)
        data = bytearray(initial)
        with conn:
            while block := conn.recv(8192):
                data.extend(block)
        records = [json.loads(line) for line in data.splitlines() if line]
        assert len(records) == COUNT + 1, len(records)
        assert records[-1] == {"success": True}, records[-1]
        for i, record in enumerate(records[:-1]):
            assert record["layer_id"] == f"layer-{i:05d}-" + "x" * 400
    result.result(timeout=15)
    print("PASS: image pull " + ("finishes after client disconnect" if disconnect else "delivers every progress record in order"), flush=True)


with socket.socket(socket.AF_UNIX) as listener:
    listener.settimeout(15)
    listener.bind(FAKE_DOCKER)
    listener.listen()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            if "--disconnect-only" in sys.argv:
                check_pull(listener, pool, True)
            else:
                with connect_api("/api/v1/events") as sse:
                    events = read_headers(sse)
                    check_pull(listener, pool, False)
                    check_pull(listener, pool, True)
                    # A later request must still work after the failed response stream.
                    check_pull(listener, pool, False)
                    while events.count(b"event: DockerPullImageEndEvent\n") < 3 or not events.endswith(b"\n\n"):
                        block = sse.recv(8192)
                        assert block, "SSE connection unexpectedly closed"
                        events += block
                    received = []
                    for frame in events.decode().split("\n\n"):
                        if frame.startswith("event: DockerPullImage"):
                            event, payload = frame.splitlines()
                            received.append(event)
                            data = json.loads(payload.removeprefix("data: "))
                            assert data["image_name"] == "wolf-api-test"
                    assert received == [
                        "event: DockerPullImageStartEvent",
                        "event: DockerPullImageEndEvent",
                    ] * 3, received
                    print("PASS: SSE delivers complete, ordered download events", flush=True)
        # Unknown paths must deliver their 404 before closing the socket.
        with connect_api("/api/v1/nonexistent") as conn:
            response = conn.recv(4096)
            assert response.startswith(b"HTTP/1.0 404"), response
        print("PASS: API remains responsive and sends its 404 response", flush=True)
    finally:
        os.unlink(FAKE_DOCKER)

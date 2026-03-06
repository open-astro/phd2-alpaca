#!/usr/bin/env python3
import argparse
import json
import socket
import sys
import time


class RpcClient:
    def __init__(self, host: str, port: int, timeout: float):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._file = self._sock.makefile("rwb", buffering=0)
        self._id = 1

    def close(self):
        try:
            self._file.close()
        finally:
            self._sock.close()

    def call(self, method: str, params=None, timeout_s: float = 5.0):
        req_id = self._id
        self._id += 1
        req = {"jsonrpc": "2.0", "method": method, "id": req_id}
        if params is not None:
            req["params"] = params
        self._file.write((json.dumps(req) + "\n").encode("utf-8"))

        deadline = time.time() + timeout_s
        while time.time() < deadline:
            line = self._file.readline()
            if not line:
                raise RuntimeError(f"socket closed while waiting for {method}")
            msg = json.loads(line.decode("utf-8", errors="replace").strip())
            if isinstance(msg, dict) and msg.get("id") == req_id:
                if msg.get("error"):
                    raise RuntimeError(f"{method} error: {msg['error']}")
                return msg.get("result")
        raise RuntimeError(f"timeout waiting for response to {method}")


def print_section(title: str, values):
    print(f"{title}:")
    if not isinstance(values, list) or not values:
        print("  (none)")
        return
    for v in values:
        print(f"  - {v}")


def main():
    ap = argparse.ArgumentParser(description="List PHD2 equipment choices (camera, mount, aux mount, AO, rotator)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4400)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    cli = None
    try:
        cli = RpcClient(args.host, args.port, args.timeout)
        result = cli.call("get_equipment_choices", timeout_s=args.timeout)
        if not isinstance(result, dict):
            raise RuntimeError(f"unexpected result type: {type(result).__name__}")

        print_section("Camera", result.get("camera"))
        print_section("Mount", result.get("mount"))
        print_section("Aux Mount", result.get("aux_mount"))
        print_section("AO", result.get("AO"))
        print_section("Rotator", result.get("rotator"))
        return 0
    except Exception as e:
        print(f"ERROR: {e}")
        return 1
    finally:
        if cli:
            cli.close()


if __name__ == "__main__":
    sys.exit(main())

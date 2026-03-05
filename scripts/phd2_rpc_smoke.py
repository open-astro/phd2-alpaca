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

    def _next_id(self):
        i = self._id
        self._id += 1
        return i

    def call(self, method: str, params=None, timeout_s: float = 5.0):
        msg = self.request(method, params=params, timeout_s=timeout_s)
        if "error" in msg and msg["error"]:
            raise RuntimeError(f"{method} error: {msg['error']}")
        return msg.get("result")

    def request(self, method: str, params=None, timeout_s: float = 5.0):
        req_id = self._next_id()
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
                return msg
        raise RuntimeError(f"timeout waiting for response to {method}")


def main():
    ap = argparse.ArgumentParser(description="PHD2 JSON-RPC smoke test for PINS/NINA compatibility methods")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4400)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    failures = []
    checks = 0

    def run_check(name, fn):
        nonlocal checks
        checks += 1
        try:
            result = fn()
            print(f"PASS {name}: {result}")
            return result
        except Exception as e:
            failures.append((name, str(e)))
            print(f"FAIL {name}: {e}")
            return None

    def expect_error(name, method, params=None, code=None, message_contains=None):
        nonlocal checks
        checks += 1
        try:
            msg = cli.request(method, params=params)
            err = msg.get("error")
            if not err:
                failures.append((name, f"expected error, got success: {msg.get('result')}"))
                print(f"FAIL {name}: expected error, got success")
                return
            if code is not None and err.get("code") != code:
                failures.append((name, f"expected code {code}, got {err.get('code')}"))
                print(f"FAIL {name}: expected code {code}, got {err.get('code')}")
                return
            if message_contains and message_contains.lower() not in str(err.get("message", "")).lower():
                failures.append((name, f"error message missing '{message_contains}': {err}"))
                print(f"FAIL {name}: error message missing '{message_contains}'")
                return
            print(f"PASS {name}: {err}")
        except Exception as e:
            failures.append((name, str(e)))
            print(f"FAIL {name}: {e}")

    try:
        cli = RpcClient(args.host, args.port, args.timeout)
    except Exception as e:
        print(f"FAIL connect {args.host}:{args.port}: {e}")
        return 2

    try:
        run_check("get_app_state", lambda: cli.call("get_app_state"))
        run_check("get_profiles", lambda: cli.call("get_profiles"))
        run_check("get_profile", lambda: cli.call("get_profile"))
        run_check("get_connected", lambda: cli.call("get_connected"))
        run_check("get_current_equipment", lambda: cli.call("get_current_equipment"))

        choices = run_check("get_equipment_choices", lambda: cli.call("get_equipment_choices")) or {}
        mount_choice = run_check("get_selected_mount", lambda: cli.call("get_selected_mount"))
        camera_choice = run_check("get_selected_camera", lambda: cli.call("get_selected_camera"))
        aux_choice = run_check("get_selected_aux_mount", lambda: cli.call("get_selected_aux_mount"))
        ao_choice = run_check("get_selected_ao", lambda: cli.call("get_selected_ao"))
        rot_choice = run_check("get_selected_rotator", lambda: cli.call("get_selected_rotator"))

        run_check("get_selected_camera_id", lambda: cli.call("get_selected_camera_id"))
        run_check("get_selected_indi_mount_driver", lambda: cli.call("get_selected_indi_mount_driver"))
        run_check("get_selected_indi_camera_driver", lambda: cli.call("get_selected_indi_camera_driver"))

        bitdepth = run_check("get_camera_bitdepth", lambda: cli.call("get_camera_bitdepth"))

        if isinstance(mount_choice, str):
            run_check("set_selected_mount", lambda: cli.call("set_selected_mount", {"mount": mount_choice}))
        if isinstance(camera_choice, str):
            run_check("set_selected_camera", lambda: cli.call("set_selected_camera", {"camera": camera_choice}))
        if isinstance(aux_choice, str):
            run_check("set_selected_aux_mount", lambda: cli.call("set_selected_aux_mount", {"aux_mount": aux_choice}))
        if isinstance(ao_choice, str):
            run_check("set_selected_ao", lambda: cli.call("set_selected_ao", {"ao": ao_choice}))
        if isinstance(rot_choice, str):
            run_check("set_selected_rotator", lambda: cli.call("set_selected_rotator", {"rotator": rot_choice}))

        if isinstance(bitdepth, int):
            run_check("set_camera_bitdepth", lambda: cli.call("set_camera_bitdepth", {"bitdepth": bitdepth}))

        current_cam_id = run_check("get_selected_camera_id (again)", lambda: cli.call("get_selected_camera_id"))
        if isinstance(current_cam_id, str):
            run_check("set_selected_camera_id", lambda: cli.call("set_selected_camera_id", {"camera_id": current_cam_id}))

        mount_driver = run_check("get_selected_indi_mount_driver (again)", lambda: cli.call("get_selected_indi_mount_driver"))
        if isinstance(mount_driver, str):
            run_check("set_selected_indi_mount_driver", lambda: cli.call("set_selected_indi_mount_driver", {"mount_driver": mount_driver}))

        cam_driver = run_check("get_selected_indi_camera_driver (again)", lambda: cli.call("get_selected_indi_camera_driver"))
        if isinstance(cam_driver, str):
            run_check("set_selected_indi_camera_driver", lambda: cli.call("set_selected_indi_camera_driver", {"camera_driver": cam_driver}))

        # basic presence check for list keys
        for key in ("camera", "mount", "aux_mount", "AO", "rotator"):
            if key not in choices:
                failures.append(("get_equipment_choices keys", f"missing key {key}"))
                print(f"FAIL get_equipment_choices keys: missing key {key}")
            else:
                print(f"PASS get_equipment_choices keys: has {key}")

        expect_error(
            "unknown_method_returns_method_not_found",
            "this_method_does_not_exist_123",
            code=-32601,
        )
        expect_error(
            "set_selected_mount_invalid_type",
            "set_selected_mount",
            params={"mount": 123},
            code=-32602,
        )
        expect_error(
            "set_selected_camera_invalid_choice",
            "set_selected_camera",
            params={"camera": "__NOT_A_REAL_CAMERA__"},
            code=-32602,
        )

        is_connected = run_check("get_connected (final)", lambda: cli.call("get_connected"))
        if is_connected and isinstance(camera_choice, str):
            expect_error(
                "set_selected_camera_rejected_while_connected",
                "set_selected_camera",
                params={"camera": camera_choice},
                message_contains="connected",
            )

    finally:
        cli.close()

    if failures:
        print("")
        print(f"SMOKE RESULT: FAIL ({len(failures)}/{checks} checks failed)")
        for name, err in failures:
            print(f"- {name}: {err}")
        return 1

    print("")
    print(f"SMOKE RESULT: PASS ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

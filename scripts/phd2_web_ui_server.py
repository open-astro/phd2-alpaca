#!/usr/bin/env python3
import argparse
import ipaddress
import json
import mimetypes
import queue
import socket
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


class PhdRpcBridge:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self._host = host
        self._port = port
        self._timeout = timeout

        self._sock: socket.socket | None = None
        self._file = None
        self._running = False
        self._desired_running = False
        self._reader_thread: threading.Thread | None = None
        self._connector_thread: threading.Thread | None = None
        self._conn_lock = threading.Lock()

        self._send_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: dict[int, queue.Queue] = {}
        self._next_id = 1

        self._event_queues_lock = threading.Lock()
        self._event_queues: list[queue.Queue] = []

    def start(self):
        self._desired_running = True
        if self._connector_thread and self._connector_thread.is_alive():
            return
        self._connector_thread = threading.Thread(target=self._connector_loop, name="phd2-rpc-connector", daemon=True)
        self._connector_thread.start()

    def stop(self):
        self._desired_running = False
        self._running = False
        self._close_socket()
        self._broadcast_event({"Event": "BridgeDisconnected"})

    def _close_socket(self):
        try:
            if self._file:
                self._file.close()
        finally:
            self._file = None
            if self._sock:
                self._sock.close()
            self._sock = None

    def _connect_once(self) -> bool:
        with self._conn_lock:
            if self._running:
                return True
            sock = socket.create_connection((self._host, self._port), timeout=self._timeout)
            sock.settimeout(None)
            self._sock = sock
            self._file = sock.makefile("rwb", buffering=0)
            self._running = True
            self._reader_thread = threading.Thread(target=self._reader_loop, name="phd2-rpc-reader", daemon=True)
            self._reader_thread.start()
        self._broadcast_event({"Event": "BridgeConnected", "Host": self._host, "Port": self._port})
        return True

    def _connector_loop(self):
        while self._desired_running:
            if self._running:
                time.sleep(1.0)
                continue
            try:
                self._connect_once()
            except Exception as e:
                self._broadcast_event({"Event": "BridgeConnectRetry", "Message": str(e)})
                time.sleep(2.0)

    def _reader_loop(self):
        try:
            while self._running and self._file:
                line = self._file.readline()
                if not line:
                    raise RuntimeError("PHD2 socket closed")
                try:
                    msg = json.loads(line.decode("utf-8", errors="replace").strip())
                except Exception as e:
                    self._broadcast_event({"Event": "BridgeParseError", "Message": str(e)})
                    continue

                if isinstance(msg, dict) and "id" in msg:
                    req_id = msg.get("id")
                    with self._pending_lock:
                        q = self._pending.get(req_id)
                    if q:
                        q.put(msg)
                elif isinstance(msg, dict) and "Event" in msg:
                    self._broadcast_event(msg)
                else:
                    self._broadcast_event({"Event": "BridgeUnknownMessage", "Payload": msg})
        except Exception as e:
            self._broadcast_event({"Event": "BridgeError", "Message": str(e)})
            self._running = False
            self._close_socket()

    def _next_request_id(self) -> int:
        with self._pending_lock:
            req_id = self._next_id
            self._next_id += 1
        return req_id

    def call(self, method: str, params: Any = None, timeout_s: float = 8.0):
        if not self._running or not self._file:
            raise RuntimeError("Bridge is not connected to PHD2")

        req_id = self._next_request_id()
        req = {"jsonrpc": "2.0", "method": method, "id": req_id}
        if params is not None:
            req["params"] = params

        resp_queue: queue.Queue = queue.Queue(maxsize=1)
        with self._pending_lock:
            self._pending[req_id] = resp_queue

        try:
            with self._send_lock:
                self._file.write((json.dumps(req) + "\n").encode("utf-8"))
            msg = resp_queue.get(timeout=timeout_s)
        except queue.Empty as e:
            raise RuntimeError(f"timeout waiting for response to {method}") from e
        finally:
            with self._pending_lock:
                self._pending.pop(req_id, None)

        if not isinstance(msg, dict):
            raise RuntimeError(f"invalid response for {method}: {msg}")

        if msg.get("error"):
            raise RuntimeError(str(msg["error"]))

        return msg.get("result")

    def subscribe_events(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=100)
        with self._event_queues_lock:
            self._event_queues.append(q)
        return q

    def unsubscribe_events(self, q: queue.Queue):
        with self._event_queues_lock:
            if q in self._event_queues:
                self._event_queues.remove(q)

    def _broadcast_event(self, event: dict):
        with self._event_queues_lock:
            queues = list(self._event_queues)

        for q in queues:
            try:
                q.put_nowait(event)
            except queue.Full:
                try:
                    q.get_nowait()
                    q.put_nowait(event)
                except Exception:
                    pass


class WebHandler(BaseHTTPRequestHandler):
    server_version = "PHD2WebBridge/1.0"

    def _json_response(self, code: int, payload: dict):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(raw.decode("utf-8"))

    def _serve_file(self, path: Path, content_type: str):
        if not path.exists() or not path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND, "Not found")
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    @staticmethod
    def _probe_tcp(host: str, port: int, timeout_s: float) -> bool:
        try:
            with socket.create_connection((host, port), timeout=timeout_s):
                return True
        except Exception:
            return False

    def _discover_indi(self, subnet: str | None, port: int, timeout_ms: int, max_hosts: int):
        timeout_s = max(0.05, timeout_ms / 1000.0)
        found: list[str] = []

        for host in ("127.0.0.1", "localhost"):
            if self._probe_tcp(host, port, timeout_s):
                found.append(f"{host}:{port}")

        scanned = 2
        if subnet:
            network = ipaddress.ip_network(subnet, strict=False)
            count = 0
            for ip in network.hosts():
                if count >= max_hosts:
                    break
                host = str(ip)
                if host == "127.0.0.1":
                    continue
                scanned += 1
                count += 1
                if self._probe_tcp(host, port, timeout_s):
                    found.append(f"{host}:{port}")

        unique = []
        seen = set()
        for item in found:
            if item in seen:
                continue
            seen.add(item)
            unique.append(item)
        return {"servers": unique, "scanned": scanned}

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            return self._serve_file(self.server.web_root / "index.html", "text/html; charset=utf-8")

        if path.startswith("/assets/"):
            rel = path.removeprefix("/assets/")
            target = (self.server.web_root / "assets" / rel).resolve()
            assets_root = (self.server.web_root / "assets").resolve()
            if assets_root not in target.parents and target != assets_root:
                return self.send_error(HTTPStatus.NOT_FOUND, "Not found")
            content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
            if content_type.startswith("text/"):
                content_type += "; charset=utf-8"
            return self._serve_file(target, content_type)

        if path == "/api/setup":
            try:
                payload = {
                    "profiles": self.server.bridge.call("get_profiles"),
                    "profile": self.server.bridge.call("get_profile"),
                    "connected": self.server.bridge.call("get_connected"),
                    "current_equipment": self.server.bridge.call("get_current_equipment"),
                    "equipment_choices": self.server.bridge.call("get_equipment_choices"),
                    "selected_camera": self.server.bridge.call("get_selected_camera"),
                    "selected_mount": self.server.bridge.call("get_selected_mount"),
                    "selected_aux_mount": self.server.bridge.call("get_selected_aux_mount"),
                    "selected_ao": self.server.bridge.call("get_selected_ao"),
                    "selected_rotator": self.server.bridge.call("get_selected_rotator"),
                    "selected_camera_id": self.server.bridge.call("get_selected_camera_id"),
                    "alpaca_server": self.server.bridge.call("get_alpaca_server"),
                    "profile_setup": self.server.bridge.call("get_profile_setup"),
                    "dark_status": self.server.bridge.call("get_calibration_files_status"),
                }
                return self._json_response(200, {"ok": True, "setup": payload})
            except Exception as e:
                return self._json_response(500, {"ok": False, "error": str(e)})

        if path == "/api/discover/alpaca":
            try:
                num_queries = int(query.get("num_queries", ["2"])[0])
                timeout_seconds = int(query.get("timeout_seconds", ["2"])[0])
                rpc_timeout = min(300.0, max(15.0, float(num_queries * timeout_seconds + 10)))
                result = self.server.bridge.call(
                    "discover_alpaca_servers",
                    {"num_queries": num_queries, "timeout_seconds": timeout_seconds},
                    timeout_s=rpc_timeout,
                )
                return self._json_response(200, {"ok": True, "servers": result or []})
            except Exception as e:
                return self._json_response(500, {"ok": False, "error": str(e)})

        if path == "/api/discover/indi":
            try:
                subnet = query.get("subnet", [""])[0].strip() or None
                port = int(query.get("port", ["7624"])[0])
                timeout_ms = int(query.get("timeout_ms", ["150"])[0])
                max_hosts = int(query.get("max_hosts", ["64"])[0])
                result = self._discover_indi(subnet, port, timeout_ms, max_hosts)
                return self._json_response(200, {"ok": True, **result})
            except Exception as e:
                return self._json_response(500, {"ok": False, "error": str(e)})

        return self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self):
        if self.path == "/api/rpc":
            try:
                body = self._read_json_body()
                method = body.get("method")
                params = body.get("params")
                timeout_s = body.get("timeout_s")
                if not isinstance(method, str) or not method:
                    return self._json_response(400, {"ok": False, "error": "method is required"})
                if timeout_s is not None:
                    timeout_s = float(timeout_s)
                result = self.server.bridge.call(method, params=params, timeout_s=timeout_s if timeout_s is not None else 8.0)
                return self._json_response(200, {"ok": True, "result": result})
            except Exception as e:
                return self._json_response(500, {"ok": False, "error": str(e)})

        return self._json_response(404, {"ok": False, "error": "Unknown API route"})

    def log_message(self, format, *args):
        return


class WebServer(ThreadingHTTPServer):
    def __init__(self, server_address, RequestHandlerClass, bridge: PhdRpcBridge, web_root: Path):
        super().__init__(server_address, RequestHandlerClass)
        self.bridge = bridge
        self.web_root = web_root


def parse_args():
    ap = argparse.ArgumentParser(
        description="(Deprecated dev fallback) Web UI bridge for PHD2 JSON-RPC TCP server"
    )
    ap.add_argument("--listen", default="127.0.0.1", help="HTTP bind address")
    ap.add_argument("--http-port", type=int, default=8080, help="HTTP server port")
    ap.add_argument("--phd-host", default="127.0.0.1", help="PHD2 event server host")
    ap.add_argument("--phd-port", type=int, default=4400, help="PHD2 event server port")
    ap.add_argument("--phd-timeout", type=float, default=5.0, help="PHD2 socket timeout seconds")
    return ap.parse_args()


def main():
    args = parse_args()
    web_root = Path(__file__).resolve().parent / "webui"

    print("WARNING: scripts/phd2_web_ui_server.py is a deprecated developer fallback.")
    print("Use embedded web mode in PHD2 (Tools -> Enable Server, Tools -> Open Web Portal).")

    bridge = PhdRpcBridge(args.phd_host, args.phd_port, args.phd_timeout)
    bridge.start()

    server = WebServer((args.listen, args.http_port), WebHandler, bridge=bridge, web_root=web_root)

    print(f"PHD2 bridge connected to {args.phd_host}:{args.phd_port}")
    print(f"Web UI available at http://{args.listen}:{args.http_port}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        bridge.stop()


if __name__ == "__main__":
    main()

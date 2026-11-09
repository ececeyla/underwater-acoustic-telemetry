#!/usr/bin/env python3
"""Telemetry ingest server for LilyGo T-SIM7080G surface nodes.

Listens on a TCP port and accepts multiple concurrent connections. Each
connection is expected to send one JSON object per line (newline-delimited),
matching the firmware's /api/state JSON, which includes a "node" field
(NODE_ID). Every received line is appended to a separate log file per node,
under logs/<node_id>.log.

Usage:
    python3 telemetry_server.py --host 0.0.0.0 --port 5000
"""
import argparse
import json
import os
import socketserver
from datetime import datetime, timezone

LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")


def node_log_path(node_id: str) -> str:
    safe_id = "".join(c for c in node_id if c.isalnum() or c in ("-", "_")) or "unknown"
    return os.path.join(LOG_DIR, f"{safe_id}.log")


class TelemetryHandler(socketserver.StreamRequestHandler):
    def handle(self):
        client_addr = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[+] Connection from {client_addr}")
        try:
            for raw_line in self.rfile:
                line = raw_line.decode("utf-8", errors="replace").strip()
                if line:
                    self._process_line(line, client_addr)
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            print(f"[-] Disconnected {client_addr}")

    def _process_line(self, line: str, client_addr: str) -> None:
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            print(f"[!] Invalid JSON from {client_addr}: {line}")
            return

        node_id = str(data.get("nodeId") or data.get("node") or "unknown")
        record = {
            "receivedAt": datetime.now(timezone.utc).isoformat(),
            "from": client_addr,
            "data": data,
        }

        os.makedirs(LOG_DIR, exist_ok=True)
        with open(node_log_path(node_id), "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

        print(f"[{node_id}] {line}")


class ThreadingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(description="Telemetry ingest server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=5000, help="TCP port")
    args = parser.parse_args()

    os.makedirs(LOG_DIR, exist_ok=True)
    server = ThreadingTCPServer((args.host, args.port), TelemetryHandler)
    print(f"Telemetry server listening on {args.host}:{args.port}")
    print(f"Logs directory: {LOG_DIR}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.shutdown()


if __name__ == "__main__":
    main()

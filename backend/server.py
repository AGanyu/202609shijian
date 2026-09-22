from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from .config import Settings
from .service import ColdChainService


class ApiRequestHandler(BaseHTTPRequestHandler):
    server_version = "ColdChainBackend/1.0"

    def do_GET(self) -> None:
        self._handle()

    def do_POST(self) -> None:
        self._handle()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self._send_cors_headers()
        self.end_headers()

    def _handle(self) -> None:
        content_length = int(self.headers.get("Content-Length", "0") or "0")
        if content_length > 2 * 1024 * 1024:
            self.send_response(413)
            self._send_cors_headers()
            self.end_headers()
            return
        body = self.rfile.read(content_length) if content_length else b""
        headers = {key: value for key, value in self.headers.items()}
        response = self.server.service.dispatch(
            self.command,
            self.path,
            headers,
            body,
            source_ip=self.client_address[0],
        )
        encoded = json.dumps(
            response.body,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(response.status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self._send_cors_headers()
        self.end_headers()
        self.wfile.write(encoded)

    def _send_cors_headers(self) -> None:
        origins = self.server.settings.cors_origins
        origin = self.headers.get("Origin")
        if "*" in origins:
            allow_origin = "*"
        elif origin in origins:
            allow_origin = origin
        else:
            allow_origin = origins[0]
        self.send_header("Access-Control-Allow-Origin", allow_origin)
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Gateway-Id, X-Api-Key, X-Request-Id")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def log_message(self, format: str, *args: Any) -> None:
        # API requests are already recorded in SQLite and requests.jsonl.
        return


class ApiServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], settings: Settings):
        self.settings = settings
        self.service = ColdChainService(settings)
        super().__init__(address, ApiRequestHandler)


def run_server(settings: Settings | None = None) -> None:
    settings = settings or Settings.from_env()
    server = ApiServer((settings.host, settings.port), settings)
    print(f"Cold-chain backend listening on http://{settings.host}:{settings.port}")
    print(f"Database: {settings.database_path}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping backend...")
    finally:
        server.server_close()

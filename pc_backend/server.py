"""TCP backend for esp32_s3_wifi_serial.

The backend uses newline-delimited JSON. It intentionally depends only on
Python's standard library so it can run on a clean Windows installation.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
from pathlib import Path
from dataclasses import dataclass
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlsplit


API_TOKEN = "u500703-esp32-telemetry-2026-change-me"


def now_text() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def _number(value: object) -> float | None:
    try:
        return float(str(value).replace("℃", "").replace("%", "").strip())
    except (TypeError, ValueError):
        return None


def parse_sensor_line(line: object) -> dict | None:
    """Parse JSON or key/value sensor lines into a dashboard module record."""
    raw: dict = {}
    if isinstance(line, dict):
        raw = line
    else:
        text = str(line)
        try:
            parsed = json.loads(text)
            if isinstance(parsed, dict):
                raw = parsed
                if isinstance(parsed.get("data"), dict):
                    raw = {**parsed, **parsed["data"]}
        except json.JSONDecodeError:
            pairs = re.findall(r"([A-Za-z_][\w.-]*|模块|温度|湿度)\s*[=:：]\s*([^,，;；\s]+)", text)
            raw = {key.lower(): value for key, value in pairs}
            if not raw:
                match = re.search(r"R\s*:\s*([0-9.]+)\s*RH\s+(-?[0-9.]+)\s*C", text, re.IGNORECASE)
                if match:
                    raw = {"module": "R", "humidity": match.group(1), "temperature": match.group(2)}
                else:
                    return None

    def pick(*keys: str) -> object:
        for key in keys:
            if key in raw and raw[key] not in (None, ""):
                return raw[key]
        lowered = {str(key).lower(): value for key, value in raw.items()}
        for key in keys:
            if key.lower() in lowered:
                return lowered[key.lower()]
        return None

    temperature = _number(pick("temperature", "temp", "温度"))
    humidity = _number(pick("humidity", "hum", "rh", "湿度"))
    if temperature is None and humidity is None:
        return None
    module = str(pick("module", "module_id", "sensor", "name", "id", "模块") or "default")
    device_id = str(pick("device_id", "device", "deviceId") or "")
    return {
        "device_id": device_id,
        "module": module,
        "temperature": temperature,
        "humidity": humidity,
        "updatedAt": now_text(),
    }


@dataclass
class Client:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    address: str

    async def send(self, payload: dict) -> None:
        data = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        self.writer.write(data)
        await self.writer.drain()

    def close(self) -> None:
        self.writer.close()


class Backend:
    def __init__(self) -> None:
        self.clients: dict[asyncio.StreamWriter, Client] = {}
        self.sensor_modules: dict[str, dict] = {}
        self.devices: dict[str, dict] = {}
        self.command_queues: dict[str, list[dict]] = {}
        self.command_id = 0
        self.recent_messages: list[dict] = []
        self.web_root = Path(__file__).with_name("web")

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        address = f"{peer[0]}:{peer[1]}" if peer else "unknown"
        client = Client(reader, writer, address)
        self.clients[writer] = client
        print(f"[{now_text()}] ESP connected: {address}")
        await client.send({"type": "hello", "data": "pc backend ready"})

        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    break
                if len(raw) > 8192:
                    print(f"[{now_text()}] Dropped oversized line from {address}")
                    continue
                try:
                    message = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    print(f"[{now_text()}] Invalid JSON from {address}: {raw[:160]!r}")
                    continue
                self.handle_message(address, message)
        except (ConnectionError, asyncio.IncompleteReadError) as error:
            print(f"[{now_text()}] Connection error from {address}: {error}")
        finally:
            self.clients.pop(writer, None)
            client.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass
            print(f"[{now_text()}] ESP disconnected: {address}")

    def handle_message(self, address: str, message: object) -> None:
        if not isinstance(message, dict):
            print(f"[{now_text()}] {address} -> {message!r}")
            return
        message_type = message.get("type", "unknown")
        if message_type == "serial":
            payload = message.get("data", "")
            data = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
            print(f"[{now_text()}] [{address}] UART: {data}")
            sensor = parse_sensor_line(payload)
            if sensor is not None:
                device_id = sensor.get("device_id") or str(message.get("device_id") or address)
                sensor["device_id"] = device_id
                sensor["source"] = address
                self.sensor_modules[f"{device_id}:{sensor['module']}"] = sensor
                self.devices[device_id] = {"device_id": device_id, "lastSeen": sensor["updatedAt"], "source": address}
            self.recent_messages.append({"time": now_text(), "source": address, "data": data})
            self.recent_messages = self.recent_messages[-50:]
        elif message_type == "status":
            print(f"[{now_text()}] [{address}] status={message.get('state')} detail={message.get('detail', '')}")
        else:
            print(f"[{now_text()}] [{address}] {json.dumps(message, ensure_ascii=False)}")

    async def broadcast_command(self, data: str, device_id: str = "") -> int:
        payload = {"type": "command", "data": data}
        sent = 0
        targets = [device_id] if device_id else list(self.devices)
        for target in targets:
            if target:
                self.command_id += 1
                self.command_queues.setdefault(target, []).append({"id": self.command_id, "data": data})
                sent += 1
        for client in list(self.clients.values()):
            try:
                await client.send(payload)
                if not device_id and not self.devices:
                    sent += 1
            except ConnectionError:
                client.close()
        return sent

    def take_commands(self, device_id: str) -> list[dict]:
        commands = self.command_queues.pop(device_id, [])
        return commands

    def state(self) -> dict:
        cutoff = datetime.now(timezone.utc).timestamp() - 60

        def recent(item: dict) -> bool:
            try:
                return datetime.fromisoformat(str(item.get("lastSeen") or item.get("updatedAt"))).timestamp() >= cutoff
            except (TypeError, ValueError):
                return False

        active_devices = [item for item in self.devices.values() if recent(item)]
        active_device_ids = {item["device_id"] for item in active_devices}
        return {
            "serverTime": now_text(),
            "clients": [{"address": client.address} for client in self.clients.values()],
            "devices": active_devices,
            "modules": [item for item in self.sensor_modules.values() if item.get("device_id") in active_device_ids and recent(item)],
            "messages": list(reversed(self.recent_messages)),
        }

    async def handle_http(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request_line = await asyncio.wait_for(reader.readline(), timeout=5)
            if not request_line:
                writer.close()
                await writer.wait_closed()
                return
            method, path, _ = request_line.decode("ascii", errors="replace").strip().split(" ", 2)
            headers: dict[str, str] = {}
            while True:
                line = await reader.readline()
                if line in {b"\r\n", b"\n", b""}:
                    break
                key, _, value = line.decode("iso-8859-1").partition(":")
                headers[key.lower()] = value.strip()
            body = b""
            content_length = int(headers.get("content-length", "0") or "0")
            if content_length:
                body = await reader.readexactly(min(content_length, 16384))

            clean_path = path.split("?", 1)[0]
            if method == "GET" and clean_path == "/api/state":
                await self.write_http(writer, 200, "application/json; charset=utf-8", json.dumps(self.state(), ensure_ascii=False).encode())
            elif method == "POST" and clean_path == "/api/telemetry":
                if headers.get("x-device-token") != API_TOKEN:
                    await self.write_http(writer, 401, "application/json; charset=utf-8", b'{"ok":false,"error":"unauthorized"}')
                    return
                try:
                    payload = json.loads(body.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    await self.write_http(writer, 400, "application/json; charset=utf-8", b'{"ok":false,"error":"invalid JSON"}')
                else:
                    peer = writer.get_extra_info("peername")
                    address = f"{peer[0]}:{peer[1]}" if peer else "http-client"
                    message = payload if isinstance(payload, dict) and payload.get("type") else {"type": "serial", "data": payload}
                    self.handle_message(address, message)
                    await self.write_http(writer, 200, "application/json; charset=utf-8", b'{"ok":true}')
            elif method == "GET" and clean_path == "/api/commands":
                if headers.get("x-device-token") != API_TOKEN:
                    await self.write_http(writer, 401, "application/json; charset=utf-8", b'{"ok":false,"error":"unauthorized"}')
                    return
                query = parse_qs(urlsplit(path).query)
                device_id = query.get("device_id", [""])[0]
                if not device_id:
                    await self.write_http(writer, 400, "application/json; charset=utf-8", b'{"ok":false,"error":"device_id required"}')
                    return
                self.devices.setdefault(device_id, {"device_id": device_id, "lastSeen": now_text(), "source": "http"})["lastSeen"] = now_text()
                body_out = json.dumps({"commands": self.take_commands(device_id)}, ensure_ascii=False).encode()
                await self.write_http(writer, 200, "application/json; charset=utf-8", body_out)
            elif method == "POST" and clean_path == "/api/command":
                try:
                    payload = json.loads(body.decode("utf-8"))
                    command = str(payload.get("data", "")).strip()
                except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
                    command = body.decode("utf-8", errors="replace").strip()
                sent = await self.broadcast_command(command) if command else 0
                response = json.dumps({"ok": bool(command), "sent": sent}, ensure_ascii=False).encode()
                await self.write_http(writer, 200, "application/json; charset=utf-8", response)
            elif method == "GET":
                await self.serve_static(writer, clean_path)
            else:
                await self.write_http(writer, 405, "text/plain; charset=utf-8", b"Method Not Allowed")
        except (asyncio.TimeoutError, ValueError, asyncio.IncompleteReadError):
            await self.write_http(writer, 400, "text/plain; charset=utf-8", b"Bad Request")
        finally:
            writer.close()
            await writer.wait_closed()

    async def serve_static(self, writer: asyncio.StreamWriter, path: str) -> None:
        file_name = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/styles.css": "styles.css"}.get(path)
        if file_name is None:
            await self.write_http(writer, 404, "text/plain; charset=utf-8", b"Not Found")
            return
        file_path = self.web_root / file_name
        if not file_path.is_file():
            await self.write_http(writer, 404, "text/plain; charset=utf-8", b"Not Found")
            return
        content_type = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8"}[file_path.suffix]
        await self.write_http(writer, 200, content_type, file_path.read_bytes())

    @staticmethod
    async def write_http(writer: asyncio.StreamWriter, status: int, content_type: str, body: bytes) -> None:
        reason = {200: "OK", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 405: "Method Not Allowed"}.get(status, "Error")
        header = f"HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {len(body)}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n".encode("ascii")
        writer.write(header + body)
        await writer.drain()

    async def command_loop(self) -> None:
        print("Commands: send <text>, list, help, quit")
        while True:
            try:
                line = (await asyncio.to_thread(input, "> ")).strip()
            except EOFError:
                break
            if not line:
                continue
            command, _, argument = line.partition(" ")
            command = command.lower()
            if command == "send" and argument:
                count = await self.broadcast_command(argument)
                print(f"Sent to {count} ESP client(s)")
            elif command == "list":
                print("Connected clients:")
                for client in self.clients.values():
                    print(f"  {client.address}")
            elif command in {"help", "?"}:
                print("send <text>  send a line to all ESP clients")
                print("list         list connected ESP clients")
                print("quit         stop the backend")
            elif command in {"quit", "exit"}:
                break
            else:
                print("Unknown command. Type help.")

    async def run(self, host: str, port: int, web_host: str, web_port: int) -> None:
        server = await asyncio.start_server(self.handle_client, host, port)
        web_server = await asyncio.start_server(self.handle_http, web_host, web_port)
        sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
        web_sockets = ", ".join(str(sock.getsockname()) for sock in web_server.sockets or [])
        print(f"Backend listening on {sockets}")
        print(f"Web dashboard listening on http://127.0.0.1:{web_port}/")
        async with server, web_server:
            await self.command_loop()
        for client in self.clients.values():
            client.close()
        await asyncio.gather(*(client.writer.wait_closed() for client in self.clients.values()), return_exceptions=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-S3 WiFi serial bridge backend")
    parser.add_argument("--host", default="0.0.0.0", help="listen address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=8765, help="TCP port (default: 8765)")
    parser.add_argument("--web-host", default="0.0.0.0", help="dashboard listen address")
    parser.add_argument("--web-port", type=int, default=8080, help="dashboard HTTP port")
    args = parser.parse_args()
    try:
        asyncio.run(Backend().run(args.host, args.port, args.web_host, args.web_port))
    except KeyboardInterrupt:
        print("\nBackend stopped")


if __name__ == "__main__":
    main()

from __future__ import annotations

import hashlib
import json
import sqlite3
import threading
from pathlib import Path
from typing import Any, Iterable

from .config import Settings
from .utils import utc_now_iso


DEFAULT_DEVICES = (
    {
        "device_id": "zb-node-001",
        "gateway_id": "esp32-gateway-001",
        "zone_id": "cold-storage-a",
        "device_type": "zigbee_sensor",
        "sensor_type": "SHT30",
    },
    {
        "device_id": "zb-node-002",
        "gateway_id": "esp32-gateway-001",
        "zone_id": "cold-storage-a",
        "device_type": "zigbee_sensor",
        "sensor_type": "SHT30",
    },
    {
        "device_id": "zb-node-003",
        "gateway_id": "esp32-gateway-001",
        "zone_id": "cold-storage-b",
        "device_type": "zigbee_sensor",
        "sensor_type": "SHT30",
    },
)


class Database:
    def __init__(self, settings: Settings):
        self.path = Path(settings.database_path)
        self.log_dir = Path(settings.log_dir)
        self._lock = threading.RLock()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.initialize(settings)

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(
            self.path,
            timeout=10,
            check_same_thread=False,
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA journal_mode = WAL")
        return connection

    def initialize(self, settings: Settings) -> None:
        schema = """
        CREATE TABLE IF NOT EXISTS gateways (
            gateway_id TEXT PRIMARY KEY,
            enabled INTEGER NOT NULL DEFAULT 1,
            firmware_version TEXT,
            ip_address TEXT,
            uptime_seconds INTEGER,
            connected_nodes INTEGER,
            free_heap INTEGER,
            last_seen_at TEXT,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY,
            gateway_id TEXT NOT NULL,
            zone_id TEXT NOT NULL,
            device_type TEXT NOT NULL,
            sensor_type TEXT NOT NULL,
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at TEXT NOT NULL,
            FOREIGN KEY (gateway_id) REFERENCES gateways(gateway_id)
        );

        CREATE TABLE IF NOT EXISTS telemetry (
            message_id TEXT PRIMARY KEY,
            gateway_id TEXT NOT NULL,
            device_id TEXT NOT NULL,
            zone_id TEXT NOT NULL,
            collected_at TEXT NOT NULL,
            received_at TEXT NOT NULL,
            temperature REAL NOT NULL,
            humidity REAL,
            signal INTEGER,
            quality TEXT NOT NULL,
            created_at TEXT NOT NULL,
            FOREIGN KEY (gateway_id) REFERENCES gateways(gateway_id),
            FOREIGN KEY (device_id) REFERENCES devices(device_id)
        );

        CREATE TABLE IF NOT EXISTS alerts (
            alert_id TEXT PRIMARY KEY,
            message_id TEXT NOT NULL,
            device_id TEXT NOT NULL,
            zone_id TEXT NOT NULL,
            type TEXT NOT NULL,
            level TEXT NOT NULL,
            value REAL NOT NULL,
            threshold REAL NOT NULL,
            status TEXT NOT NULL,
            created_at TEXT NOT NULL,
            acknowledged_at TEXT,
            operator TEXT,
            remark TEXT,
            FOREIGN KEY (message_id) REFERENCES telemetry(message_id),
            FOREIGN KEY (device_id) REFERENCES devices(device_id)
        );

        CREATE TABLE IF NOT EXISTS request_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id TEXT NOT NULL,
            requested_at TEXT NOT NULL,
            source_ip TEXT,
            gateway_id TEXT,
            method TEXT NOT NULL,
            path TEXT NOT NULL,
            status_code INTEGER NOT NULL,
            error_code INTEGER,
            duration_ms REAL
        );

        CREATE INDEX IF NOT EXISTS idx_telemetry_device_collected
            ON telemetry(device_id, collected_at DESC);
        CREATE INDEX IF NOT EXISTS idx_telemetry_zone_collected
            ON telemetry(zone_id, collected_at DESC);
        CREATE INDEX IF NOT EXISTS idx_alerts_status_created
            ON alerts(status, created_at DESC);
        """
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    connection.executescript(schema)
                    now = utc_now_iso()
                    for gateway_id in settings.gateway_api_keys:
                        connection.execute(
                            """
                            INSERT INTO gateways (gateway_id, created_at)
                            VALUES (?, ?)
                            ON CONFLICT(gateway_id) DO NOTHING
                            """,
                            (gateway_id, now),
                        )
                    existing_count = connection.execute(
                        "SELECT COUNT(*) AS count FROM devices"
                    ).fetchone()["count"]
                    if existing_count == 0:
                        for device in DEFAULT_DEVICES:
                            if device["gateway_id"] not in settings.gateway_api_keys:
                                continue
                            connection.execute(
                                """
                                INSERT INTO devices (
                                    device_id, gateway_id, zone_id, device_type,
                                    sensor_type, enabled, created_at
                                ) VALUES (?, ?, ?, ?, ?, 1, ?)
                                """,
                                (
                                    device["device_id"],
                                    device["gateway_id"],
                                    device["zone_id"],
                                    device["device_type"],
                                    device["sensor_type"],
                                    now,
                                ),
                            )
            finally:
                connection.close()

    def fetchone(self, query: str, params: Iterable[Any] = ()) -> sqlite3.Row | None:
        with self._lock:
            connection = self.connect()
            try:
                return connection.execute(query, tuple(params)).fetchone()
            finally:
                connection.close()

    def fetchall(self, query: str, params: Iterable[Any] = ()) -> list[sqlite3.Row]:
        with self._lock:
            connection = self.connect()
            try:
                return list(connection.execute(query, tuple(params)).fetchall())
            finally:
                connection.close()

    def execute(self, query: str, params: Iterable[Any] = ()) -> int:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    cursor = connection.execute(query, tuple(params))
                    return cursor.rowcount
            finally:
                connection.close()

    def insert_telemetry(self, item: dict[str, Any]) -> bool:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    try:
                        connection.execute(
                            """
                            INSERT INTO telemetry (
                                message_id, gateway_id, device_id, zone_id,
                                collected_at, received_at, temperature, humidity,
                                signal, quality, created_at
                            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                            """,
                            (
                                item["message_id"],
                                item["gateway_id"],
                                item["device_id"],
                                item["zone_id"],
                                item["collected_at"],
                                item["received_at"],
                                item["temperature"],
                                item["humidity"],
                                item["signal"],
                                item["quality"],
                                utc_now_iso(),
                            ),
                        )
                        connection.execute(
                            """
                            UPDATE gateways
                            SET last_seen_at = ?
                            WHERE gateway_id = ?
                            """,
                            (utc_now_iso(), item["gateway_id"]),
                        )
                        return True
                    except sqlite3.IntegrityError as exc:
                        if "UNIQUE constraint failed: telemetry.message_id" in str(exc):
                            return False
                        raise
            finally:
                connection.close()

    def update_gateway_heartbeat(self, item: dict[str, Any]) -> None:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    connection.execute(
                        """
                        UPDATE gateways
                        SET firmware_version = ?,
                            ip_address = ?,
                            uptime_seconds = ?,
                            connected_nodes = ?,
                            free_heap = ?,
                            last_seen_at = ?
                        WHERE gateway_id = ?
                        """,
                        (
                            item["firmware_version"],
                            item["ip_address"],
                            item["uptime_seconds"],
                            item["connected_nodes"],
                            item["free_heap"],
                            utc_now_iso(),
                            item["gateway_id"],
                        ),
                    )
            finally:
                connection.close()

    def create_alert(
        self,
        *,
        alert_id: str,
        message_id: str,
        device_id: str,
        zone_id: str,
        alert_type: str,
        level: str,
        value: float,
        threshold: float,
    ) -> None:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    connection.execute(
                        """
                        INSERT INTO alerts (
                            alert_id, message_id, device_id, zone_id, type, level,
                            value, threshold, status, created_at
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'open', ?)
                        """,
                        (
                            alert_id,
                            message_id,
                            device_id,
                            zone_id,
                            alert_type,
                            level,
                            value,
                            threshold,
                            utc_now_iso(),
                        ),
                    )
            finally:
                connection.close()

    def close_open_alerts(self, device_id: str, alert_type: str) -> None:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    connection.execute(
                        """
                        UPDATE alerts
                        SET status = 'closed'
                        WHERE device_id = ? AND type = ? AND status = 'open'
                        """,
                        (device_id, alert_type),
                    )
            finally:
                connection.close()

    def log_request(self, entry: dict[str, Any]) -> None:
        with self._lock:
            connection = self.connect()
            try:
                with connection:
                    connection.execute(
                        """
                        INSERT INTO request_logs (
                            request_id, requested_at, source_ip, gateway_id,
                            method, path, status_code, error_code, duration_ms
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        (
                            entry["request_id"],
                            entry["requested_at"],
                            entry.get("source_ip"),
                            entry.get("gateway_id"),
                            entry["method"],
                            entry["path"],
                            entry["status_code"],
                            entry.get("error_code"),
                            entry.get("duration_ms"),
                        ),
                    )
            finally:
                connection.close()
        self._append_jsonl("requests.jsonl", entry)

    def log_raw_telemetry(self, payload: dict[str, Any]) -> None:
        self._append_jsonl("telemetry.jsonl", payload)

    def _append_jsonl(self, filename: str, payload: dict[str, Any]) -> None:
        path = self.log_dir / filename
        line = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        with self._lock:
            with path.open("a", encoding="utf-8") as log_file:
                log_file.write(line + "\n")

    @staticmethod
    def api_key_hash(api_key: str) -> str:
        return hashlib.sha256(api_key.encode("utf-8")).hexdigest()

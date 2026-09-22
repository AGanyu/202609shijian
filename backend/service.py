from __future__ import annotations

import hmac
import json
import sqlite3
import time
from datetime import timedelta
from typing import Any
from urllib.parse import parse_qs, urlparse
from uuid import uuid4

from .config import Settings
from .database import Database
from .errors import ApiError, bad_request, not_found
from .utils import (
    canonical_iso_datetime,
    new_alert_id,
    parse_iso_datetime,
    request_id as make_request_id,
    utc_now,
    utc_now_iso,
    validate_identifier,
    validate_integer,
    validate_number,
)


class Response:
    def __init__(self, status_code: int, body: dict[str, Any]):
        self.status_code = status_code
        self.body = body


class ColdChainService:
    prefix = "/api/v1"

    def __init__(self, settings: Settings | None = None):
        self.settings = settings or Settings.from_env()
        self.database = Database(self.settings)

    def dispatch(
        self,
        method: str,
        raw_path: str,
        headers: dict[str, str] | None = None,
        body: bytes | str | None = None,
        source_ip: str | None = None,
    ) -> Response:
        headers = {key.lower(): value for key, value in (headers or {}).items()}
        request_id = headers.get("x-request-id") or make_request_id()
        started = time.perf_counter()
        status_code = 500
        error_code: int | None = None
        response: Response
        try:
            response = self._dispatch(
                method.upper(),
                raw_path,
                headers,
                body,
            )
            status_code = response.status_code
            error_code = response.body.get("code") if status_code >= 400 else None
        except ApiError as exc:
            status_code = exc.status_code
            error_code = exc.code
            response = Response(status_code, exc.payload(request_id))
        except (sqlite3.Error, OSError):
            status_code = 500
            error_code = 2001
            response = Response(
                500,
                {
                    "code": 2001,
                    "message": "database write failed",
                    "data": None,
                    "request_id": request_id,
                },
            )
        except Exception:
            status_code = 500
            error_code = 2001
            response = Response(
                500,
                {
                    "code": 2001,
                    "message": "internal server error",
                    "data": None,
                    "request_id": request_id,
                },
            )

        try:
            self.database.log_request(
                {
                    "request_id": request_id,
                    "requested_at": utc_now_iso(),
                    "source_ip": source_ip,
                    "gateway_id": headers.get("x-gateway-id"),
                    "method": method.upper(),
                    "path": urlparse(raw_path).path,
                    "status_code": status_code,
                    "error_code": error_code,
                    "duration_ms": round((time.perf_counter() - started) * 1000, 2),
                }
            )
        except Exception:
            # Request logging must not hide the API response.
            pass

        if response.status_code >= 400:
            response.body.setdefault("request_id", request_id)
        return response

    def _dispatch(
        self,
        method: str,
        raw_path: str,
        headers: dict[str, str],
        body: bytes | str | None,
    ) -> Response:
        parsed = urlparse(raw_path)
        path = parsed.path.rstrip("/") or "/"
        query = parse_qs(parsed.query)

        if path == f"{self.prefix}/health" and method == "GET":
            return self._health()
        if path == f"{self.prefix}/telemetry" and method == "POST":
            gateway_id = self._authenticate_gateway(headers)
            payload = self._json_body(body)
            return self._post_telemetry(payload, gateway_id, headers)
        if path == f"{self.prefix}/telemetry/batch" and method == "POST":
            gateway_id = self._authenticate_gateway(headers)
            payload = self._json_body(body)
            return self._post_telemetry_batch(payload, gateway_id, headers)
        if path == f"{self.prefix}/gateways/heartbeat" and method == "POST":
            gateway_id = self._authenticate_gateway(headers)
            payload = self._json_body(body)
            return self._post_heartbeat(payload, gateway_id)
        if path == f"{self.prefix}/devices" and method == "GET":
            return self._get_devices(query)
        if path == f"{self.prefix}/telemetry/latest" and method == "GET":
            return self._get_latest(query)
        if path == f"{self.prefix}/telemetry/history" and method == "GET":
            return self._get_history(query)
        if path == f"{self.prefix}/alerts" and method == "GET":
            return self._get_alerts(query)
        if path.startswith(f"{self.prefix}/alerts/") and path.endswith("/ack"):
            if method != "POST":
                raise not_found("endpoint not found")
            alert_id = path[len(f"{self.prefix}/alerts/") : -len("/ack")]
            payload = self._json_body(body)
            return self._ack_alert(alert_id, payload)
        raise not_found("endpoint not found")

    def _health(self) -> Response:
        row = self.database.fetchone("SELECT 1 AS ok")
        if not row or row["ok"] != 1:
            raise ApiError(503, 2002, "service temporarily unavailable")
        return self._ok(
            {
                "service": "cold-chain-backend",
                "status": "up",
                "database": "up",
                "timestamp": utc_now_iso(),
            }
        )

    def _authenticate_gateway(self, headers: dict[str, str]) -> str:
        gateway_id = headers.get("x-gateway-id")
        api_key = headers.get("x-api-key")
        expected = self.settings.gateway_api_keys.get(gateway_id or "")
        if not gateway_id or not api_key or not expected:
            raise ApiError(401, 1004, "invalid api key")
        if not hmac.compare_digest(api_key, expected):
            raise ApiError(401, 1004, "invalid api key")
        gateway = self.database.fetchone(
            "SELECT gateway_id, enabled FROM gateways WHERE gateway_id = ?",
            (gateway_id,),
        )
        if not gateway or not gateway["enabled"]:
            raise ApiError(401, 1004, "invalid api key")
        return gateway_id

    @staticmethod
    def _json_body(body: bytes | str | None) -> dict[str, Any]:
        if body is None or body == b"" or body == "":
            raise bad_request(1001, "invalid JSON body")
        try:
            decoded = body.decode("utf-8") if isinstance(body, bytes) else body
            payload = json.loads(decoded)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise bad_request(1001, "invalid JSON body") from exc
        if not isinstance(payload, dict):
            raise bad_request(1001, "JSON body must be an object")
        return payload

    def _post_telemetry(
        self,
        payload: dict[str, Any],
        gateway_id: str,
        headers: dict[str, str],
    ) -> Response:
        item = self._validate_telemetry(payload, gateway_id)
        existing = self.database.fetchone(
            "SELECT message_id FROM telemetry WHERE message_id = ?",
            (item["message_id"],),
        )
        if existing:
            return self._ok(
                {
                    "message_id": item["message_id"],
                    "stored": False,
                    "duplicate": True,
                },
                message="already accepted",
            )

        inserted = self.database.insert_telemetry(item)
        if not inserted:
            return self._ok(
                {
                    "message_id": item["message_id"],
                    "stored": False,
                    "duplicate": True,
                },
                message="already accepted",
            )
        self._update_alerts(item)
        self.database.log_raw_telemetry(
            {
                **item,
                "source": "gateway",
                "api_key": "[redacted]",
                "headers": {
                    "x-gateway-id": headers.get("x-gateway-id"),
                },
            }
        )
        return self._ok(
            {
                "message_id": item["message_id"],
                "stored": True,
                "duplicate": False,
            },
            message="accepted",
        )

    def _post_telemetry_batch(
        self,
        payload: dict[str, Any],
        gateway_id: str,
        headers: dict[str, str],
    ) -> Response:
        body_gateway_id = payload.get("gateway_id")
        if body_gateway_id != gateway_id:
            raise ApiError(409, 1006, "gateway id conflict")
        items = payload.get("items")
        if not isinstance(items, list) or not items:
            raise bad_request(1003, "items is required")
        if len(items) > 100:
            raise bad_request(1002, "batch size must not exceed 100")

        accepted: list[str] = []
        duplicates: list[str] = []
        failed: list[dict[str, Any]] = []
        for raw_item in items:
            if not isinstance(raw_item, dict):
                failed.append({"message_id": None, "code": 1001, "message": "item must be an object"})
                continue
            message_id = raw_item.get("message_id")
            try:
                item = dict(raw_item)
                item["gateway_id"] = gateway_id
                normalized = self._validate_telemetry(item, gateway_id)
                existing = self.database.fetchone(
                    "SELECT message_id FROM telemetry WHERE message_id = ?",
                    (normalized["message_id"],),
                )
                if existing or not self.database.insert_telemetry(normalized):
                    duplicates.append(normalized["message_id"])
                    continue
                self._update_alerts(normalized)
                self.database.log_raw_telemetry(
                    {
                        **normalized,
                        "source": "gateway-batch",
                        "api_key": "[redacted]",
                        "headers": {"x-gateway-id": headers.get("x-gateway-id")},
                    }
                )
                accepted.append(normalized["message_id"])
            except ApiError as exc:
                failed.append(
                    {
                        "message_id": message_id,
                        "code": exc.code,
                        "message": exc.message,
                    }
                )
            except ValueError as exc:
                failed.append(
                    {
                        "message_id": message_id,
                        "code": 1002,
                        "message": str(exc),
                    }
                )

        return self._ok(
            {
                "total": len(items),
                "accepted": accepted,
                "duplicates": duplicates,
                "failed": failed,
            },
            message="batch processed",
        )

    def _post_heartbeat(
        self,
        payload: dict[str, Any],
        gateway_id: str,
    ) -> Response:
        required = (
            "gateway_id",
            "firmware_version",
            "ip_address",
            "uptime_seconds",
            "connected_nodes",
            "free_heap",
            "sent_at",
        )
        self._require_fields(payload, required)
        if payload["gateway_id"] != gateway_id:
            raise ApiError(409, 1006, "gateway id conflict")
        for field in ("firmware_version", "ip_address"):
            if not isinstance(payload[field], str) or not payload[field].strip():
                raise bad_request(1002, f"{field} is invalid")
        for field in ("uptime_seconds", "connected_nodes", "free_heap"):
            try:
                value = validate_integer(payload[field], field)
            except ValueError as exc:
                raise bad_request(1002, str(exc)) from exc
            if value < 0:
                raise bad_request(1002, f"{field} must be non-negative")
        try:
            sent_at = canonical_iso_datetime(payload["sent_at"], "sent_at")
        except ValueError as exc:
            raise bad_request(1002, str(exc)) from exc

        normalized = {
            "gateway_id": gateway_id,
            "firmware_version": payload["firmware_version"].strip(),
            "ip_address": payload["ip_address"].strip(),
            "uptime_seconds": payload["uptime_seconds"],
            "connected_nodes": payload["connected_nodes"],
            "free_heap": payload["free_heap"],
            "sent_at": sent_at,
        }
        self.database.update_gateway_heartbeat(normalized)
        return self._ok(
            {
                "gateway_id": gateway_id,
                "accepted": True,
                "received_at": utc_now_iso(),
            },
            message="heartbeat accepted",
        )

    def _validate_telemetry(
        self,
        payload: dict[str, Any],
        gateway_id: str,
    ) -> dict[str, Any]:
        required = (
            "message_id",
            "gateway_id",
            "device_id",
            "zone_id",
            "collected_at",
            "temperature",
            "quality",
        )
        self._require_fields(payload, required)
        if payload["gateway_id"] != gateway_id:
            raise ApiError(409, 1006, "gateway id conflict")

        try:
            message_id = validate_identifier(payload["message_id"], "message_id")
            device_id = validate_identifier(payload["device_id"], "device_id")
            zone_id = validate_identifier(payload["zone_id"], "zone_id")
            collected_at = canonical_iso_datetime(payload["collected_at"], "collected_at")
            received_at = canonical_iso_datetime(
                payload.get("received_at", utc_now_iso()),
                "received_at",
            )
            temperature = validate_number(payload["temperature"], "temperature")
            humidity = payload.get("humidity")
            if humidity is not None:
                humidity = validate_number(humidity, "humidity")
            signal = payload.get("signal")
            if signal is not None:
                signal = validate_integer(signal, "signal")
            quality = payload["quality"]
        except ValueError as exc:
            raise bad_request(1002, str(exc)) from exc

        if not isinstance(quality, str) or quality not in {
            "normal",
            "warning",
            "invalid",
        }:
            raise bad_request(1002, "quality is invalid")
        if not -50 <= temperature <= 80:
            raise bad_request(1002, "invalid temperature")
        if humidity is not None and not 0 <= humidity <= 100:
            raise bad_request(1002, "invalid humidity")
        if signal is not None and not -150 <= signal <= 0:
            raise bad_request(1002, "invalid signal")

        device = self.database.fetchone(
            """
            SELECT device_id, gateway_id, zone_id, enabled
            FROM devices
            WHERE device_id = ?
            """,
            (device_id,),
        )
        if not device:
            raise not_found("device not found")
        if (
            not device["enabled"]
            or device["gateway_id"] != gateway_id
            or device["zone_id"] != zone_id
        ):
            raise ApiError(409, 1006, "device configuration conflict")

        return {
            "message_id": message_id,
            "gateway_id": gateway_id,
            "device_id": device_id,
            "zone_id": zone_id,
            "collected_at": collected_at,
            "received_at": received_at,
            "temperature": temperature,
            "humidity": humidity,
            "signal": signal,
            "quality": quality,
        }

    def _update_alerts(self, item: dict[str, Any]) -> None:
        temperature = item["temperature"]
        if temperature > self.settings.temp_high_threshold:
            self._upsert_alert(
                item,
                alert_type="temperature_high",
                value=temperature,
                threshold=self.settings.temp_high_threshold,
            )
        elif temperature < self.settings.temp_low_threshold:
            self._upsert_alert(
                item,
                alert_type="temperature_low",
                value=temperature,
                threshold=self.settings.temp_low_threshold,
            )
        else:
            self.database.close_open_alerts(item["device_id"], "temperature_high")
            self.database.close_open_alerts(item["device_id"], "temperature_low")

    def _upsert_alert(
        self,
        item: dict[str, Any],
        *,
        alert_type: str,
        value: float,
        threshold: float,
    ) -> None:
        existing = self.database.fetchone(
            """
            SELECT alert_id FROM alerts
            WHERE device_id = ? AND type = ? AND status = 'open'
            ORDER BY created_at DESC
            LIMIT 1
            """,
            (item["device_id"], alert_type),
        )
        if existing:
            self.database.execute(
                """
                UPDATE alerts
                SET message_id = ?, value = ?, threshold = ?
                WHERE alert_id = ?
                """,
                (
                    item["message_id"],
                    value,
                    threshold,
                    existing["alert_id"],
                ),
            )
            return
        self.database.create_alert(
            alert_id=new_alert_id(),
            message_id=item["message_id"],
            device_id=item["device_id"],
            zone_id=item["zone_id"],
            alert_type=alert_type,
            level="warning",
            value=value,
            threshold=threshold,
        )

    def _get_devices(self, query: dict[str, list[str]]) -> Response:
        zone_id = self._query_one(query, "zone_id")
        enabled_raw = self._query_one(query, "enabled")
        params: list[Any] = []
        where = []
        if zone_id:
            where.append("d.zone_id = ?")
            params.append(zone_id)
        if enabled_raw is not None:
            if enabled_raw not in {"true", "false"}:
                raise bad_request(1002, "enabled must be true or false")
            where.append("d.enabled = ?")
            params.append(1 if enabled_raw == "true" else 0)
        sql = """
            SELECT d.device_id, d.gateway_id, d.zone_id, d.device_type,
                   d.sensor_type, d.enabled
            FROM devices d
        """
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY d.device_id"
        rows = self.database.fetchall(sql, params)
        return self._ok({"items": [self._device_dict(row) for row in rows], "total": len(rows)})

    def _get_latest(self, query: dict[str, list[str]]) -> Response:
        zone_id = self._query_one(query, "zone_id")
        params: list[Any] = []
        filter_sql = ""
        if zone_id:
            filter_sql = " AND t.zone_id = ?"
            params.append(zone_id)
        rows = self.database.fetchall(
            f"""
            SELECT t.device_id, t.gateway_id, t.zone_id, t.temperature,
                   t.humidity, t.quality, t.collected_at, g.last_seen_at
            FROM telemetry t
            JOIN gateways g ON g.gateway_id = t.gateway_id
            WHERE NOT EXISTS (
                SELECT 1 FROM telemetry newer
                WHERE newer.device_id = t.device_id
                  AND (
                    newer.collected_at > t.collected_at OR
                    (newer.collected_at = t.collected_at
                     AND newer.received_at > t.received_at)
                  )
            )
            {filter_sql}
            ORDER BY t.device_id
            """,
            params,
        )
        now = utc_now()
        data = []
        for row in rows:
            last_seen = self._safe_datetime(row["last_seen_at"])
            online = bool(
                last_seen
                and now - last_seen
                <= timedelta(seconds=self.settings.heartbeat_timeout_seconds)
            )
            data.append(
                {
                    "device_id": row["device_id"],
                    "zone_id": row["zone_id"],
                    "temperature": row["temperature"],
                    "humidity": row["humidity"],
                    "quality": row["quality"],
                    "collected_at": row["collected_at"],
                    "online": online,
                }
            )
        return self._ok(data)

    def _get_history(self, query: dict[str, list[str]]) -> Response:
        device_id = self._query_one(query, "device_id")
        zone_id = self._query_one(query, "zone_id")
        if not device_id and not zone_id:
            raise bad_request(1003, "device_id or zone_id is required")

        start_raw = self._query_one(query, "start")
        end_raw = self._query_one(query, "end")
        if bool(start_raw) != bool(end_raw):
            raise bad_request(1002, "start and end must be provided together")
        params: list[Any] = []
        where: list[str] = []
        if device_id:
            where.append("t.device_id = ?")
            params.append(device_id)
        if zone_id:
            where.append("t.zone_id = ?")
            params.append(zone_id)
        if start_raw and end_raw:
            try:
                start = parse_iso_datetime(start_raw, "start")
                end = parse_iso_datetime(end_raw, "end")
            except ValueError as exc:
                raise bad_request(1002, str(exc)) from exc
            if end <= start:
                raise bad_request(1002, "end must be greater than start")
            if end - start > timedelta(days=7):
                raise bad_request(1002, "time range must not exceed 7 days")
            where.extend(["t.collected_at >= ?", "t.collected_at <= ?"])
            params.extend(
                [
                    start.isoformat(timespec="seconds").replace("+00:00", "Z"),
                    end.isoformat(timespec="seconds").replace("+00:00", "Z"),
                ]
            )

        page = self._positive_int_query(query, "page", default=1, maximum=1000000)
        page_size = self._positive_int_query(query, "page_size", default=100, maximum=500)
        where_sql = " AND ".join(where)
        total_row = self.database.fetchone(
            f"SELECT COUNT(*) AS total FROM telemetry t WHERE {where_sql}",
            params,
        )
        total = int(total_row["total"])
        offset = (page - 1) * page_size
        rows = self.database.fetchall(
            f"""
            SELECT t.message_id, t.gateway_id, t.device_id, t.zone_id,
                   t.collected_at, t.received_at, t.temperature, t.humidity,
                   t.signal, t.quality
            FROM telemetry t
            WHERE {where_sql}
            ORDER BY t.collected_at DESC, t.received_at DESC
            LIMIT ? OFFSET ?
            """,
            [*params, page_size, offset],
        )
        return self._ok(
            {
                "items": [self._telemetry_dict(row) for row in rows],
                "page": page,
                "page_size": page_size,
                "total": total,
            }
        )

    def _get_alerts(self, query: dict[str, list[str]]) -> Response:
        status = self._query_one(query, "status")
        level = self._query_one(query, "level")
        device_id = self._query_one(query, "device_id")
        zone_id = self._query_one(query, "zone_id")
        if status and status not in {"open", "acknowledged", "closed"}:
            raise bad_request(1002, "status is invalid")
        if level and level not in {"warning", "critical", "info"}:
            raise bad_request(1002, "level is invalid")

        where: list[str] = []
        params: list[Any] = []
        for field, value in (
            ("a.status", status),
            ("a.level", level),
            ("a.device_id", device_id),
            ("a.zone_id", zone_id),
        ):
            if value:
                where.append(f"{field} = ?")
                params.append(value)
        where_sql = " AND ".join(where) or "1 = 1"
        page = self._positive_int_query(query, "page", default=1, maximum=1000000)
        page_size = self._positive_int_query(query, "page_size", default=20, maximum=500)
        total_row = self.database.fetchone(
            f"SELECT COUNT(*) AS total FROM alerts a WHERE {where_sql}",
            params,
        )
        total = int(total_row["total"])
        rows = self.database.fetchall(
            f"""
            SELECT a.alert_id, a.message_id, a.device_id, a.zone_id,
                   a.type, a.level, a.value, a.threshold, a.status,
                   a.created_at, a.acknowledged_at, a.operator, a.remark
            FROM alerts a
            WHERE {where_sql}
            ORDER BY a.created_at DESC
            LIMIT ? OFFSET ?
            """,
            [*params, page_size, (page - 1) * page_size],
        )
        return self._ok(
            {
                "items": [self._alert_dict(row) for row in rows],
                "page": page,
                "page_size": page_size,
                "total": total,
            }
        )

    def _ack_alert(self, alert_id: str, payload: dict[str, Any]) -> Response:
        try:
            validate_identifier(alert_id, "alert_id")
        except ValueError as exc:
            raise not_found("alert not found") from exc
        self._require_fields(payload, ("operator",))
        operator = payload.get("operator")
        remark = payload.get("remark", "")
        if not isinstance(operator, str) or not operator.strip():
            raise bad_request(1002, "operator is invalid")
        if not isinstance(remark, str):
            raise bad_request(1002, "remark is invalid")
        existing = self.database.fetchone(
            "SELECT alert_id FROM alerts WHERE alert_id = ?",
            (alert_id,),
        )
        if not existing:
            raise not_found("alert not found")
        self.database.execute(
            """
            UPDATE alerts
            SET status = 'acknowledged', acknowledged_at = ?,
                operator = ?, remark = ?
            WHERE alert_id = ?
            """,
            (utc_now_iso(), operator.strip(), remark.strip(), alert_id),
        )
        row = self.database.fetchone(
            """
            SELECT alert_id, message_id, device_id, zone_id, type, level,
                   value, threshold, status, created_at, acknowledged_at,
                   operator, remark
            FROM alerts WHERE alert_id = ?
            """,
            (alert_id,),
        )
        return self._ok(self._alert_dict(row), message="alert acknowledged")

    @staticmethod
    def _require_fields(payload: dict[str, Any], fields: tuple[str, ...]) -> None:
        missing = [field for field in fields if field not in payload]
        if missing:
            raise bad_request(1003, f"missing required field: {missing[0]}")

    @staticmethod
    def _query_one(query: dict[str, list[str]], key: str) -> str | None:
        values = query.get(key)
        return values[0] if values else None

    @staticmethod
    def _positive_int_query(
        query: dict[str, list[str]],
        key: str,
        *,
        default: int,
        maximum: int,
    ) -> int:
        raw = ColdChainService._query_one(query, key)
        if raw is None:
            return default
        try:
            value = int(raw)
        except ValueError as exc:
            raise bad_request(1002, f"{key} must be an integer") from exc
        if value < 1 or value > maximum:
            raise bad_request(1002, f"{key} is out of range")
        return value

    @staticmethod
    def _safe_datetime(value: str | None):
        if not value:
            return None
        try:
            return parse_iso_datetime(value, "last_seen_at")
        except ValueError:
            return None

    @staticmethod
    def _device_dict(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "device_id": row["device_id"],
            "gateway_id": row["gateway_id"],
            "zone_id": row["zone_id"],
            "device_type": row["device_type"],
            "sensor_type": row["sensor_type"],
            "enabled": bool(row["enabled"]),
        }

    @staticmethod
    def _telemetry_dict(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "message_id": row["message_id"],
            "gateway_id": row["gateway_id"],
            "device_id": row["device_id"],
            "zone_id": row["zone_id"],
            "collected_at": row["collected_at"],
            "received_at": row["received_at"],
            "temperature": row["temperature"],
            "humidity": row["humidity"],
            "signal": row["signal"],
            "quality": row["quality"],
        }

    @staticmethod
    def _alert_dict(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "alert_id": row["alert_id"],
            "message_id": row["message_id"],
            "device_id": row["device_id"],
            "zone_id": row["zone_id"],
            "type": row["type"],
            "level": row["level"],
            "value": row["value"],
            "threshold": row["threshold"],
            "status": row["status"],
            "created_at": row["created_at"],
            "acknowledged_at": row["acknowledged_at"],
            "operator": row["operator"],
            "remark": row["remark"],
        }

    @staticmethod
    def _ok(data: Any, message: str = "ok", status_code: int = 200) -> Response:
        return Response(
            status_code,
            {
                "code": 0,
                "message": message,
                "data": data,
            },
        )

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from backend.config import Settings
from backend.service import ColdChainService


class BackendApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        root = Path(self.temp_dir.name)
        self.gateway_id = "esp32-gateway-001"
        self.api_key = "test-gateway-key"
        self.service = ColdChainService(
            Settings(
                database_path=root / "cold_chain.db",
                log_dir=root / "logs",
                gateway_api_keys={self.gateway_id: self.api_key},
            )
        )
        self.headers = {
            "Content-Type": "application/json",
            "X-Gateway-Id": self.gateway_id,
            "X-Api-Key": self.api_key,
        }

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def call(self, method: str, path: str, payload=None, headers=None):
        body = None
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
        return self.service.dispatch(method, path, headers or {}, body)

    def telemetry(self, message_id="msg-001", temperature=-18.6):
        return {
            "message_id": message_id,
            "gateway_id": self.gateway_id,
            "device_id": "zb-node-001",
            "zone_id": "cold-storage-a",
            "collected_at": "2026-09-19T07:30:00Z",
            "received_at": "2026-09-19T07:30:01Z",
            "temperature": temperature,
            "humidity": None,
            "signal": -62,
            "quality": "normal",
        }

    def test_health_and_devices(self):
        health = self.call("GET", "/api/v1/health")
        self.assertEqual(health.status_code, 200)
        self.assertEqual(health.body["data"]["database"], "up")

        devices = self.call("GET", "/api/v1/devices?enabled=true")
        self.assertEqual(devices.status_code, 200)
        self.assertEqual(devices.body["data"]["total"], 3)

    def test_telemetry_is_idempotent_and_queryable(self):
        first = self.call(
            "POST",
            "/api/v1/telemetry",
            self.telemetry(),
            self.headers,
        )
        self.assertEqual(first.status_code, 200)
        self.assertTrue(first.body["data"]["stored"])

        duplicate = self.call(
            "POST",
            "/api/v1/telemetry",
            self.telemetry(),
            self.headers,
        )
        self.assertEqual(duplicate.status_code, 200)
        self.assertTrue(duplicate.body["data"]["duplicate"])

        latest = self.call("GET", "/api/v1/telemetry/latest?zone_id=cold-storage-a")
        self.assertEqual(latest.status_code, 200)
        self.assertEqual(len(latest.body["data"]), 1)
        self.assertEqual(latest.body["data"][0]["temperature"], -18.6)

        history = self.call(
            "GET",
            "/api/v1/telemetry/history?device_id=zb-node-001&page_size=10",
        )
        self.assertEqual(history.status_code, 200)
        self.assertEqual(history.body["data"]["total"], 1)

    def test_alert_creation_and_acknowledgement(self):
        response = self.call(
            "POST",
            "/api/v1/telemetry",
            self.telemetry("msg-high", temperature=-10.0),
            self.headers,
        )
        self.assertEqual(response.status_code, 200)

        alerts = self.call("GET", "/api/v1/alerts?status=open")
        self.assertEqual(alerts.body["data"]["total"], 1)
        alert_id = alerts.body["data"]["items"][0]["alert_id"]
        acknowledged = self.call(
            "POST",
            f"/api/v1/alerts/{alert_id}/ack",
            {"operator": "member6", "remark": "已确认"},
        )
        self.assertEqual(acknowledged.status_code, 200)
        self.assertEqual(
            acknowledged.body["data"]["status"],
            "acknowledged",
        )

    def test_heartbeat_marks_gateway_online(self):
        heartbeat = {
            "gateway_id": self.gateway_id,
            "firmware_version": "0.1.0",
            "ip_address": "192.168.1.20",
            "uptime_seconds": 3600,
            "connected_nodes": 3,
            "free_heap": 182432,
            "sent_at": "2026-09-19T07:30:00Z",
        }
        response = self.call(
            "POST",
            "/api/v1/gateways/heartbeat",
            heartbeat,
            self.headers,
        )
        self.assertEqual(response.status_code, 200)
        self.call(
            "POST",
            "/api/v1/telemetry",
            self.telemetry("msg-online"),
            self.headers,
        )
        latest = self.call("GET", "/api/v1/telemetry/latest")
        self.assertTrue(latest.body["data"][0]["online"])

    def test_batch_reports_failed_items_without_losing_valid_items(self):
        payload = {
            "gateway_id": self.gateway_id,
            "items": [
                {
                    "message_id": "batch-001",
                    "device_id": "zb-node-001",
                    "zone_id": "cold-storage-a",
                    "collected_at": "2026-09-19T07:30:00Z",
                    "temperature": -18.6,
                    "humidity": None,
                    "quality": "normal",
                },
                {
                    "message_id": "batch-bad",
                    "device_id": "zb-node-001",
                    "zone_id": "cold-storage-a",
                    "collected_at": "2026-09-19T07:30:00Z",
                    "temperature": 100,
                    "humidity": None,
                    "quality": "normal",
                },
            ],
        }
        response = self.call(
            "POST",
            "/api/v1/telemetry/batch",
            payload,
            self.headers,
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.body["data"]["accepted"], ["batch-001"])
        self.assertEqual(response.body["data"]["failed"][0]["code"], 1002)

    def test_authentication_and_validation_errors(self):
        unauthorized = self.call(
            "POST",
            "/api/v1/telemetry",
            self.telemetry(),
            {**self.headers, "X-Api-Key": "wrong"},
        )
        self.assertEqual(unauthorized.status_code, 401)
        self.assertEqual(unauthorized.body["code"], 1004)

        invalid = self.call(
            "POST",
            "/api/v1/telemetry",
            {**self.telemetry("msg-invalid"), "temperature": 90},
            self.headers,
        )
        self.assertEqual(invalid.status_code, 400)
        self.assertEqual(invalid.body["code"], 1002)


if __name__ == "__main__":
    unittest.main()

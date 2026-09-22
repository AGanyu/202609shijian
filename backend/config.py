from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path


def _parse_gateway_keys() -> dict[str, str]:
    raw = os.getenv("GATEWAY_API_KEYS", "").strip()
    if raw:
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict) and parsed:
                return {str(key): str(value) for key, value in parsed.items()}
        except json.JSONDecodeError as exc:
            raise ValueError("GATEWAY_API_KEYS must be valid JSON") from exc

    gateway_id = os.getenv("GATEWAY_ID", "esp32-gateway-001")
    api_key = os.getenv("GATEWAY_API_KEY", "dev-gateway-key")
    return {gateway_id: api_key}


@dataclass(frozen=True)
class Settings:
    host: str = "0.0.0.0"
    port: int = 8000
    database_path: Path = Path("data/cold_chain.db")
    log_dir: Path = Path("data/logs")
    gateway_api_keys: dict[str, str] = field(
        default_factory=lambda: {"esp32-gateway-001": "dev-gateway-key"}
    )
    temp_low_threshold: float = -25.0
    temp_high_threshold: float = -18.0
    heartbeat_timeout_seconds: int = 90
    cors_origins: tuple[str, ...] = ("*",)

    @classmethod
    def from_env(cls) -> "Settings":
        database_path = Path(os.getenv("DATABASE_PATH", "data/cold_chain.db"))
        log_dir = Path(os.getenv("LOG_DIR", "data/logs"))
        cors_raw = os.getenv("CORS_ORIGINS", "*")
        cors_origins = tuple(
            origin.strip() for origin in cors_raw.split(",") if origin.strip()
        ) or ("*",)
        return cls(
            host=os.getenv("HOST", "0.0.0.0"),
            port=int(os.getenv("PORT", "8000")),
            database_path=database_path,
            log_dir=log_dir,
            gateway_api_keys=_parse_gateway_keys(),
            temp_low_threshold=float(os.getenv("TEMP_LOW_THRESHOLD", "-25")),
            temp_high_threshold=float(os.getenv("TEMP_HIGH_THRESHOLD", "-18")),
            heartbeat_timeout_seconds=int(
                os.getenv("HEARTBEAT_TIMEOUT_SECONDS", "90")
            ),
            cors_origins=cors_origins,
        )

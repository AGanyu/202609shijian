from __future__ import annotations

import math
import re
import uuid
from datetime import datetime, timezone
from typing import Any


IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9._:-]{1,100}$")


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def utc_now_iso() -> str:
    return utc_now().isoformat(timespec="seconds").replace("+00:00", "Z")


def request_id() -> str:
    return f"req-{uuid.uuid4().hex[:16]}"


def new_alert_id() -> str:
    return f"alert-{uuid.uuid4().hex[:16]}"


def parse_iso_datetime(value: Any, field_name: str) -> datetime:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field_name} must be an ISO 8601 datetime")
    raw = value.strip()
    try:
        parsed = datetime.fromisoformat(raw.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError(f"{field_name} must be an ISO 8601 datetime") from exc
    if parsed.tzinfo is None:
        raise ValueError(f"{field_name} must include a timezone")
    return parsed.astimezone(timezone.utc)


def canonical_iso_datetime(value: Any, field_name: str) -> str:
    return parse_iso_datetime(value, field_name).isoformat(timespec="seconds").replace(
        "+00:00", "Z"
    )


def validate_identifier(value: Any, field_name: str) -> str:
    if not isinstance(value, str) or not IDENTIFIER_RE.fullmatch(value):
        raise ValueError(f"{field_name} is invalid")
    return value


def validate_number(value: Any, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field_name} must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{field_name} must be finite")
    return number


def validate_integer(value: Any, field_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field_name} must be an integer")
    return value


def json_safe(value: Any) -> Any:
    if isinstance(value, datetime):
        return value.isoformat(timespec="seconds").replace("+00:00", "Z")
    return value

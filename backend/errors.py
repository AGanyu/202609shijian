from __future__ import annotations


class ApiError(Exception):
    def __init__(self, status_code: int, code: int, message: str):
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message

    def payload(self, request_id: str) -> dict:
        return {
            "code": self.code,
            "message": self.message,
            "data": None,
            "request_id": request_id,
        }


def bad_request(code: int, message: str) -> ApiError:
    return ApiError(400, code, message)


def not_found(message: str) -> ApiError:
    return ApiError(404, 1005, message)

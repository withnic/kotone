"""Kotone (琴音) offline voice profiler package."""

__version__ = "1.0.0"


class KotoneProfilerError(Exception):
    """Structured error carrying Kotone error code and CLI exit code."""

    def __init__(self, code: str, message: str, exit_code: int = 1) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.exit_code = exit_code

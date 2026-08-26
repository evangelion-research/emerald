"""A minimal LSP client: enough framing to drive the server in a test.

`pygls` ships in-process helpers, but running the real `python -m emlsp`
over pipes is what actually proves the packaging, the stdio transport, and the
handler signatures at once -- the three things that break silently.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Session:
    def __init__(self, root: Path | None = None) -> None:
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "emlsp"],
            cwd=str(ROOT),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.next_id = 0
        self.root = root

    def __enter__(self) -> "Session":
        self.request(
            "initialize",
            {
                "processId": None,
                "rootUri": self.root.as_uri() if self.root else None,
                "capabilities": {},
                "workspaceFolders": (
                    [{"uri": self.root.as_uri(), "name": self.root.name}]
                    if self.root
                    else None
                ),
            },
        )
        self.notify("initialized", {})
        return self

    def __exit__(self, *exc: object) -> None:
        try:
            self.request("shutdown", None)
            self.notify("exit", None)
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        finally:
            for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
                if stream:
                    stream.close()

    # -- transport -------------------------------------------------------
    def _send(self, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        assert self.proc.stdin is not None
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def _read(self) -> dict:
        assert self.proc.stdout is not None
        length = 0
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("server closed the connection")
            if line in (b"\r\n", b"\n"):
                break
            name, _, value = line.decode("ascii").partition(":")
            if name.strip().lower() == "content-length":
                length = int(value.strip())
        return json.loads(self.proc.stdout.read(length))

    def request(self, method: str, params: object, timeout: float = 20.0) -> dict:
        self.next_id += 1
        request_id = self.next_id
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self._read()
            if message.get("id") == request_id:
                if "error" in message:
                    raise AssertionError(f"{method} failed: {message['error']}")
                return message.get("result")

    def notify(self, method: str, params: object) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def open(self, path: Path, text: str | None = None, version: int = 1) -> str:
        uri = path.as_uri()
        self.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "emerald",
                    "version": version,
                    "text": text if text is not None else path.read_text(),
                }
            },
        )
        return uri

    def wait_for(self, method: str, timeout: float = 20.0) -> dict:
        """Drain notifications until one of `method` arrives."""
        while True:
            message = self._read()
            if message.get("method") == method:
                return message.get("params", {})

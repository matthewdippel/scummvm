"""
Driver for scummvm-as-MCP-server.

Spawns `scummvm --mcp <target>` as a subprocess, speaks JSON-RPC over its
stdio per the Model Context Protocol. Used both for testing the engine-side
MCP work and as a foundation for the eventual TAS authoring tool.

Usage:
    python3 driver.py smoke              # default smoke test
    python3 driver.py shell              # interactive REPL
    python3 driver.py raw '{"jsonrpc":...}'   # send one raw request

Library usage:
    with McpDriver() as d:
        tools = d.list_tools()
        d.call_tool("pause")
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import shlex
import subprocess
import sys
import threading
import time
from typing import Any, Iterator, Optional


PROTOCOL_VERSION = "2024-11-05"
CLIENT_NAME = "shivers-driver"
CLIENT_VERSION = "0.1"

# Default scummvm binary lives at the repo root, two levels up from this file.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCUMMVM = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "scummvm"))


class McpError(RuntimeError):
    def __init__(self, code: int, message: str, data: Any = None):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.data = data


class McpDriver:
    """Synchronous MCP client over a child scummvm process's stdio."""

    def __init__(
        self,
        scummvm_path: str = DEFAULT_SCUMMVM,
        target: str = "Shivers",
        *,
        extra_args: Optional[list[str]] = None,
        forward_stderr: bool = True,
        startup_timeout: float = 5.0,
    ):
        argv = [scummvm_path, "--mcp"]
        if extra_args:
            argv.extend(extra_args)
        argv.append(target)

        self._argv = argv
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None if forward_stderr else subprocess.DEVNULL,
            bufsize=0,  # unbuffered binary
        )
        self._next_id = 1
        self._initialized = False
        self._closed = False
        self._lock = threading.Lock()
        self._startup_timeout = startup_timeout

    # ── Low-level transport ────────────────────────────────────────────────

    def _send_request(self, method: str, params: Any = None) -> dict:
        with self._lock:
            req: dict[str, Any] = {"jsonrpc": "2.0", "method": method, "id": self._next_id}
            self._next_id += 1
            if params is not None:
                req["params"] = params
            line = (json.dumps(req) + "\n").encode("utf-8")
            assert self.proc.stdin is not None
            self.proc.stdin.write(line)
            self.proc.stdin.flush()

            # Read a single line response. (MCP doesn't pipeline notifications
            # back during a request/response in our server, so this is safe.)
            assert self.proc.stdout is not None
            raw = self.proc.stdout.readline()
            if not raw:
                rc = self.proc.poll()
                raise McpError(-32000, f"scummvm closed stdout (returncode={rc})")
            try:
                resp = json.loads(raw)
            except json.JSONDecodeError as e:
                raise McpError(-32700, f"bad JSON from server: {e}; raw={raw!r}")
            if "error" in resp:
                err = resp["error"]
                raise McpError(err.get("code", -32603), err.get("message", "unknown error"), err.get("data"))
            return resp.get("result", {})

    def _send_notification(self, method: str, params: Any = None) -> None:
        with self._lock:
            note: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
            if params is not None:
                note["params"] = params
            line = (json.dumps(note) + "\n").encode("utf-8")
            assert self.proc.stdin is not None
            self.proc.stdin.write(line)
            self.proc.stdin.flush()

    # ── MCP handshake ──────────────────────────────────────────────────────

    def initialize(self) -> dict:
        result = self._send_request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": CLIENT_NAME, "version": CLIENT_VERSION},
            },
        )
        self._send_notification("notifications/initialized")
        self._initialized = True
        return result

    def list_tools(self) -> list[dict]:
        result = self._send_request("tools/list", {})
        return result.get("tools", [])

    def call_tool(self, name: str, arguments: Optional[dict] = None) -> dict:
        """Returns whatever the tool's `result` is. Raises McpError on failure."""
        return self._send_request("tools/call", {"name": name, "arguments": arguments or {}})

    @staticmethod
    def _content_text(result: dict) -> str:
        """Extract the first text content item from an MCP tool result."""
        items = result.get("content", [])
        for item in items:
            if item.get("type") == "text":
                return item.get("text", "")
        return ""

    @staticmethod
    def _content_image_bytes(result: dict) -> bytes:
        """Extract the first image content item (base64-decoded) from an MCP tool result."""
        for item in result.get("content", []):
            if item.get("type") == "image":
                return base64.b64decode(item.get("data", ""))
        return b""

    # ── High-level wrappers (added as the engine side grows) ───────────────

    def pause(self) -> str:
        return self._content_text(self.call_tool("pause"))

    def unpause(self) -> str:
        return self._content_text(self.call_tool("unpause"))

    def step(self, frames: int = 1) -> int:
        result = self.call_tool("step", {"frames": frames})
        if result.get("isError"):
            raise McpError(-32000, f"step: {self._content_text(result)}")
        text = self._content_text(result)
        return int(json.loads(text)["frames_advanced"])

    def screenshot(self, save_path: Optional[str] = None) -> bytes:
        result = self.call_tool("screenshot")
        png = self._content_image_bytes(result)
        if not png:
            raise McpError(-32000, "screenshot returned no image content")
        if save_path:
            with open(save_path, "wb") as f:
                f.write(png)
        return png

    def click(self, x: int, y: int, button: str = "left") -> None:
        self.call_tool("click", {"x": x, "y": y, "button": button})

    def move_cursor(self, x: int, y: int) -> None:
        self.call_tool("move_cursor", {"x": x, "y": y})

    def save(self, slot: int, name: str) -> None:
        self.call_tool("save", {"slot": slot, "name": name})

    def restore(self, slot: int) -> None:
        self.call_tool("restore", {"slot": slot})

    def list_saves(self) -> list[dict]:
        result = self.call_tool("list_saves")
        return result.get("saves", [])

    # ── Lifecycle ──────────────────────────────────────────────────────────

    def shutdown(self) -> None:
        try:
            self._send_request("shutdown")
        except McpError:
            pass

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self.proc.poll() is None:
            try:
                self.shutdown()
            except Exception:
                pass
            try:
                if self.proc.stdin:
                    self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()

    def __enter__(self) -> "McpDriver":
        self.initialize()
        return self

    def __exit__(self, *args) -> None:
        self.close()


# ── CLI entrypoints ────────────────────────────────────────────────────────


def cmd_smoke(args: argparse.Namespace) -> int:
    print(f"Starting: {' '.join(map(shlex.quote, [args.scummvm, '--mcp', args.target]))}")
    t0 = time.time()
    with McpDriver(args.scummvm, args.target, forward_stderr=args.verbose) as d:
        print(f"  initialize     OK ({(time.time()-t0)*1000:.0f}ms)")
        tools = d.list_tools()
        names = [t["name"] for t in tools]
        print(f"  tools/list     {len(tools)} tools: {names}")
        # Sanity: calling a nonexistent tool should yield an MCP error.
        try:
            d.call_tool("definitely_not_a_real_tool")
            print("  unknown tool   FAIL (expected error, got success)")
            return 1
        except McpError as e:
            print(f"  unknown tool   OK (rejected: {e})")
        if "pause" in names and "unpause" in names:
            print(f"  pause          OK ({d.pause()!r})")
            print(f"  pause (idem)   OK ({d.pause()!r})")
            print(f"  unpause        OK ({d.unpause()!r})")
            print(f"  unpause (idem) OK ({d.unpause()!r})")
        if "screenshot" in names:
            # Let the engine boot past splash transitions before sampling.
            time.sleep(2.0)
            png = d.screenshot()
            if png[:8] != b"\x89PNG\r\n\x1a\n":
                print(f"  screenshot     FAIL (no PNG magic, got first 8: {png[:8]!r})")
                return 1
            # Parse IHDR (bytes 16..24): width, height as big-endian uint32
            w = int.from_bytes(png[16:20], "big")
            h = int.from_bytes(png[20:24], "big")
            print(f"  screenshot     OK ({len(png)} B, {w}x{h})")
            # Pause-determinism: pauseEngine may take a frame to fully settle,
            # so we wait, then compare two snapshots.
            d.pause()
            time.sleep(0.3)
            a = d.screenshot()
            b = d.screenshot()
            d.unpause()
            if a != b:
                print(f"  paused-deterministic FAIL ({len(a)} B vs {len(b)} B differ)")
                return 1
            print(f"  paused-deterministic OK (both {len(a)} B, identical)")
            # Liveness: with engine running, screenshots over time should differ.
            c = d.screenshot()
            time.sleep(0.5)
            e = d.screenshot()
            if c == e:
                print("  unpaused-changes WARN (two screenshots ~500ms apart were identical; engine may not be running)")
            else:
                print("  unpaused-changes OK (frames differ)")
        if "step" in names:
            # step(0) — no-op while paused.
            d.pause()
            time.sleep(0.3)
            n = d.step(0)
            if n != 0:
                print(f"  step(0)        FAIL (expected 0, got {n})")
                return 1
            print("  step(0)        OK")
            # step while unpaused must error.
            d.unpause()
            try:
                d.step(1)
                print("  step-needs-pause FAIL (expected error)")
                return 1
            except McpError as e:
                print(f"  step-needs-pause OK (rejected: {e})")
            # Step advances the game: snapshots before/after must differ.
            d.pause()
            time.sleep(0.3)
            a = d.screenshot()
            t0 = time.time()
            n = d.step(60)
            elapsed = time.time() - t0
            b = d.screenshot()
            if n != 60:
                print(f"  step(60)       FAIL (advanced {n})")
                return 1
            if a == b:
                print("  step-advances  FAIL (frames identical after step)")
                return 1
            timing = "OK" if 0.5 <= elapsed <= 5.0 else "WARN"
            print(f"  step(60)       {timing} ({elapsed*1000:.0f}ms, frames differ)")
            # Repeated stepping continues to make progress.
            f = d.screenshot()
            d.step(60)
            g = d.screenshot()
            if f == g:
                print("  step-repeat    FAIL (no progress on second step)")
                return 1
            print("  step-repeat    OK")
            d.unpause()
    print("Smoke test passed.")
    return 0


def cmd_screenshot(args: argparse.Namespace) -> int:
    with McpDriver(args.scummvm, args.target, forward_stderr=args.verbose) as d:
        if args.pause:
            d.pause()
        if args.delay > 0:
            time.sleep(args.delay)
        png = d.screenshot(args.out)
        if args.pause:
            d.unpause()
    print(f"Wrote {len(png)} bytes to {args.out}")
    return 0


def cmd_shell(args: argparse.Namespace) -> int:
    print("Interactive shell. Commands:")
    print("  list                       — show registered tools")
    print("  call <name> [json args]    — invoke a tool")
    print("  raw <json>                 — send a raw JSON-RPC request")
    print("  quit                       — exit")
    with McpDriver(args.scummvm, args.target, forward_stderr=args.verbose) as d:
        while True:
            try:
                line = input("mcp> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not line or line in ("quit", "exit"):
                break
            try:
                if line == "list":
                    for t in d.list_tools():
                        print(f"  {t['name']:24s} {t.get('description', '')}")
                elif line.startswith("call "):
                    parts = line[len("call "):].split(None, 1)
                    name = parts[0]
                    arg_dict = json.loads(parts[1]) if len(parts) > 1 else {}
                    result = d.call_tool(name, arg_dict)
                    print(json.dumps(result, indent=2))
                elif line.startswith("raw "):
                    payload = json.loads(line[len("raw "):])
                    if "id" not in payload:
                        result = d._send_notification(payload["method"], payload.get("params"))
                        print("(notification sent)")
                    else:
                        # bypass the per-call lock & response-shape parser
                        with d._lock:
                            assert d.proc.stdin and d.proc.stdout
                            d.proc.stdin.write((json.dumps(payload) + "\n").encode("utf-8"))
                            d.proc.stdin.flush()
                            print(d.proc.stdout.readline().decode("utf-8").strip())
                else:
                    print("(unknown command; try 'list', 'call <name>', or 'quit')")
            except McpError as e:
                print(f"error: {e}")
            except Exception as e:
                print(f"client error: {e}")
    return 0


def cmd_raw(args: argparse.Namespace) -> int:
    payload = json.loads(args.json)
    with McpDriver(args.scummvm, args.target, forward_stderr=args.verbose) as d:
        with d._lock:
            assert d.proc.stdin and d.proc.stdout
            d.proc.stdin.write((json.dumps(payload) + "\n").encode("utf-8"))
            d.proc.stdin.flush()
            print(d.proc.stdout.readline().decode("utf-8").strip())
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(prog="driver.py", description=__doc__.strip(), formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scummvm", default=DEFAULT_SCUMMVM, help=f"path to scummvm binary (default: {DEFAULT_SCUMMVM})")
    p.add_argument("--target", default="Shivers", help="game target id (default: Shivers)")
    p.add_argument("-v", "--verbose", action="store_true", help="forward scummvm stderr")

    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("smoke", help="run the basic protocol smoke test").set_defaults(func=cmd_smoke)
    sub.add_parser("shell", help="interactive REPL against the server").set_defaults(func=cmd_shell)
    raw = sub.add_parser("raw", help="send a single raw JSON-RPC request and exit")
    raw.add_argument("json", help="JSON-RPC request as a string")
    raw.set_defaults(func=cmd_raw)
    shot = sub.add_parser("screenshot", help="take a screenshot and save it to a file")
    shot.add_argument("--out", default="/tmp/scummvm-mcp-shot.png", help="output PNG path (default: /tmp/scummvm-mcp-shot.png)")
    shot.add_argument("--delay", type=float, default=0.0, help="seconds to wait after spawn before capturing")
    shot.add_argument("--pause", action="store_true", help="pause the engine before capturing")
    shot.set_defaults(func=cmd_screenshot)

    ns = p.parse_args(argv)
    return ns.func(ns)


if __name__ == "__main__":
    sys.exit(main())

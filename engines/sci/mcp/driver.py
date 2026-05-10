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
import atexit
import base64
import json
import os
import readline
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

HISTORY_FILE = os.path.expanduser("~/.scummvm-mcp-history")


def _coerce_value(v: str) -> Any:
    """Coerce a CLI-style value to int/float/bool/str for tool arguments."""
    if (v.startswith('"') and v.endswith('"')) or (v.startswith("'") and v.endswith("'")):
        return v[1:-1]
    if v in ("true", "True"):
        return True
    if v in ("false", "False"):
        return False
    try:
        return int(v)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        pass
    return v


def _parse_kwargs(parts: list[str]) -> dict:
    """Parse a list of "key=value" tokens into a dict, with type inference."""
    result: dict[str, Any] = {}
    for part in parts:
        if "=" not in part:
            raise ValueError(f"expected key=value, got {part!r}")
        k, v = part.split("=", 1)
        result[k] = _coerce_value(v)
    return result


def _diff_bbox(png_a: bytes, png_b: bytes) -> Optional[tuple]:
    """Bounding box of differing pixels between two PNGs, or None if PIL is unavailable.

    Handles paletted PNGs (Shivers's screenshots) by diffing palette indices via
    L-mode conversion of the diff image; ImageChops.difference on a 'P' image
    yields a 'P' diff that getbbox can't read directly.
    """
    try:
        from PIL import Image, ImageChops
        from io import BytesIO
    except ImportError:
        return None
    a = Image.open(BytesIO(png_a))
    b = Image.open(BytesIO(png_b))
    if a.size != b.size:
        return None
    if a.mode != b.mode:
        a = a.convert("RGBA")
        b = b.convert("RGBA")
    diff = ImageChops.difference(a, b)
    if diff.mode == "P":
        diff = diff.convert("L")
    return diff.getbbox()


def _format_result(result: dict) -> str:
    """Pretty-print a tool result, summarizing huge image content instead of dumping base64."""
    if "content" in result:
        lines = []
        for item in result.get("content", []):
            t = item.get("type")
            if t == "image":
                lines.append(f"  [image {item.get('mimeType', '?')}, {len(item.get('data', ''))} bytes base64]")
            elif t == "text":
                lines.append(f"  [text] {item.get('text', '')}")
            else:
                lines.append(f"  [{t}] {json.dumps({k: v for k, v in item.items() if k != 'type'})}")
        if result.get("isError"):
            lines.append("  isError: true")
        return "\n".join(lines) if lines else "  (empty result)"
    return json.dumps(result, indent=2)


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

    def click(self, x: int, y: int, button: str = "left") -> str:
        result = self.call_tool("click", {"x": x, "y": y, "button": button})
        if result.get("isError"):
            raise McpError(-32000, f"click: {self._content_text(result)}")
        return self._content_text(result)

    def move_cursor(self, x: int, y: int) -> str:
        result = self.call_tool("move_cursor", {"x": x, "y": y})
        if result.get("isError"):
            raise McpError(-32000, f"move_cursor: {self._content_text(result)}")
        return self._content_text(result)

    def snapshot(self, name: str) -> dict:
        result = self.call_tool("snapshot", {"name": name})
        if result.get("isError"):
            raise McpError(-32000, f"snapshot: {self._content_text(result)}")
        return json.loads(self._content_text(result))

    def restore_snapshot(self, name: str) -> str:
        result = self.call_tool("restore_snapshot", {"name": name})
        if result.get("isError"):
            raise McpError(-32000, f"restore_snapshot: {self._content_text(result)}")
        return self._content_text(result)

    def list_snapshots(self) -> list[dict]:
        result = self.call_tool("list_snapshots")
        if result.get("isError"):
            raise McpError(-32000, f"list_snapshots: {self._content_text(result)}")
        try:
            return json.loads(self._content_text(result))
        except (ValueError, AttributeError):
            return []

    def drop_snapshot(self, name: str) -> str:
        result = self.call_tool("drop_snapshot", {"name": name})
        if result.get("isError"):
            raise McpError(-32000, f"drop_snapshot: {self._content_text(result)}")
        return self._content_text(result)

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
        if "move_cursor" in names:
            # NB: lockScreen captures the OSystem game surface; the SDL cursor is
            # composited separately and thus doesn't show in our screenshots.
            # We can't visually verify the cursor moved, so this just exercises
            # the round-trip and a few different button combos. Manual REPL is
            # the only way to confirm the cursor actually warps.
            d.pause()
            time.sleep(0.3)
            d.move_cursor(50, 50)
            d.move_cursor(320, 240)
            d.move_cursor(600, 430)
            d.step(5)
            print("  move_cursor    OK (3 moves dispatched)")
            d.unpause()
        if "click" in names:
            d.pause()
            time.sleep(0.3)
            d.click(5, 5)
            d.click(5, 5, "right")
            d.click(5, 5, "middle")
            d.step(5)
            print("  click          OK (left/right/middle dispatched)")
            try:
                d.click(5, 5, "scroll")
                print("  click bad-button FAIL (expected error for unknown button)")
                return 1
            except McpError as e:
                print(f"  click bad-button OK (rejected: {e})")
            d.unpause()
        if "snapshot" in names and "restore_snapshot" in names:
            d.pause()
            time.sleep(0.3)
            # Snapshot, advance, restore, advance same amount, screenshots should match.
            info = d.snapshot("smoke-a")
            print(f"  snapshot       OK (name=smoke-a, {info['bytes']} B)")
            # snapshot requires pause: try while unpaused
            d.unpause()
            try:
                d.snapshot("smoke-b")
                print("  snapshot-needs-pause FAIL (expected error)")
                return 1
            except McpError as e:
                print(f"  snapshot-needs-pause OK (rejected: {e})")
            d.pause()
            time.sleep(0.3)
            # list_snapshots should include smoke-a.
            snaps = d.list_snapshots()
            if not any(s["name"] == "smoke-a" for s in snaps):
                print(f"  list_snapshots FAIL (smoke-a not listed: {snaps})")
                return 1
            print(f"  list_snapshots OK ({len(snaps)} entries)")
            # advance, restore, observe
            d.step(60)
            after = d.screenshot()
            d.restore_snapshot("smoke-a")
            d.step(10)  # let SCI VM finish processing the load
            restored = d.screenshot()
            # After restore + 10 frames, state should be near where snapshot was
            # taken (post-snapshot animation). It won't be byte-identical to the
            # pre-step snapshot screenshot, but it should *differ* from `after`
            # — restore did something. Liberal check.
            if restored == after:
                print("  restore        WARN (post-restore screen identical to pre-restore; restore may not have taken effect)")
            else:
                print("  restore        OK (post-restore screen differs from pre-restore)")
            # drop_snapshot
            d.drop_snapshot("smoke-a")
            snaps = d.list_snapshots()
            if any(s["name"] == "smoke-a" for s in snaps):
                print(f"  drop_snapshot  FAIL (still listed: {snaps})")
                return 1
            print(f"  drop_snapshot  OK ({len(snaps)} remain)")
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
    print("Interactive shell. Anything not recognized below is treated as a tool name.")
    print("  list                              — show registered tools")
    print("  snapshot [name]                   — pause+snapshot+unpause; auto-names if no arg")
    print("  raw <json>                        — send a raw JSON-RPC request")
    print("  help                              — show this help")
    print("  quit                              — exit")
    print("  <tool> [key=value ...]            — invoke an MCP tool (e.g. `step frames=60`)")
    print("(↑/↓ navigate history; history persists at ~/.scummvm-mcp-history)")

    # Wire up readline history. Importing readline already enables line editing
    # and arrow-key history within input(); we just persist it across sessions.
    try:
        readline.read_history_file(HISTORY_FILE)
    except (FileNotFoundError, OSError):
        pass
    readline.set_history_length(1000)
    atexit.register(lambda: _safe_write_history())

    snap_counter = 0
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
                tokens = shlex.split(line)
            except ValueError as e:
                print(f"parse error: {e}")
                continue
            if not tokens:
                continue
            # Allow `call <tool>` for muscle memory but it's no longer required.
            if tokens[0] == "call" and len(tokens) >= 2:
                tokens = tokens[1:]
            cmd = tokens[0]
            try:
                if cmd in ("help", "?"):
                    print("  list                              — show registered tools")
                    print("  snapshot [name]                   — pause+snapshot+unpause (auto-names if no arg)")
                    print("  raw <json>                        — send a raw JSON-RPC request")
                    print("  help                              — show this help")
                    print("  quit                              — exit")
                    print("  <tool> [key=value ...]            — invoke an MCP tool (e.g. `step frames=60`)")
                elif cmd == "list":
                    for t in d.list_tools():
                        print(f"  {t['name']:24s} {t.get('description', '')}")
                elif cmd == "snapshot":
                    if len(tokens) >= 2 and "=" not in tokens[1]:
                        name = tokens[1]
                    else:
                        snap_counter += 1
                        name = f"snap{snap_counter}"
                    d.pause()
                    info = d.snapshot(name)
                    d.unpause()
                    print(f"snapshot {name} OK ({info.get('bytes', '?')} bytes)")
                elif cmd == "raw":
                    payload = json.loads(line[len("raw "):])
                    if "id" not in payload:
                        d._send_notification(payload["method"], payload.get("params"))
                        print("(notification sent)")
                    else:
                        with d._lock:
                            assert d.proc.stdin and d.proc.stdout
                            d.proc.stdin.write((json.dumps(payload) + "\n").encode("utf-8"))
                            d.proc.stdin.flush()
                            print(d.proc.stdout.readline().decode("utf-8").strip())
                else:
                    # Treat the first token as a tool name; remaining are key=value args.
                    try:
                        kw = _parse_kwargs(tokens[1:])
                    except ValueError as e:
                        print(f"args error: {e}")
                        continue
                    result = d.call_tool(cmd, kw)
                    print(_format_result(result))
            except McpError as e:
                print(f"error: {e}")
            except Exception as e:
                print(f"client error: {e}")
    return 0


def _safe_write_history() -> None:
    try:
        readline.write_history_file(HISTORY_FILE)
    except OSError:
        pass


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

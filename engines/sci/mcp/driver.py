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

    # ── High-level wrappers (added as the engine side grows) ───────────────

    def pause(self) -> None:
        self.call_tool("pause")

    def unpause(self) -> None:
        self.call_tool("unpause")

    def step(self, frames: int = 1) -> int:
        result = self.call_tool("step", {"frames": frames})
        return int(result.get("frames_advanced", 0))

    def screenshot(self, save_path: Optional[str] = None) -> bytes:
        result = self.call_tool("screenshot")
        png = base64.b64decode(result["png_base64"])
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
        print(f"  tools/list     {len(tools)} tools: {[t['name'] for t in tools]}")
        # Sanity: calling a nonexistent tool should yield an MCP error.
        try:
            d.call_tool("definitely_not_a_real_tool")
            print("  unknown tool   FAIL (expected error, got success)")
            return 1
        except McpError as e:
            print(f"  unknown tool   OK (rejected: {e})")
    print("Smoke test passed.")
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

    ns = p.parse_args(argv)
    return ns.func(ns)


if __name__ == "__main__":
    sys.exit(main())

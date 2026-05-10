/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// We use POSIX read/write/dup/close/pthread directly to bridge stdio to MCP;
// this isolated translation unit opts out of common/forbidden.h's bans.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "sci/mcp/server.h"

#include "common/base64.h"
#include "common/events.h"
#include "common/formats/json.h"
#include "common/memstream.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"
#include "image/png.h"
#include "sci/sci.h"

#ifdef POSIX
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

extern int g_mcpRealStdoutFd; // global, defined in base/main.cpp
#endif

namespace Sci {

#ifdef POSIX

static const char *kProtocolVersion = "2024-11-05";
static const char *kServerName = "scummvm-shivers-mcp";
static const char *kServerVersion = "0.1";

McpServer::McpServer(SciEngine *engine) :
	_engine(engine), _realStdoutFd(-1), _running(false),
	_threadHandle(nullptr), _stepMutex(nullptr), _stepCond(nullptr),
	_stepFramesRemaining(0) {}

McpServer::~McpServer() {
	stop();
}

void *McpServer::threadEntry(void *opaque) {
	static_cast<McpServer *>(opaque)->readerLoop();
	return nullptr;
}

void McpServer::start() {
	// scummvm_main saves the real stdout fd and redirects stdout to stderr
	// when --mcp is parsed, before any startup printing — so by the time we
	// get here, fd 1 already points at stderr.
	_realStdoutFd = ::g_mcpRealStdoutFd;
	if (_realStdoutFd < 0) {
		warning("McpServer: real stdout fd not captured; protocol output disabled");
		return;
	}

	pthread_mutex_t *mutex = new pthread_mutex_t;
	pthread_cond_t *cond = new pthread_cond_t;
	pthread_mutex_init(mutex, nullptr);
	pthread_cond_init(cond, nullptr);
	_stepMutex = mutex;
	_stepCond = cond;

	_running = true;
	pthread_t *thread = new pthread_t;
	if (pthread_create(thread, nullptr, &McpServer::threadEntry, this) != 0) {
		warning("McpServer: pthread_create failed; MCP disabled");
		delete thread;
		_running = false;
		return;
	}
	_threadHandle = thread;
}

void McpServer::stop() {
	if (!_running)
		return;
	_running = false;
	if (_threadHandle) {
		pthread_t *thread = static_cast<pthread_t *>(_threadHandle);
		// Close stdin so the reader's blocked read returns immediately.
		close(STDIN_FILENO);
		pthread_join(*thread, nullptr);
		delete thread;
		_threadHandle = nullptr;
	}
	if (_stepMutex) {
		pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
		pthread_mutex_destroy(mutex);
		delete mutex;
		_stepMutex = nullptr;
	}
	if (_stepCond) {
		pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
		pthread_cond_destroy(cond);
		delete cond;
		_stepCond = nullptr;
	}
	// _realStdoutFd is owned by scummvm_main (g_mcpRealStdoutFd); don't close.
	_realStdoutFd = -1;
}

// Send buffered inputs to scummvm in order. For move events we use warpMouse,
// which moves the OS cursor and synthesizes its own MOUSEMOVE event into the
// queue; for button events we pushEvent directly. Caller must hold _stepMutex.
void McpServer::flushPendingInputs() {
	Common::EventManager *em = g_system->getEventManager();
	for (uint i = 0; i < _pendingInputs.size(); ++i) {
		const Common::Event &ev = _pendingInputs[i];
		if (ev.type == Common::EVENT_MOUSEMOVE) {
			g_system->warpMouse(ev.mouse.x, ev.mouse.y);
		} else {
			em->pushEvent(ev);
		}
	}
	_pendingInputs.clear();
}

void McpServer::onFrame() {
	if (!_stepMutex || !_stepCond)
		return;
	pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
	pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
	pthread_mutex_lock(mutex);
	if (_stepFramesRemaining > 0) {
		if (--_stepFramesRemaining == 0)
			pthread_cond_signal(cond);
	}
	pthread_mutex_unlock(mutex);
}

void McpServer::readerLoop() {
	Common::String buffer;
	char chunk[4096];
	while (_running) {
		ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
		if (n <= 0) {
			// EOF or error — graceful shutdown.
			break;
		}
		buffer += Common::String(chunk, (uint32)n);
		// Drain any complete line-delimited messages.
		uint32 nl;
		while ((nl = buffer.find('\n')) != Common::String::npos) {
			Common::String line(buffer.c_str(), nl);
			buffer = Common::String(buffer.c_str() + nl + 1);
			if (!line.empty())
				handleRequest(line);
		}
	}
}

// Build {"jsonrpc": "2.0", "id": <id>, "result": <result>} as a JSON string.
static Common::String buildOk(const Common::JSONValue *id, const Common::JSONValue *result) {
	Common::JSONObject obj;
	obj["jsonrpc"] = new Common::JSONValue("2.0");
	if (id)
		obj["id"] = new Common::JSONValue(*id);
	else
		obj["id"] = new Common::JSONValue();
	obj["result"] = result ? new Common::JSONValue(*result) : new Common::JSONValue(Common::JSONObject());
	Common::JSONValue wrapper(obj);
	return wrapper.stringify();
}

// Build a no-input tool definition for tools/list.
static Common::JSONValue *makeNoArgToolDef(const char *name, const char *desc) {
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(Common::JSONObject());
	schema["required"] = new Common::JSONValue(Common::JSONArray());
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String(name));
	tool["description"] = new Common::JSONValue(Common::String(desc));
	tool["inputSchema"] = new Common::JSONValue(schema);
	return new Common::JSONValue(tool);
}

// Build the `click` tool definition.
static Common::JSONValue *makeClickToolDef() {
	Common::JSONObject xProp, yProp, buttonProp;
	xProp["type"] = new Common::JSONValue(Common::String("integer"));
	xProp["description"] = new Common::JSONValue(Common::String("Buffer-coord X (0-639 in Shivers)."));
	yProp["type"] = new Common::JSONValue(Common::String("integer"));
	yProp["description"] = new Common::JSONValue(Common::String("Buffer-coord Y (0-479 in Shivers)."));
	buttonProp["type"] = new Common::JSONValue(Common::String("string"));
	Common::JSONArray buttonEnum;
	buttonEnum.push_back(new Common::JSONValue(Common::String("left")));
	buttonEnum.push_back(new Common::JSONValue(Common::String("right")));
	buttonEnum.push_back(new Common::JSONValue(Common::String("middle")));
	buttonProp["enum"] = new Common::JSONValue(buttonEnum);
	buttonProp["default"] = new Common::JSONValue(Common::String("left"));
	Common::JSONObject properties;
	properties["x"] = new Common::JSONValue(xProp);
	properties["y"] = new Common::JSONValue(yProp);
	properties["button"] = new Common::JSONValue(buttonProp);
	Common::JSONArray required;
	required.push_back(new Common::JSONValue(Common::String("x")));
	required.push_back(new Common::JSONValue(Common::String("y")));
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(properties);
	schema["required"] = new Common::JSONValue(required);
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String("click"));
	tool["description"] = new Common::JSONValue(Common::String(
		"Click at (x,y) with the given button (left/right/middle, default left). "
		"Press and release fire on the same frame. Buffer coords (1:1 with screenshot)."));
	tool["inputSchema"] = new Common::JSONValue(schema);
	return new Common::JSONValue(tool);
}

// Build the `move_cursor` tool definition.
static Common::JSONValue *makeMoveCursorToolDef() {
	Common::JSONObject xProp, yProp;
	xProp["type"] = new Common::JSONValue(Common::String("integer"));
	yProp["type"] = new Common::JSONValue(Common::String("integer"));
	Common::JSONObject properties;
	properties["x"] = new Common::JSONValue(xProp);
	properties["y"] = new Common::JSONValue(yProp);
	Common::JSONArray required;
	required.push_back(new Common::JSONValue(Common::String("x")));
	required.push_back(new Common::JSONValue(Common::String("y")));
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(properties);
	schema["required"] = new Common::JSONValue(required);
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String("move_cursor"));
	tool["description"] = new Common::JSONValue(Common::String(
		"Move the cursor to (x,y) without clicking. Buffer coords."));
	tool["inputSchema"] = new Common::JSONValue(schema);
	return new Common::JSONValue(tool);
}

// Build the `step` tool definition with a single integer "frames" property.
static Common::JSONValue *makeStepToolDef() {
	Common::JSONObject framesProp;
	framesProp["type"] = new Common::JSONValue(Common::String("integer"));
	framesProp["minimum"] = new Common::JSONValue((long long int)0);
	framesProp["description"] = new Common::JSONValue(Common::String("Number of frames to advance (default 1; 0 is a no-op)."));
	Common::JSONObject properties;
	properties["frames"] = new Common::JSONValue(framesProp);
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(properties);
	schema["required"] = new Common::JSONValue(Common::JSONArray());
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String("step"));
	tool["description"] = new Common::JSONValue(Common::String(
		"Advance the engine N frames synchronously. Engine must be paused; "
		"errors otherwise. Result is JSON-encoded text {\"frames_advanced\": N}."));
	tool["inputSchema"] = new Common::JSONValue(schema);
	return new Common::JSONValue(tool);
}

// Build an MCP image content result:
//   {"content": [{"type": "image", "mimeType": "image/png", "data": <base64>}], "isError": false}
static Common::JSONValue *makeImageResult(const Common::String &base64Png) {
	Common::JSONObject contentItem;
	contentItem["type"] = new Common::JSONValue(Common::String("image"));
	contentItem["mimeType"] = new Common::JSONValue(Common::String("image/png"));
	contentItem["data"] = new Common::JSONValue(base64Png);
	Common::JSONArray content;
	content.push_back(new Common::JSONValue(contentItem));
	Common::JSONObject result;
	result["content"] = new Common::JSONValue(content);
	result["isError"] = new Common::JSONValue(false);
	return new Common::JSONValue(result);
}

// Capture the current OSystem screen, encode as PNG, and base64. Returns an
// empty string on failure. Caller must own the strings going in/out.
static Common::String captureScreenshotBase64() {
	Graphics::Surface *screen = g_system->lockScreen();
	if (!screen) {
		warning("McpServer: lockScreen returned null");
		return Common::String();
	}
	Common::MemoryWriteStreamDynamic out(DisposeAfterUse::NO);
	bool ok;
	if (g_system->getScreenFormat().isCLUT8()) {
		byte palette[256 * 3];
		g_system->getPaletteManager()->grabPalette(palette, 0, 256);
		ok = Image::writePNG(out, *screen, palette, 256);
	} else {
		ok = Image::writePNG(out, *screen);
	}
	g_system->unlockScreen();
	if (!ok) {
		warning("McpServer: writePNG failed");
		free(out.getData());
		return Common::String();
	}
	Common::String b64 = Common::b64EncodeData(out.getData(), out.size());
	free(out.getData());
	return b64;
}

// Build an MCP-compliant tools/call result with a single text content item:
//   {"content": [{"type": "text", "text": <text>}], "isError": <isError>}
static Common::JSONValue *makeTextResult(const Common::String &text, bool isError = false) {
	Common::JSONObject contentItem;
	contentItem["type"] = new Common::JSONValue(Common::String("text"));
	contentItem["text"] = new Common::JSONValue(text);
	Common::JSONArray content;
	content.push_back(new Common::JSONValue(contentItem));
	Common::JSONObject result;
	result["content"] = new Common::JSONValue(content);
	result["isError"] = new Common::JSONValue(isError);
	return new Common::JSONValue(result);
}

// Build {"jsonrpc": "2.0", "id": <id>, "error": {"code": <code>, "message": <msg>}}.
static Common::String buildError(const Common::JSONValue *id, int code, const Common::String &message) {
	Common::JSONObject err;
	err["code"] = new Common::JSONValue((long long int)code);
	err["message"] = new Common::JSONValue(message);
	Common::JSONObject obj;
	obj["jsonrpc"] = new Common::JSONValue("2.0");
	if (id)
		obj["id"] = new Common::JSONValue(*id);
	else
		obj["id"] = new Common::JSONValue();
	obj["error"] = new Common::JSONValue(err);
	Common::JSONValue wrapper(obj);
	return wrapper.stringify();
}

void McpServer::handleRequest(const Common::String &line) {
	Common::JSONValue *parsed = Common::JSON::parse(line.c_str());
	if (!parsed || !parsed->isObject()) {
		delete parsed;
		sendResponse(buildError(nullptr, -32700, "Parse error"));
		return;
	}
	const Common::JSONObject &req = parsed->asObject();

	const Common::JSONValue *idVal = nullptr;
	if (req.contains("id"))
		idVal = req["id"];

	if (!req.contains("method") || !req["method"]->isString()) {
		sendResponse(buildError(idVal, -32600, "Invalid Request: missing method"));
		delete parsed;
		return;
	}
	const Common::String &method = req["method"]->asString();

	if (method == "initialize") {
		Common::JSONObject capabilities;
		capabilities["tools"] = new Common::JSONValue(Common::JSONObject());
		Common::JSONObject serverInfo;
		serverInfo["name"] = new Common::JSONValue(Common::String(kServerName));
		serverInfo["version"] = new Common::JSONValue(Common::String(kServerVersion));
		Common::JSONObject result;
		result["protocolVersion"] = new Common::JSONValue(Common::String(kProtocolVersion));
		result["capabilities"] = new Common::JSONValue(capabilities);
		result["serverInfo"] = new Common::JSONValue(serverInfo);
		Common::JSONValue resultVal(result);
		sendResponse(buildOk(idVal, &resultVal));
	} else if (method == "notifications/initialized") {
		// Notification — no response.
	} else if (method == "tools/list") {
		Common::JSONArray tools;
		tools.push_back(makeNoArgToolDef("pause",
			"Pause the SCI engine. Idempotent; subsequent step calls advance "
			"the game while paused. Has no effect if already paused via the "
			"in-game menu."));
		tools.push_back(makeNoArgToolDef("unpause",
			"Resume the SCI engine at native speed (returning interactive "
			"control to the human player). Step calls error while unpaused."));
		tools.push_back(makeNoArgToolDef("screenshot",
			"Capture the current OSystem screen as a PNG and return it as "
			"base64 in MCP image content. Reflects exactly what the user "
			"sees, including cursor and any debug overlays."));
		tools.push_back(makeStepToolDef());
		tools.push_back(makeClickToolDef());
		tools.push_back(makeMoveCursorToolDef());
		Common::JSONObject result;
		result["tools"] = new Common::JSONValue(tools);
		Common::JSONValue resultVal(result);
		sendResponse(buildOk(idVal, &resultVal));
	} else if (method == "tools/call") {
		// Parse tools/call params: {"name": "<tool>", "arguments": {...}}
		const Common::JSONObject *params = (req.contains("params") && req["params"]->isObject()) ? &req["params"]->asObject() : nullptr;
		Common::String toolName;
		if (params && params->contains("name") && (*params)["name"]->isString())
			toolName = (*params)["name"]->asString();

		// NOTE: Engine::pauseEngine is a counter — calling pause() N times
		// requires N matching unpause()s to fully resume. The in-game menu
		// (and saving/loading) increment this counter independently. So
		// after the user opens and closes the in-game menu, our `unpause`
		// tool will only decrement by one and the engine stays paused. Live
		// with it for now; the proper fix is to track our own MCP-pause
		// state and reconcile against pauseLevel before each transition.
		// TODO(mcp): reconcile MCP pause state with engine pauseLevel.
		// NOTE: We're calling pauseEngine from the MCP reader thread, not
		// the engine's main thread. ScummVM doesn't formally guarantee this
		// is safe, but the pauseLevel int is updated atomically enough in
		// practice and engine subsystems poll it from their own threads.
		if (toolName == "pause") {
			if (!_pauseToken.isActive())
				_pauseToken = _engine->pauseEngine();
			Common::JSONValue *result = makeTextResult("paused");
			sendResponse(buildOk(idVal, result));
			delete result;
		} else if (toolName == "unpause") {
			pthread_mutex_t *flushMutex = static_cast<pthread_mutex_t *>(_stepMutex);
			if (flushMutex) pthread_mutex_lock(flushMutex);
			flushPendingInputs();
			if (flushMutex) pthread_mutex_unlock(flushMutex);
			if (_pauseToken.isActive())
				_pauseToken.clear();
			Common::JSONValue *result = makeTextResult("unpaused");
			sendResponse(buildOk(idVal, result));
			delete result;
		} else if (toolName == "screenshot") {
			Common::String b64 = captureScreenshotBase64();
			if (b64.empty()) {
				Common::JSONValue *result = makeTextResult("screenshot capture failed", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else {
				Common::JSONValue *result = makeImageResult(b64);
				sendResponse(buildOk(idVal, result));
				delete result;
			}
		} else if (toolName == "click" || toolName == "move_cursor") {
			int x = 0, y = 0;
			Common::String button = "left";
			bool haveX = false, haveY = false;
			if (params && params->contains("arguments") && (*params)["arguments"]->isObject()) {
				const Common::JSONObject &a = (*params)["arguments"]->asObject();
				if (a.contains("x")) {
					if (a["x"]->isIntegerNumber()) { x = (int)a["x"]->asIntegerNumber(); haveX = true; }
					else if (a["x"]->isNumber()) { x = (int)a["x"]->asNumber(); haveX = true; }
				}
				if (a.contains("y")) {
					if (a["y"]->isIntegerNumber()) { y = (int)a["y"]->asIntegerNumber(); haveY = true; }
					else if (a["y"]->isNumber()) { y = (int)a["y"]->asNumber(); haveY = true; }
				}
				if (a.contains("button") && a["button"]->isString())
					button = a["button"]->asString();
			}
			if (!haveX || !haveY) {
				Common::JSONValue *result = makeTextResult("x and y are required", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else {
				// Build the events we want to deliver. For click, that's a move
				// followed by button-down and button-up at the same position.
				Common::Event mv;
				mv.type = Common::EVENT_MOUSEMOVE;
				mv.mouse = Common::Point(x, y);
				Common::Array<Common::Event> events;
				events.push_back(mv);
				if (toolName == "click") {
					Common::EventType down, up;
					if (button == "left") { down = Common::EVENT_LBUTTONDOWN; up = Common::EVENT_LBUTTONUP; }
					else if (button == "right") { down = Common::EVENT_RBUTTONDOWN; up = Common::EVENT_RBUTTONUP; }
					else if (button == "middle") { down = Common::EVENT_MBUTTONDOWN; up = Common::EVENT_MBUTTONUP; }
					else {
						Common::JSONValue *result = makeTextResult(Common::String::format("unknown button: %s", button.c_str()), true);
						sendResponse(buildOk(idVal, result));
						delete result;
						delete parsed;
						return;
					}
					Common::Event d;
					d.type = down;
					d.mouse = Common::Point(x, y);
					events.push_back(d);
					Common::Event u;
					u.type = up;
					u.mouse = Common::Point(x, y);
					events.push_back(u);
				}
				// While MCP-paused, buffer rather than fire — the engine's script
				// VM still drains live events even with pauseEngine in effect, so
				// we'd advance state immediately. Buffered events are flushed on
				// step (right before resuming) and on unpause.
				pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
				if (_pauseToken.isActive()) {
					if (mutex) pthread_mutex_lock(mutex);
					for (uint i = 0; i < events.size(); ++i)
						_pendingInputs.push_back(events[i]);
					if (mutex) pthread_mutex_unlock(mutex);
				} else {
					Common::EventManager *em = g_system->getEventManager();
					for (uint i = 0; i < events.size(); ++i) {
						const Common::Event &ev = events[i];
						if (ev.type == Common::EVENT_MOUSEMOVE)
							g_system->warpMouse(ev.mouse.x, ev.mouse.y);
						else
							em->pushEvent(ev);
					}
				}
				Common::String msg = (toolName == "click")
					? Common::String::format("clicked %s at (%d,%d)%s", button.c_str(), x, y, _pauseToken.isActive() ? " (queued)" : "")
					: Common::String::format("moved to (%d,%d)%s", x, y, _pauseToken.isActive() ? " (queued)" : "");
				Common::JSONValue *result = makeTextResult(msg);
				sendResponse(buildOk(idVal, result));
				delete result;
			}
		} else if (toolName == "step") {
			int frames = 1;
			if (params && params->contains("arguments") && (*params)["arguments"]->isObject()) {
				const Common::JSONObject &toolArgs = (*params)["arguments"]->asObject();
				if (toolArgs.contains("frames")) {
					if (toolArgs["frames"]->isIntegerNumber())
						frames = (int)toolArgs["frames"]->asIntegerNumber();
					else if (toolArgs["frames"]->isNumber())
						frames = (int)toolArgs["frames"]->asNumber();
				}
			}
			if (frames < 0) {
				Common::JSONValue *result = makeTextResult("frames must be >= 0", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else if (frames == 0) {
				Common::JSONValue *result = makeTextResult("{\"frames_advanced\": 0}");
				sendResponse(buildOk(idVal, result));
				delete result;
			} else if (!_pauseToken.isActive()) {
				Common::JSONValue *result = makeTextResult("step requires pause", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else if (!_stepMutex || !_stepCond) {
				Common::JSONValue *result = makeTextResult("step synchronization not initialized", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else {
				pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
				pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
				pthread_mutex_lock(mutex);
				flushPendingInputs();          // deliver buffered click/move events first
				_stepFramesRemaining = frames;
				_pauseToken.clear();          // engine resumes
				while (_stepFramesRemaining > 0) {
					pthread_cond_wait(cond, mutex);
				}
				_pauseToken = _engine->pauseEngine(); // re-pause while we still hold the mutex
				pthread_mutex_unlock(mutex);
				Common::JSONValue *result = makeTextResult(Common::String::format("{\"frames_advanced\": %d}", frames));
				sendResponse(buildOk(idVal, result));
				delete result;
			}
		} else {
			sendResponse(buildError(idVal, -32601, Common::String::format("Unknown tool: %s", toolName.c_str())));
		}
	} else if (method == "shutdown") {
		_running = false;
		sendResponse(buildOk(idVal, nullptr));
	} else {
		sendResponse(buildError(idVal, -32601, Common::String::format("Method not found: %s", method.c_str())));
	}

	delete parsed;
}

void McpServer::sendResponse(const Common::String &json) {
	if (_realStdoutFd < 0)
		return;
	Common::String framed = json + "\n";
	(void)write(_realStdoutFd, framed.c_str(), framed.size());
}

#else // !POSIX

McpServer::McpServer(SciEngine *engine) :
	_engine(engine), _realStdoutFd(-1), _running(false),
	_threadHandle(nullptr), _stepMutex(nullptr), _stepCond(nullptr),
	_stepFramesRemaining(0) {}

McpServer::~McpServer() {}

void McpServer::start() {
	warning("McpServer: --mcp is only supported on POSIX platforms");
}

void McpServer::stop() {}
void McpServer::onFrame() {}
void McpServer::readerLoop() {}
void McpServer::handleRequest(const Common::String &) {}
void McpServer::sendResponse(const Common::String &) {}
void *McpServer::threadEntry(void *) { return nullptr; }

#endif // POSIX

} // End of namespace Sci

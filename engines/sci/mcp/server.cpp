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
#include "common/serializer.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"
#include "image/png.h"
#include "sci/engine/savegame.h"
#include "sci/engine/state.h"
#include "sci/graphics/frameout.h"
#include "sci/sci.h"

#ifdef POSIX
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

extern int g_mcpRealStdoutFd; // global, defined in base/main.cpp
#endif

namespace Sci {

#ifdef POSIX

static const char *kProtocolVersion = "2024-11-05";
static const char *kServerName = "scummvm-shivers-mcp";
static const char *kServerVersion = "0.1";

// EventObserver that forwards every dispatched event to the McpServer for
// recording. Always returns false (doesn't consume) so the event still flows
// to the game and to other observers.
class McpEventObserver : public Common::EventObserver {
public:
	explicit McpEventObserver(McpServer *srv) : _srv(srv) {}
	bool notifyEvent(const Common::Event &event) override {
		_srv->onObservedEvent(event);
		return false;
	}
private:
	McpServer *_srv;
};

McpServer::McpServer(SciEngine *engine) :
	_engine(engine), _realStdoutFd(-1), _running(false),
	_threadHandle(nullptr), _stepMutex(nullptr), _stepCond(nullptr),
	_stepFramesRemaining(0),
	_playbackActive(false), _playbackPrevPaused(false),
	_playbackFrame(0), _playbackEndFrame(0), _playbackIndex(0),
	_recordingActive(false), _recordingFrame(0), _observerHandle(nullptr) {}

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

	// Install the event observer for recording. Priority 10 puts us below the
	// keymapper (999) but above the default engine-event-handler priority (0),
	// so we see every event the keymapper hasn't already consumed. Returning
	// false from notifyEvent keeps the event flowing to the game.
	McpEventObserver *obs = new McpEventObserver(this);
	g_system->getEventManager()->getEventDispatcher()->registerObserver(obs, 10, false);
	_observerHandle = obs;
}

void McpServer::stop() {
	if (!_running)
		return;
	_running = false;
	if (_observerHandle) {
		McpEventObserver *obs = static_cast<McpEventObserver *>(_observerHandle);
		g_system->getEventManager()->getEventDispatcher()->unregisterObserver(obs);
		delete obs;
		_observerHandle = nullptr;
	}
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

// Called from the engine thread by McpEventObserver for every dispatched event
// (mouse, keyboard, custom action, …). Records mouse-up clicks while recording
// is active. Note that events injected by our own playback also flow through
// here — that's expected and means the recorded script reflects whatever the
// engine actually saw.
void McpServer::onObservedEvent(const Common::Event &event) {
	int button = 0;
	if (event.type == Common::EVENT_LBUTTONUP) button = 1;
	else if (event.type == Common::EVENT_RBUTTONUP) button = 2;
	else if (event.type == Common::EVENT_MBUTTONUP) button = 3;
	else return;

	if (!_stepMutex)
		return;
	pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
	pthread_mutex_lock(mutex);
	if (_recordingActive) {
		RecordedClick c;
		c.frame = _recordingFrame;
		c.x = event.mouse.x;
		c.y = event.mouse.y;
		c.button = button;
		_recordedClicks.push_back(c);
	}
	pthread_mutex_unlock(mutex);
}

void McpServer::onFrame() {
	if (!_stepMutex || !_stepCond)
		return;
	pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
	pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
	pthread_mutex_lock(mutex);
	// Drain any pending snapshot restore on this thread first; we're at a
	// kernel-call boundary (kFrameOut), which is the SCI-safe place to do it.
	if (!_pendingRestoreData.empty()) {
		Common::MemoryReadStream in(_pendingRestoreData.data(), _pendingRestoreData.size());
		gamestate_restore(_engine->getEngineState(), &in);
		_pendingRestoreData.clear();
		// SCI32 save/restore doesn't sync GfxFrameout's plane list; the game's
		// replay handler is supposed to teardown+re-add. For mid-game restores
		// (especially across rooms) leftover state shows up as ghost sprites.
		// Force-clear here so replay rebuilds from scratch. Note: don't call
		// resetHardware — its setPalette asserts when scummvm is in a
		// non-paletted screen mode (videos, hi-res transitions).
		if (_engine->_gfxFrameout) {
			_engine->_gfxFrameout->clear();
			_engine->_gfxFrameout->run(); // re-create the background fill plane
		}
	}
	if (_stepFramesRemaining > 0) {
		if (--_stepFramesRemaining == 0)
			pthread_cond_signal(cond);
	}
	if (_recordingActive)
		_recordingFrame++;
	// Script playback runs once per displayed frame; we fire any scheduled
	// clicks whose frame has arrived, then advance the playback frame counter.
	// When everything has fired and the trailing wait has elapsed, signal the
	// reader thread that the play_script call can return.
	if (_playbackActive) {
		Common::EventManager *em = g_system->getEventManager();
		while (_playbackIndex < _playbackQueue.size() &&
		       _playbackQueue[_playbackIndex].frame <= _playbackFrame) {
			const PlaybackAction &a = _playbackQueue[_playbackIndex];
			g_system->warpMouse(a.x, a.y);
			Common::EventType down = Common::EVENT_LBUTTONDOWN, up = Common::EVENT_LBUTTONUP;
			if (a.button == 2) { down = Common::EVENT_RBUTTONDOWN; up = Common::EVENT_RBUTTONUP; }
			else if (a.button == 3) { down = Common::EVENT_MBUTTONDOWN; up = Common::EVENT_MBUTTONUP; }
			if (a.kind != kPlaybackMouseUp) {
				Common::Event d;
				d.type = down;
				d.mouse = Common::Point(a.x, a.y);
				em->pushEvent(d);
			}
			if (a.kind != kPlaybackMouseDown) {
				Common::Event u;
				u.type = up;
				u.mouse = Common::Point(a.x, a.y);
				em->pushEvent(u);
			}
			_playbackIndex++;
		}
		if (_playbackIndex >= _playbackQueue.size() && _playbackFrame >= _playbackEndFrame) {
			_playbackActive = false;
			pthread_cond_signal(cond);
		} else {
			_playbackFrame++;
		}
	}
	pthread_mutex_unlock(mutex);
}

void McpServer::readerLoop() {
	Common::String buffer;
	char chunk[4096];
	while (_running) {
		ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
		if (n <= 0) {
			// EOF or error — graceful shutdown. Don't leave scummvm orphaned
			// if the MCP client process disappeared: drop our pause and push
			// a QUIT event so the engine exits cleanly.
			if (_pauseToken.isActive())
				_pauseToken.clear();
			Common::Event quit;
			quit.type = Common::EVENT_QUIT;
			g_system->getEventManager()->pushEvent(quit);
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

// Build a tool definition with the same x/y/button schema as click. Used for
// click, mouse_down, and mouse_up.
static Common::JSONValue *makeXYButtonToolDef(const char *name, const char *desc) {
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
	tool["name"] = new Common::JSONValue(Common::String(name));
	tool["description"] = new Common::JSONValue(Common::String(desc));
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

// Build a tool definition that takes a single required string "name" property.
static Common::JSONValue *makeNamedToolDef(const char *name, const char *desc) {
	Common::JSONObject nameProp;
	nameProp["type"] = new Common::JSONValue(Common::String("string"));
	nameProp["description"] = new Common::JSONValue(Common::String("User-chosen identifier for the snapshot."));
	Common::JSONObject properties;
	properties["name"] = new Common::JSONValue(nameProp);
	Common::JSONArray required;
	required.push_back(new Common::JSONValue(Common::String("name")));
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(properties);
	schema["required"] = new Common::JSONValue(required);
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String(name));
	tool["description"] = new Common::JSONValue(Common::String(desc));
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

// Build the `play_script` tool definition. Takes an `actions` array of
// {type: "click", x, y, button?} or {type: "wait", frames}.
static Common::JSONValue *makePlayScriptToolDef() {
	Common::JSONObject actionsArr;
	actionsArr["type"] = new Common::JSONValue(Common::String("array"));
	actionsArr["description"] = new Common::JSONValue(Common::String(
		"Ordered list of action objects. Types: "
		"{type:\"click\", x, y, button?} (press+release on the same frame), "
		"{type:\"mouse_down\", x, y, button?} (press only — hold), "
		"{type:\"mouse_up\", x, y, button?} (release only — completes a hold), "
		"{type:\"wait\", frames}."));
	Common::JSONObject properties;
	properties["actions"] = new Common::JSONValue(actionsArr);
	Common::JSONArray required;
	required.push_back(new Common::JSONValue(Common::String("actions")));
	Common::JSONObject schema;
	schema["type"] = new Common::JSONValue(Common::String("object"));
	schema["properties"] = new Common::JSONValue(properties);
	schema["required"] = new Common::JSONValue(required);
	Common::JSONObject tool;
	tool["name"] = new Common::JSONValue(Common::String("play_script"));
	tool["description"] = new Common::JSONValue(Common::String(
		"Play a script of click/wait actions at native engine speed. Blocks "
		"until the script completes. Clicks fire at their scheduled frame "
		"(determined by preceding `wait` durations). Engine runs unpaused "
		"throughout; pause state is restored on return. Result is JSON-encoded "
		"text {\"actions_played\": N, \"frames\": M}."));
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
		tools.push_back(makeXYButtonToolDef("click",
			"Click at (x,y) with the given button (left/right/middle, default left). "
			"Press and release fire on the same frame. Buffer coords (1:1 with screenshot)."));
		tools.push_back(makeXYButtonToolDef("mouse_down",
			"Press the given button at (x,y) without releasing. Used for puzzles "
			"that require holding (drag, sustained press). Pair with mouse_up to "
			"release. Buffer coords."));
		tools.push_back(makeXYButtonToolDef("mouse_up",
			"Release the given button at (x,y). Use after mouse_down to complete "
			"a hold/drag. The release coord can differ from the press coord (e.g. "
			"for drag-and-drop). Buffer coords."));
		tools.push_back(makeMoveCursorToolDef());
		tools.push_back(makeNamedToolDef("snapshot",
			"Save the current engine state to a named in-memory snapshot. "
			"Engine must be paused. Names overwrite. Snapshots are not "
			"persisted across scummvm exit and are not visible in the save menu."));
		tools.push_back(makeNamedToolDef("restore_snapshot",
			"Restore the engine to a previously-taken named snapshot. Engine "
			"must be paused. After restore, you'll usually need to step a few "
			"frames for the SCI VM to finish processing the load."));
		tools.push_back(makeNoArgToolDef("list_snapshots",
			"Return [{name, bytes, created_ms_since_engine_start}] for all snapshots in memory."));
		tools.push_back(makeNamedToolDef("drop_snapshot",
			"Free the in-memory snapshot with the given name. No-op if absent."));
		tools.push_back(makePlayScriptToolDef());
		tools.push_back(makeNoArgToolDef("start_record",
			"Begin recording clicks for TAS script authoring. Resets the recording "
			"frame counter and clears any prior recording. The engine continues "
			"running; clicks observed (left/right/middle mouse-up) are appended to "
			"the recording with their absolute frame number."));
		tools.push_back(makeNoArgToolDef("get_room",
			"Return the current SCI room number as JSON-encoded text "
			"{\"room\": N}. For Shivers, this is the global variable that "
			"identifies the current scene/screen; cross-reference it with the "
			"decompiled SCI script source to find click hotspots and game "
			"logic for the room."));
		tools.push_back(makeNoArgToolDef("end_record",
			"Stop recording and return the captured clicks as JSON-encoded text: "
			"[{\"frame\": N, \"x\": X, \"y\": Y, \"button\": B}, ...] where button "
			"is 1=left, 2=right, 3=middle. The driver folds the gaps between "
			"clicks into `wait` lines to produce a runnable script."));
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
		} else if (toolName == "get_room") {
			uint16 room = _engine->getEngineState()->currentRoomNumber();
			Common::JSONValue *result = makeTextResult(Common::String::format(
				"{\"room\": %u}", (unsigned int)room));
			sendResponse(buildOk(idVal, result));
			delete result;
		} else if (toolName == "click" || toolName == "move_cursor" ||
		           toolName == "mouse_down" || toolName == "mouse_up") {
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
				// Build the events we want to deliver. All of click/mouse_down/
				// mouse_up start with a move; click adds down+up; mouse_down
				// adds down only; mouse_up adds up only; move_cursor adds nothing.
				Common::Event mv;
				mv.type = Common::EVENT_MOUSEMOVE;
				mv.mouse = Common::Point(x, y);
				Common::Array<Common::Event> events;
				events.push_back(mv);
				if (toolName != "move_cursor") {
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
					if (toolName == "click" || toolName == "mouse_down") {
						Common::Event d;
						d.type = down;
						d.mouse = Common::Point(x, y);
						events.push_back(d);
					}
					if (toolName == "click" || toolName == "mouse_up") {
						Common::Event u;
						u.type = up;
						u.mouse = Common::Point(x, y);
						events.push_back(u);
					}
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
				const char *suffix = _pauseToken.isActive() ? " (queued)" : "";
				Common::String msg;
				if (toolName == "click")
					msg = Common::String::format("clicked %s at (%d,%d)%s", button.c_str(), x, y, suffix);
				else if (toolName == "mouse_down")
					msg = Common::String::format("pressed %s at (%d,%d)%s", button.c_str(), x, y, suffix);
				else if (toolName == "mouse_up")
					msg = Common::String::format("released %s at (%d,%d)%s", button.c_str(), x, y, suffix);
				else
					msg = Common::String::format("moved to (%d,%d)%s", x, y, suffix);
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
		} else if (toolName == "snapshot" || toolName == "restore_snapshot" || toolName == "drop_snapshot") {
			Common::String snapName;
			if (params && params->contains("arguments") && (*params)["arguments"]->isObject()) {
				const Common::JSONObject &a = (*params)["arguments"]->asObject();
				if (a.contains("name") && a["name"]->isString())
					snapName = a["name"]->asString();
			}
			if (snapName.empty()) {
				Common::JSONValue *result = makeTextResult("name is required and must be non-empty", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else if (toolName == "snapshot") {
				if (!_pauseToken.isActive()) {
					Common::JSONValue *result = makeTextResult("snapshot requires pause", true);
					sendResponse(buildOk(idVal, result));
					delete result;
				} else {
					Common::MemoryWriteStreamDynamic out(DisposeAfterUse::NO);
					bool ok = gamestate_save(_engine->getEngineState(), &out, snapName, "");
					if (!ok) {
						free(out.getData());
						Common::JSONValue *result = makeTextResult("gamestate_save failed", true);
						sendResponse(buildOk(idVal, result));
						delete result;
					} else {
						Common::Array<byte> data;
						data.resize(out.size());
						memcpy(data.data(), out.getData(), out.size());
						free(out.getData());
						_snapshotData[snapName].swap(data);
						_snapshotCreatedMs[snapName] = g_system->getMillis();
						Common::JSONValue *result = makeTextResult(Common::String::format("{\"name\": \"%s\", \"bytes\": %u}", snapName.c_str(), (uint)_snapshotData[snapName].size()));
						sendResponse(buildOk(idVal, result));
						delete result;
					}
				}
			} else if (toolName == "restore_snapshot") {
				if (!_pauseToken.isActive()) {
					Common::JSONValue *result = makeTextResult("restore_snapshot requires pause", true);
					sendResponse(buildOk(idVal, result));
					delete result;
				} else if (!_snapshotData.contains(snapName)) {
					Common::JSONValue *result = makeTextResult(Common::String::format("no snapshot named %s", snapName.c_str()), true);
					sendResponse(buildOk(idVal, result));
					delete result;
				} else if (!_stepMutex || !_stepCond) {
					Common::JSONValue *result = makeTextResult("restore synchronization not initialized", true);
					sendResponse(buildOk(idVal, result));
					delete result;
				} else {
					// Queue the restore to happen on the engine thread inside
					// onFrame (a SCI kernel-call boundary, same as how kRestoreGame
					// would do it). Then step a generous number of frames so the
					// abort-and-restart-VM sequence completes — capped by a wall
					// clock timeout in case the engine doesn't render those frames.
					pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
					pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
					pthread_mutex_lock(mutex);
					Common::Array<byte> &blob = _snapshotData[snapName];
					_pendingRestoreData.resize(blob.size());
					memcpy(_pendingRestoreData.data(), blob.data(), blob.size());
					flushPendingInputs();
					_stepFramesRemaining = 30;
					_pauseToken.clear();
					struct timespec deadline;
					clock_gettime(CLOCK_REALTIME, &deadline);
					deadline.tv_sec += 5;
					bool timedOut = false;
					while (_stepFramesRemaining > 0) {
						int rc = pthread_cond_timedwait(cond, mutex, &deadline);
						if (rc == ETIMEDOUT) { timedOut = true; break; }
					}
					_pauseToken = _engine->pauseEngine();
					int leftover = _stepFramesRemaining;
					_stepFramesRemaining = 0;
					pthread_mutex_unlock(mutex);
					Common::JSONValue *result;
					if (timedOut) {
						result = makeTextResult(Common::String::format(
							"restored %s; timed out waiting for %d more frames "
							"(engine may be in a non-rendering state — try `step` to advance)",
							snapName.c_str(), leftover), true);
					} else {
						result = makeTextResult(Common::String::format("restored %s", snapName.c_str()));
					}
					sendResponse(buildOk(idVal, result));
					delete result;
				}
			} else { // drop_snapshot
				bool found = _snapshotData.contains(snapName);
				_snapshotData.erase(snapName);
				_snapshotCreatedMs.erase(snapName);
				Common::JSONValue *result = makeTextResult(found ? "dropped" : "(not present)");
				sendResponse(buildOk(idVal, result));
				delete result;
			}
		} else if (toolName == "start_record") {
			pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
			if (mutex) pthread_mutex_lock(mutex);
			_recordedClicks.clear();
			_recordingFrame = 0;
			_recordingActive = true;
			if (mutex) pthread_mutex_unlock(mutex);
			Common::JSONValue *result = makeTextResult("recording");
			sendResponse(buildOk(idVal, result));
			delete result;
		} else if (toolName == "end_record") {
			pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
			Common::JSONArray arr;
			if (mutex) pthread_mutex_lock(mutex);
			_recordingActive = false;
			for (uint i = 0; i < _recordedClicks.size(); ++i) {
				const RecordedClick &c = _recordedClicks[i];
				Common::JSONObject obj;
				obj["frame"] = new Common::JSONValue((long long int)c.frame);
				obj["x"] = new Common::JSONValue((long long int)c.x);
				obj["y"] = new Common::JSONValue((long long int)c.y);
				obj["button"] = new Common::JSONValue((long long int)c.button);
				arr.push_back(new Common::JSONValue(obj));
			}
			if (mutex) pthread_mutex_unlock(mutex);
			Common::JSONValue jarr(arr);
			Common::String text = jarr.stringify();
			Common::JSONValue *result = makeTextResult(text);
			sendResponse(buildOk(idVal, result));
			delete result;
		} else if (toolName == "play_script") {
			// Parse actions into a flat queue of clicks with absolute frame
			// numbers. `wait` actions only advance a cursor; the trailing wait
			// determines the playback end frame.
			Common::Array<PlaybackAction> queue;
			int cursorFrame = 0;
			bool parseOk = true;
			Common::String parseErr;
			if (params && params->contains("arguments") && (*params)["arguments"]->isObject()) {
				const Common::JSONObject &a = (*params)["arguments"]->asObject();
				if (a.contains("actions") && a["actions"]->isArray()) {
					const Common::JSONArray &actions = a["actions"]->asArray();
					for (uint i = 0; i < actions.size(); ++i) {
						if (!actions[i]->isObject()) { parseOk = false; parseErr = "action must be an object"; break; }
						const Common::JSONObject &ao = actions[i]->asObject();
						if (!ao.contains("type") || !ao["type"]->isString()) { parseOk = false; parseErr = "action missing type"; break; }
						const Common::String &type = ao["type"]->asString();
						if (type == "click" || type == "mouse_down" || type == "mouse_up") {
							int x = 0, y = 0, btn = 1;
							bool haveX = false, haveY = false;
							if (ao.contains("x")) {
								if (ao["x"]->isIntegerNumber()) { x = (int)ao["x"]->asIntegerNumber(); haveX = true; }
								else if (ao["x"]->isNumber()) { x = (int)ao["x"]->asNumber(); haveX = true; }
							}
							if (ao.contains("y")) {
								if (ao["y"]->isIntegerNumber()) { y = (int)ao["y"]->asIntegerNumber(); haveY = true; }
								else if (ao["y"]->isNumber()) { y = (int)ao["y"]->asNumber(); haveY = true; }
							}
							if (!haveX || !haveY) { parseOk = false; parseErr = type + " action missing x or y"; break; }
							if (ao.contains("button") && ao["button"]->isString()) {
								const Common::String &b = ao["button"]->asString();
								if (b == "left") btn = 1;
								else if (b == "right") btn = 2;
								else if (b == "middle") btn = 3;
								else { parseOk = false; parseErr = Common::String::format("unknown button: %s", b.c_str()); break; }
							}
							PlaybackAction pa;
							pa.frame = cursorFrame;
							pa.kind = (type == "click") ? kPlaybackClick
							        : (type == "mouse_down") ? kPlaybackMouseDown
							        : kPlaybackMouseUp;
							pa.x = x; pa.y = y; pa.button = btn;
							queue.push_back(pa);
						} else if (type == "wait") {
							int frames = 0;
							bool haveFrames = false;
							if (ao.contains("frames")) {
								if (ao["frames"]->isIntegerNumber()) { frames = (int)ao["frames"]->asIntegerNumber(); haveFrames = true; }
								else if (ao["frames"]->isNumber()) { frames = (int)ao["frames"]->asNumber(); haveFrames = true; }
							}
							if (!haveFrames) { parseOk = false; parseErr = "wait action missing frames"; break; }
							if (frames < 0) { parseOk = false; parseErr = "wait frames must be >= 0"; break; }
							cursorFrame += frames;
						} else {
							parseOk = false; parseErr = Common::String::format("unknown action type: %s", type.c_str()); break;
						}
					}
				} else {
					parseOk = false; parseErr = "arguments.actions array required";
				}
			} else {
				parseOk = false; parseErr = "arguments object required";
			}
			if (!parseOk) {
				Common::JSONValue *result = makeTextResult(parseErr, true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else if (!_stepMutex || !_stepCond) {
				Common::JSONValue *result = makeTextResult("playback synchronization not initialized", true);
				sendResponse(buildOk(idVal, result));
				delete result;
			} else {
				pthread_mutex_t *mutex = static_cast<pthread_mutex_t *>(_stepMutex);
				pthread_cond_t *cond = static_cast<pthread_cond_t *>(_stepCond);
				pthread_mutex_lock(mutex);
				flushPendingInputs();
				_playbackQueue.swap(queue);
				_playbackEndFrame = cursorFrame;
				_playbackFrame = 0;
				_playbackIndex = 0;
				_playbackActive = true;
				_playbackPrevPaused = _pauseToken.isActive();
				if (_playbackPrevPaused)
					_pauseToken.clear();
				while (_playbackActive)
					pthread_cond_wait(cond, mutex);
				uint actionsPlayed = _playbackQueue.size();
				int endFrame = _playbackEndFrame;
				if (_playbackPrevPaused)
					_pauseToken = _engine->pauseEngine();
				pthread_mutex_unlock(mutex);
				Common::JSONValue *result = makeTextResult(Common::String::format(
					"{\"actions_played\": %u, \"frames\": %d}", actionsPlayed, endFrame));
				sendResponse(buildOk(idVal, result));
				delete result;
			}
		} else if (toolName == "list_snapshots") {
			Common::JSONArray arr;
			for (Common::HashMap<Common::String, Common::Array<byte> >::const_iterator it = _snapshotData.begin();
				 it != _snapshotData.end(); ++it) {
				Common::JSONObject obj;
				obj["name"] = new Common::JSONValue(it->_key);
				obj["bytes"] = new Common::JSONValue((long long int)it->_value.size());
				uint32 created = _snapshotCreatedMs.contains(it->_key) ? _snapshotCreatedMs[it->_key] : 0;
				obj["created_ms"] = new Common::JSONValue((long long int)created);
				arr.push_back(new Common::JSONValue(obj));
			}
			Common::JSONValue jarr(arr);
			Common::String text = jarr.stringify();
			Common::JSONValue *result = makeTextResult(text);
			sendResponse(buildOk(idVal, result));
			delete result;
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
	_stepFramesRemaining(0),
	_playbackActive(false), _playbackPrevPaused(false),
	_playbackFrame(0), _playbackEndFrame(0), _playbackIndex(0),
	_recordingActive(false), _recordingFrame(0), _observerHandle(nullptr) {}

McpServer::~McpServer() {}

void McpServer::start() {
	warning("McpServer: --mcp is only supported on POSIX platforms");
}

void McpServer::stop() {}
void McpServer::onFrame() {}
void McpServer::onObservedEvent(const Common::Event &) {}
void McpServer::readerLoop() {}
void McpServer::handleRequest(const Common::String &) {}
void McpServer::sendResponse(const Common::String &) {}
void *McpServer::threadEntry(void *) { return nullptr; }

#endif // POSIX

} // End of namespace Sci

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

#ifndef SCI_MCP_SERVER_H
#define SCI_MCP_SERVER_H

#include "common/array.h"
#include "common/events.h"
#include "common/hash-str.h"
#include "common/hashmap.h"
#include "common/scummsys.h"
#include "common/str.h"
#include "engines/engine.h" // for PauseToken

namespace Sci {

class SciEngine;

/**
 * A minimal JSON-RPC 2.0 server that speaks the Model Context Protocol over
 * stdio. Spawned by SciEngine when --mcp is on. Step 1 of an iterative buildout:
 * scaffolding only, no engine-touching tools yet.
 */
class McpServer {
public:
	explicit McpServer(SciEngine *engine);
	~McpServer();

	/** Start the reader thread and take over stdout for protocol output. */
	void start();

	/** Signal the reader thread to exit and join. */
	void stop();

	/**
	 * Called from the engine main thread once per displayed frame. Used by
	 * the `step` tool to count down frames advanced while the engine was
	 * temporarily resumed.
	 */
	void onFrame();

private:
	SciEngine *_engine;
	int _realStdoutFd;
	bool _running;
	void *_threadHandle;       // pthread_t cast to void* to avoid the dep in headers
	void *_stepMutex;          // pthread_mutex_t *, owns _stepFramesRemaining
	void *_stepCond;           // pthread_cond_t *, signaled when remaining hits 0
	int _stepFramesRemaining;  // protected by _stepMutex
	PauseToken _pauseToken;    // active while pause tool has been called without a matching unpause
	Common::Array<Common::Event> _pendingInputs; // buffered while paused; protected by _stepMutex

	// Named save-states, in-memory only, keyed by user string. Each entry holds
	// the bytes of a gamestate_save and the millis at which it was created.
	Common::HashMap<Common::String, Common::Array<byte> > _snapshotData;
	Common::HashMap<Common::String, uint32> _snapshotCreatedMs;

	// Pending restore: gamestate_restore must run on the engine thread between
	// frames (so the SCI VM is at a kernel-call boundary, the same place
	// kRestoreGame would invoke it from). The reader thread fills this and
	// waits; onFrame consumes it and signals back via _stepCond.
	Common::Array<byte> _pendingRestoreData; // protected by _stepMutex

	// Script playback. Populated by play_script and consumed on the engine
	// thread in onFrame, advancing one frame per onFrame call and firing
	// scheduled clicks at the right absolute frame. The engine runs at native
	// speed throughout — there is no pause/step cycling.
	enum PlaybackKind {
		kPlaybackClick = 0,      // press + release on the same frame
		kPlaybackMouseDown = 1,  // press only (puzzle drag/hold)
		kPlaybackMouseUp = 2,    // release only
	};
	struct PlaybackAction {
		int frame;   // absolute frame at which to fire (0 = first onFrame)
		PlaybackKind kind;
		int x, y;
		int button;  // 1 = left, 2 = right, 3 = middle
	};
	bool _playbackActive;       // protected by _stepMutex
	bool _playbackPrevPaused;   // remember pause state across playback
	bool _playbackCancelled;    // set by onFrame when a SIGUSR1 cancel landed mid-playback
	int _playbackFrame;
	int _playbackEndFrame;
	uint _playbackIndex;
	Common::Array<PlaybackAction> _playbackQueue;

	// Recording. Active between the start_record and end_record tools. An
	// EventObserver (registered with the OSystem event dispatcher in start())
	// observes — without consuming — every dispatched event; mouse button
	// events (down and up, separately) while _recordingActive is true are
	// appended to _recordedClicks with the recording-mode frame counter as
	// their timestamp. The driver collapses same-frame down+up pairs into
	// `click` lines and emits unpaired half-events as `mouse_down`/`mouse_up`.
	struct RecordedClick {
		int frame;
		PlaybackKind kind;  // kPlaybackMouseDown or kPlaybackMouseUp
		int x, y;
		int button;         // 1 = left, 2 = right, 3 = middle
	};
	bool _recordingActive;       // protected by _stepMutex
	int _recordingFrame;         // protected by _stepMutex
	Common::Array<RecordedClick> _recordedClicks; // protected by _stepMutex
	void *_observerHandle;       // McpEventObserver *, lifetime owned by McpServer

public:
	/** Engine thread: called by the registered EventObserver for every dispatched event. */
	void onObservedEvent(const Common::Event &event);

private:

	void flushPendingInputs(); // assumes _stepMutex is held

	void readerLoop();
	void handleRequest(const Common::String &line);
	void sendResponse(const Common::String &json);

	static void *threadEntry(void *opaque);
};

} // End of namespace Sci

#endif

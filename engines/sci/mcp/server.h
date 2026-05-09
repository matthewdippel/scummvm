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

#include "common/scummsys.h"
#include "common/str.h"

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

private:
	SciEngine *_engine;
	int _realStdoutFd;
	bool _running;
	void *_threadHandle;       // pthread_t cast to void* to avoid the dep in headers

	void readerLoop();
	void handleRequest(const Common::String &line);
	void sendResponse(const Common::String &json);

	static void *threadEntry(void *opaque);
};

} // End of namespace Sci

#endif

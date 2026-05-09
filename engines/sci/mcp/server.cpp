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

#include "common/formats/json.h"
#include "common/textconsole.h"
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
	_engine(engine), _realStdoutFd(-1), _running(false), _threadHandle(nullptr) {}

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
	// _realStdoutFd is owned by scummvm_main (g_mcpRealStdoutFd); don't close.
	_realStdoutFd = -1;
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
		Common::JSONObject result;
		result["tools"] = new Common::JSONValue(Common::JSONArray()); // empty for now
		Common::JSONValue resultVal(result);
		sendResponse(buildOk(idVal, &resultVal));
	} else if (method == "tools/call") {
		sendResponse(buildError(idVal, -32601, "No tools registered yet"));
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
	_engine(engine), _realStdoutFd(-1), _running(false), _threadHandle(nullptr) {}

McpServer::~McpServer() {}

void McpServer::start() {
	warning("McpServer: --mcp is only supported on POSIX platforms");
}

void McpServer::stop() {}
void McpServer::readerLoop() {}
void McpServer::handleRequest(const Common::String &) {}
void McpServer::sendResponse(const Common::String &) {}
void *McpServer::threadEntry(void *) { return nullptr; }

#endif // POSIX

} // End of namespace Sci

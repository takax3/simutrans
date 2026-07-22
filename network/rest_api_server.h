/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#ifndef NETWORK_REST_API_SERVER_H
#define NETWORK_REST_API_SERVER_H

#include "network.h"

#include <string>
#include <vector>

class karte_t;

/**
 * Small, read-only HTTP server for exporting observer data.
 *
 * The server is polled from the game main loop.  It never keeps pointers to
 * game objects after a response body has been built, and all socket I/O is
 * non-blocking.
 */
class rest_api_server_t
{
public:
	/** Start loopback listeners on the given port. */
	static bool init(uint16 port);

	/** Accept requests and flush responses. Called from the main game loop. */
	static void step(karte_t *welt);

	/** Close all connections and listener sockets. */
	static void shutdown();

	/** Mark that a new map was created or loaded. */
	static void notify_world_changed();

	static bool is_active() { return !listen_socks.empty(); }

private:
	struct connection_t {
		SOCKET sock;
		std::string recv_buf;
		std::string send_buf;
		uint32 accepted_at;
		bool request_handled;

		connection_t(SOCKET socket, uint32 now) :
			sock(socket), accepted_at(now), request_handled(false)
		{}
	};

	static std::vector<SOCKET> listen_socks;
	static std::vector<connection_t *> connections;
	static karte_t *world;
	static uint64 world_epoch;
	static uint64 snapshot_sequence;

	static void accept_new();
	static void io_connection(connection_t *connection);
	static void handle_request(connection_t *connection, const std::string &request_line);
	static void close_socket(SOCKET socket);
};

#endif // NETWORK_REST_API_SERVER_H

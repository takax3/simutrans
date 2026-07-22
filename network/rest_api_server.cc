/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#include "rest_api_server.h"
#include "rest_api_spec_generated.h"

#include <algorithm>
#include <errno.h>
#include <sstream>
#include <stdio.h>
#include <string.h>

#if !USE_WINSOCK
#	include <fcntl.h>
#	include <unistd.h>
#endif

#include "../dataobj/koord3d.h"
#include "../dataobj/schedule.h"
#include "../linehandle_t.h"
#include "../player/simplay.h"
#include "../simconvoi.h"
#include "../simconst.h"
#include "../simdebug.h"
#include "../simline.h"
#include "../simunits.h"
#include "../simworld.h"
#include "../sys/simsys.h"
#include "../vehicle/simvehicle.h"

std::vector<SOCKET> rest_api_server_t::listen_socks;
std::vector<rest_api_server_t::connection_t *> rest_api_server_t::connections;
karte_t *rest_api_server_t::world = NULL;
uint64 rest_api_server_t::world_epoch = 0;
uint64 rest_api_server_t::snapshot_sequence = 0;

namespace {

static const size_t MAX_CONNECTIONS = 8;
static const size_t MAX_REQUEST_SIZE = 8 * 1024;
static const uint32 REQUEST_TIMEOUT_MS = 5000;

struct waytype_filter_t {
	enum mode_t { all, rail, exact } mode;
	waytype_t waytype;

	waytype_filter_t() : mode(all), waytype(invalid_wt) {}
};

struct line_filter_t {
	waytype_filter_t waytype;
	bool has_company_id;
	uint8 company_id;

	line_filter_t() : has_company_id(false), company_id(0) {}
};

static void set_nonblocking(SOCKET socket)
{
#if USE_WINSOCK
	u_long mode = 1;
	ioctlsocket(socket, FIONBIO, &mode);
#else
	const int flags = fcntl(socket, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(socket, F_SETFL, flags | O_NONBLOCK);
	}
#endif
}

static bool socket_would_block()
{
#if USE_WINSOCK
	const int error = GET_LAST_ERROR();
	return error == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static std::string json_escape(const char *text)
{
	if (text == NULL) {
		return std::string();
	}

	std::string escaped;
	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
		switch (*p) {
			case '"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (*p < 0x20) {
					char buffer[8];
					snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned int)*p);
					escaped += buffer;
				}
				else {
					escaped += static_cast<char>(*p);
				}
		}
	}
	return escaped;
}

static const char *waytype_name(waytype_t waytype)
{
	switch (waytype) {
		case road_wt:        return "road";
		case track_wt:       return "track";
		case water_wt:       return "water";
		case air_wt:         return "air";
		case monorail_wt:    return "monorail";
		case tram_wt:        return "tram";
		case maglev_wt:      return "maglev";
		case narrowgauge_wt: return "narrowgauge";
		default:             return "unknown";
	}
}

static waytype_t convoy_waytype(const convoi_t *convoy)
{
	if (convoy == NULL) {
		return invalid_wt;
	}
	if (const schedule_t *schedule = convoy->get_schedule()) {
		return schedule->get_waytype();
	}
	if (convoy->get_vehicle_count() > 0 && convoy->front() != NULL) {
		return convoy->front()->get_waytype();
	}
	return invalid_wt;
}

static bool is_rail_waytype(waytype_t waytype)
{
	return waytype == track_wt || waytype == tram_wt || waytype == monorail_wt ||
		waytype == maglev_wt || waytype == narrowgauge_wt;
}

static bool filter_matches(const waytype_filter_t &filter, waytype_t waytype)
{
	switch (filter.mode) {
		case waytype_filter_t::all:   return true;
		case waytype_filter_t::rail:  return is_rail_waytype(waytype);
		case waytype_filter_t::exact: return filter.waytype == waytype;
	}
	return false;
}

static bool parse_waytype_name(const std::string &value, waytype_filter_t &filter)
{
	if (value.empty() || value == "all") {
		filter.mode = waytype_filter_t::all;
		return true;
	}
	if (value == "rail") {
		filter.mode = waytype_filter_t::rail;
		return true;
	}

	struct named_waytype_t {
		const char *name;
		waytype_t waytype;
	};
	static const named_waytype_t named_waytypes[] = {
		{ "road", road_wt },
		{ "track", track_wt },
		{ "water", water_wt },
		{ "air", air_wt },
		{ "monorail", monorail_wt },
		{ "tram", tram_wt },
		{ "maglev", maglev_wt },
		{ "narrowgauge", narrowgauge_wt }
	};

	for (size_t i = 0; i < sizeof(named_waytypes) / sizeof(named_waytypes[0]); ++i) {
		if (value == named_waytypes[i].name) {
			filter.mode = waytype_filter_t::exact;
			filter.waytype = named_waytypes[i].waytype;
			return true;
		}
	}
	return false;
}

static bool parse_waytype_query(const std::string &query, waytype_filter_t &filter, std::string &error)
{
	if (query.empty()) {
		return true;
	}

	bool found_waytype = false;
	size_t start = 0;
	while (start <= query.size()) {
		const size_t end = query.find('&', start);
		const std::string item = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const size_t equals = item.find('=');
		if (equals == std::string::npos || item.substr(0, equals) != "waytype") {
			error = "unknown or malformed query parameter";
			return false;
		}
		if (found_waytype) {
			error = "waytype may only be specified once";
			return false;
		}
		found_waytype = true;
		if (!parse_waytype_name(item.substr(equals + 1), filter)) {
			error = "unknown waytype";
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

static bool parse_company_id(const std::string &value, uint8 &company_id)
{
	if (value.empty()) {
		return false;
	}
	unsigned int parsed = 0;
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] < '0' || value[i] > '9') {
			return false;
		}
		parsed = parsed * 10 + static_cast<unsigned int>(value[i] - '0');
		if (parsed >= MAX_PLAYER_COUNT) {
			return false;
		}
	}
	company_id = static_cast<uint8>(parsed);
	return true;
}

static bool parse_line_query(const std::string &query, line_filter_t &filter, std::string &error)
{
	if (query.empty()) {
		return true;
	}

	bool found_waytype = false;
	size_t start = 0;
	while (start <= query.size()) {
		const size_t end = query.find('&', start);
		const std::string item = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const size_t equals = item.find('=');
		if (equals == std::string::npos) {
			error = "unknown or malformed query parameter";
			return false;
		}
		const std::string name = item.substr(0, equals);
		const std::string value = item.substr(equals + 1);
		if (name == "waytype") {
			if (found_waytype) {
				error = "waytype may only be specified once";
				return false;
			}
			found_waytype = true;
			if (!parse_waytype_name(value, filter.waytype)) {
				error = "unknown waytype";
				return false;
			}
		}
		else if (name == "company_id") {
			if (filter.has_company_id) {
				error = "company_id may only be specified once";
				return false;
			}
			filter.has_company_id = true;
			if (!parse_company_id(value, filter.company_id)) {
				error = "invalid company_id";
				return false;
			}
		}
		else {
			error = "unknown or malformed query parameter";
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

static const char *ai_type_name(uint8 ai_type)
{
	switch (ai_type) {
		case player_t::HUMAN:        return "human";
		case player_t::AI_GOODS:     return "goods";
		case player_t::AI_PASSENGER: return "passenger";
		case player_t::AI_SCRIPTED:  return "scripted";
		default:                     return "unknown";
	}
}

static const char *convoy_state_name(int state)
{
	switch (state) {
		case convoi_t::INITIAL:                          return "initial";
		case convoi_t::EDIT_SCHEDULE:                    return "edit_schedule";
		case convoi_t::ROUTING_1:                        return "routing";
		case convoi_t::DUMMY4:                           return "reserved_3";
		case convoi_t::DUMMY5:                           return "reserved_4";
		case convoi_t::NO_ROUTE:                         return "no_route";
		case convoi_t::DRIVING:                          return "driving";
		case convoi_t::LOADING:                          return "loading";
		case convoi_t::WAITING_FOR_CLEARANCE:            return "waiting_for_clearance";
		case convoi_t::WAITING_FOR_CLEARANCE_ONE_MONTH:  return "waiting_for_clearance_one_month";
		case convoi_t::CAN_START:                        return "can_start";
		case convoi_t::CAN_START_ONE_MONTH:              return "can_start_one_month";
		case convoi_t::SELF_DESTRUCT:                    return "self_destruct";
		case convoi_t::WAITING_FOR_CLEARANCE_TWO_MONTHS: return "waiting_for_clearance_two_months";
		case convoi_t::CAN_START_TWO_MONTHS:             return "can_start_two_months";
		case convoi_t::LEAVING_DEPOT:                    return "leaving_depot";
		case convoi_t::ENTERING_DEPOT:                   return "entering_depot";
		case convoi_t::COUPLED:                          return "coupled";
		case convoi_t::COUPLED_LOADING:                  return "coupled_loading";
		case convoi_t::WAITING_FOR_LEAVING_DEPOT:        return "waiting_for_leaving_depot";
		default:                                         return "unknown";
	}
}

static std::string metadata_headers(uint64 epoch, uint64 sequence, uint32 sync_step, uint32 generated_at)
{
	std::ostringstream headers;
	headers << "X-Simutrans-World-Epoch: " << epoch << "\r\n"
		<< "X-Simutrans-Snapshot-Sequence: " << sequence << "\r\n"
		<< "X-Simutrans-Sync-Step: " << sync_step << "\r\n"
		<< "X-Simutrans-Generated-At-Ms: " << generated_at << "\r\n";
	return headers.str();
}

static std::string make_http_response(int status, const char *reason, const char *content_type,
	const std::string &body, const std::string &extra_headers = std::string())
{
	std::ostringstream response;
	response << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
	if (content_type != NULL) {
		response << "Content-Type: " << content_type << "\r\n";
	}
	response
		<< "Content-Length: " << body.size() << "\r\n"
		<< "Cache-Control: no-store\r\n"
		<< "X-Content-Type-Options: nosniff\r\n"
		<< "Access-Control-Allow-Origin: *\r\n"
		<< "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
		<< "Access-Control-Allow-Headers: Accept, Content-Type\r\n"
		<< "Access-Control-Expose-Headers: X-Simutrans-World-Epoch, X-Simutrans-Snapshot-Sequence, X-Simutrans-Sync-Step, X-Simutrans-Generated-At-Ms\r\n"
		<< "Connection: close\r\n"
		<< extra_headers
		<< "\r\n"
		<< body;
	return response.str();
}

static std::string make_error_response(int status, const char *reason, const std::string &message,
	const std::string &extra_headers = std::string())
{
	return make_http_response(status, reason, "application/json; charset=utf-8",
		std::string("{\"error\":\"") + json_escape(message.c_str()) + "\"}\n", extra_headers);
}

static bool is_known_path(const std::string &path)
{
	return path == "/" ||
		path == "/api/v1/openapi.yaml" ||
		path == "/api/v1/openapi.json" ||
		path == "/api/v1/companies" ||
		path == "/api/v1/lines" ||
		path == "/api/v1/convoys" ||
		path == "/api/v1/convoy-positions";
}

static std::string make_convoys_json(karte_t *world, const waytype_filter_t &filter,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at
		<< ",\"carunits_per_tile\":" << CARUNITS_PER_TILE
		<< ",\"convoys\":[";

	bool first = true;
	const vector_tpl<convoihandle_t> &convoys = world->convoys();
	for (size_t i = 0; i < convoys.get_count(); ++i) {
		const convoihandle_t handle = convoys[i];
		if (!handle.is_bound()) {
			continue;
		}
		const convoi_t *convoy = handle.get_rep();
		const waytype_t waytype = convoy_waytype(convoy);
		if (!filter_matches(filter, waytype)) {
			continue;
		}

		if (!first) {
			body << ',';
		}
		first = false;

		body << "{\"id\":" << handle.get_id()
			<< ",\"name\":\"" << json_escape(convoy->get_name()) << '"';

		if (const player_t *owner = convoy->get_owner()) {
			body << ",\"company_id\":" << static_cast<unsigned int>(owner->get_player_nr());
		}
		else {
			body << ",\"company_id\":null";
		}

		const linehandle_t line = convoy->get_line();
		if (line.is_bound()) {
			body << ",\"line_id\":" << line.get_id();
		}
		else {
			body << ",\"line_id\":null";
		}

		body << ",\"waytype\":\"" << waytype_name(waytype) << '"'
			<< ",\"vehicle_count\":" << static_cast<unsigned int>(convoy->get_vehicle_count())
			<< ",\"length_carunits\":" << convoy->get_length()
			<< '}';
	}

	body << "]}\n";
	return body.str();
}

static std::string make_companies_json(karte_t *world, uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at
		<< ",\"companies\":[";

	bool first = true;
	for (uint8 i = 0; i < MAX_PLAYER_COUNT; ++i) {
		const player_t *company = world->get_player(i);
		if (company == NULL) {
			continue;
		}
		if (!first) {
			body << ',';
		}
		first = false;
		body << "{\"id\":" << static_cast<unsigned int>(company->get_player_nr())
			<< ",\"name\":\"" << json_escape(company->get_name()) << '"'
			<< ",\"public_service\":" << (company->is_public_service() ? "true" : "false")
			<< ",\"ai_type\":\"" << ai_type_name(company->get_ai_id()) << '"'
			<< ",\"ai_active\":" << (company->is_active() ? "true" : "false")
			<< ",\"locked\":" << (company->is_locked() ? "true" : "false")
			<< ",\"primary_color_index\":" << static_cast<unsigned int>(company->get_player_color1())
			<< ",\"secondary_color_index\":" << static_cast<unsigned int>(company->get_player_color2())
			<< '}';
	}
	body << "]}\n";
	return body.str();
}

struct line_id_less_t {
	bool operator()(const linehandle_t &a, const linehandle_t &b) const { return a.get_id() < b.get_id(); }
};

static std::string make_lines_json(karte_t *world, const line_filter_t &filter,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::vector<linehandle_t> lines;
	for (uint8 i = 0; i < MAX_PLAYER_COUNT; ++i) {
		const player_t *company = world->get_player(i);
		if (company == NULL || (filter.has_company_id && filter.company_id != i)) {
			continue;
		}
		const vector_tpl<linehandle_t> &company_lines = company->simlinemgmt.get_line_list();
		for (uint32 line_index = 0; line_index < company_lines.get_count(); ++line_index) {
			const linehandle_t line = company_lines[line_index];
			if (line.is_bound() && filter_matches(filter.waytype, simline_t::linetype_to_waytype(line->get_linetype()))) {
				lines.push_back(line);
			}
		}
	}
	std::sort(lines.begin(), lines.end(), line_id_less_t());

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at
		<< ",\"lines\":[";
	for (size_t i = 0; i < lines.size(); ++i) {
		const linehandle_t line = lines[i];
		const player_t *owner = line->get_owner();
		if (i > 0) {
			body << ',';
		}
		body << "{\"id\":" << line.get_id()
			<< ",\"name\":\"" << json_escape(line->get_name()) << '"'
			<< ",\"company_id\":" << static_cast<unsigned int>(owner->get_player_nr())
			<< ",\"waytype\":\"" << waytype_name(simline_t::linetype_to_waytype(line->get_linetype())) << '"'
			<< ",\"convoy_count\":" << line->count_convoys()
			<< ",\"withdraw\":" << (line->get_withdraw() ? "true" : "false")
			<< ",\"color_index\":" << static_cast<unsigned int>(line->get_colour())
			<< '}';
	}
	body << "]}\n";
	return body.str();
}

static std::string make_positions_csv(karte_t *world, const waytype_filter_t &filter)
{
	std::ostringstream body;
	body << "convoy_id,waytype,state,state_code,speed_kmh,x,y,z,route_index\r\n";

	const vector_tpl<convoihandle_t> &convoys = world->convoys();
	for (size_t i = 0; i < convoys.get_count(); ++i) {
		const convoihandle_t handle = convoys[i];
		if (!handle.is_bound()) {
			continue;
		}
		const convoi_t *convoy = handle.get_rep();
		const waytype_t waytype = convoy_waytype(convoy);
		if (!filter_matches(filter, waytype)) {
			continue;
		}

		const int state = convoy->get_state();
		const sint32 measured_speed_kmh = speed_to_kmh(convoy->get_akt_speed());
		const sint32 speed_kmh = measured_speed_kmh > 0 ? measured_speed_kmh : 0;
		const koord3d position = convoy->get_pos();

		body << handle.get_id() << ',' << waytype_name(waytype) << ','
			<< convoy_state_name(state) << ',' << state << ',' << speed_kmh << ',';
		if (position != koord3d::invalid) {
			body << position.x << ',' << position.y << ',' << static_cast<int>(position.z) << ',';
		}
		else {
			body << ",,,";
		}

		if (!convoy->in_depot() && convoy->get_vehicle_count() > 0 && convoy->front() != NULL) {
			body << convoy->front()->get_route_index();
		}
		body << "\r\n";
	}

	return body.str();
}

} // namespace

bool rest_api_server_t::init(uint16 port)
{
	if (!listen_socks.empty()) {
		dbg->warning("rest_api_server_t::init", "already running");
		return false;
	}

#if USE_WINSOCK
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		dbg->warning("rest_api_server_t::init", "WSAStartup failed");
		return false;
	}
#endif

	SOCKET ipv6 = socket(AF_INET6, SOCK_STREAM, 0);
	if (ipv6 != INVALID_SOCKET) {
		int on = 1;
		setsockopt(ipv6, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));
		setsockopt(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&on), sizeof(on));

		struct sockaddr_in6 address;
		memset(&address, 0, sizeof(address));
		address.sin6_family = AF_INET6;
		address.sin6_port = htons(port);
		address.sin6_addr = in6addr_loopback;
		if (bind(ipv6, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0 && listen(ipv6, 8) == 0) {
			set_nonblocking(ipv6);
			listen_socks.push_back(ipv6);
		}
		else {
			close_socket(ipv6);
		}
	}

	SOCKET ipv4 = socket(AF_INET, SOCK_STREAM, 0);
	if (ipv4 != INVALID_SOCKET) {
		int on = 1;
		setsockopt(ipv4, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));

		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(ipv4, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0 && listen(ipv4, 8) == 0) {
			set_nonblocking(ipv4);
			listen_socks.push_back(ipv4);
		}
		else {
			close_socket(ipv4);
		}
	}

	if (listen_socks.empty()) {
		dbg->warning("rest_api_server_t::init", "could not bind loopback port %u", port);
		return false;
	}

	dbg->message("rest_api_server_t::init", "REST API listening on loopback port %u", port);
	return true;
}

void rest_api_server_t::shutdown()
{
	for (size_t i = 0; i < connections.size(); ++i) {
		close_socket(connections[i]->sock);
		delete connections[i];
	}
	connections.clear();

	for (size_t i = 0; i < listen_socks.size(); ++i) {
		close_socket(listen_socks[i]);
	}
	listen_socks.clear();
	world = NULL;
}

void rest_api_server_t::notify_world_changed()
{
	++world_epoch;
}

void rest_api_server_t::step(karte_t *welt)
{
	if (listen_socks.empty()) {
		return;
	}
	if (world != welt) {
		world = welt;
		++world_epoch;
	}

	accept_new();

	std::vector<connection_t *> dead;
	for (size_t i = 0; i < connections.size(); ++i) {
		connection_t *connection = connections[i];
		io_connection(connection);
		if (connection->sock == INVALID_SOCKET) {
			dead.push_back(connection);
		}
	}
	for (size_t i = 0; i < dead.size(); ++i) {
		connection_t *connection = dead[i];
		connections.erase(std::find(connections.begin(), connections.end(), connection));
		delete connection;
	}
}

void rest_api_server_t::accept_new()
{
	for (size_t listener_index = 0; listener_index < listen_socks.size(); ++listener_index) {
		for (;;) {
			struct sockaddr_storage address;
			socklen_t address_length = sizeof(address);
			SOCKET client = accept(listen_socks[listener_index], reinterpret_cast<struct sockaddr *>(&address), &address_length);
			if (client == INVALID_SOCKET) {
				break;
			}
			if (connections.size() >= MAX_CONNECTIONS) {
				close_socket(client);
				continue;
			}

			set_nonblocking(client);
			int on = 1;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
			connections.push_back(new connection_t(client, dr_time()));
		}
	}
}

void rest_api_server_t::io_connection(connection_t *connection)
{
	if (connection->sock == INVALID_SOCKET) {
		return;
	}

	if (!connection->request_handled && dr_time() - connection->accepted_at > REQUEST_TIMEOUT_MS) {
		connection->send_buf = make_error_response(408, "Request Timeout", "request timed out");
		connection->request_handled = true;
	}

	if (!connection->request_handled) {
		char buffer[4096];
		for (;;) {
#if USE_WINSOCK
			const int received = recv(connection->sock, buffer, sizeof(buffer), 0);
#else
			const ssize_t received = recv(connection->sock, buffer, sizeof(buffer), 0);
#endif
			if (received > 0) {
				connection->recv_buf.append(buffer, static_cast<size_t>(received));
			}
			else if (received == 0) {
				close_socket(connection->sock);
				connection->sock = INVALID_SOCKET;
				return;
			}
			else {
				if (!socket_would_block()) {
					close_socket(connection->sock);
					connection->sock = INVALID_SOCKET;
				}
				break;
			}
		}

		if (connection->recv_buf.size() > MAX_REQUEST_SIZE) {
			connection->send_buf = make_error_response(431, "Request Header Fields Too Large", "request headers too large");
			connection->request_handled = true;
		}
		else {
			size_t header_end = connection->recv_buf.find("\r\n\r\n");
			if (header_end == std::string::npos) {
				header_end = connection->recv_buf.find("\n\n");
			}
			if (header_end != std::string::npos) {
				size_t line_end = connection->recv_buf.find("\r\n");
				if (line_end == std::string::npos) {
					line_end = connection->recv_buf.find('\n');
				}
				if (line_end == std::string::npos) {
					connection->send_buf = make_error_response(400, "Bad Request", "missing request line");
				}
				else {
					handle_request(connection, connection->recv_buf.substr(0, line_end));
				}
				connection->request_handled = true;
			}
		}
	}

	while (connection->sock != INVALID_SOCKET && !connection->send_buf.empty()) {
#if USE_WINSOCK
		const int sent = send(connection->sock, connection->send_buf.data(), static_cast<int>(connection->send_buf.size()), 0);
#else
		const ssize_t sent = send(connection->sock, connection->send_buf.data(), connection->send_buf.size(), 0);
#endif
		if (sent > 0) {
			connection->send_buf.erase(0, static_cast<size_t>(sent));
		}
		else {
			if (!socket_would_block()) {
				close_socket(connection->sock);
				connection->sock = INVALID_SOCKET;
			}
			break;
		}
	}

	if (connection->sock != INVALID_SOCKET && connection->request_handled && connection->send_buf.empty()) {
		close_socket(connection->sock);
		connection->sock = INVALID_SOCKET;
	}
}

void rest_api_server_t::handle_request(connection_t *connection, const std::string &request_line)
{
	std::istringstream line(request_line);
	std::string method;
	std::string target;
	std::string version;
	std::string extra;
	if (!(line >> method >> target >> version) || (line >> extra)) {
		connection->send_buf = make_error_response(400, "Bad Request", "malformed request line");
		return;
	}
	if (version != "HTTP/1.0" && version != "HTTP/1.1") {
		connection->send_buf = make_error_response(505, "HTTP Version Not Supported", "only HTTP/1.0 and HTTP/1.1 are supported");
		return;
	}
	const size_t question = target.find('?');
	const std::string path = target.substr(0, question);
	const std::string query = question == std::string::npos ? std::string() : target.substr(question + 1);
	if (method == "OPTIONS") {
		if (!is_known_path(path)) {
			connection->send_buf = make_error_response(404, "Not Found", "unknown API path");
		}
		else {
			connection->send_buf = make_http_response(204, "No Content", NULL, std::string(),
				"Access-Control-Max-Age: 86400\r\n");
		}
		return;
	}
	if (method != "GET") {
		connection->send_buf = make_error_response(405, "Method Not Allowed", "only GET and OPTIONS are supported",
			"Allow: GET, OPTIONS\r\n");
		return;
	}
	if (path == "/") {
		if (!query.empty()) {
			connection->send_buf = make_error_response(400, "Bad Request", "query parameters are not supported");
			return;
		}
		const std::string body =
			"{\"name\":\"Simutrans Observer REST API\",\"api_version\":\"v1\","
			"\"openapi\":{\"yaml\":\"/api/v1/openapi.yaml\",\"json\":\"/api/v1/openapi.json\"},"
			"\"endpoints\":{\"companies\":\"/api/v1/companies\",\"lines\":\"/api/v1/lines\","
			"\"convoys\":\"/api/v1/convoys\",\"convoy_positions\":\"/api/v1/convoy-positions\"}}\n";
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body);
		return;
	}
	if (path == "/api/v1/openapi.yaml" || path == "/api/v1/openapi.json") {
		if (!query.empty()) {
			connection->send_buf = make_error_response(400, "Bad Request", "query parameters are not supported");
			return;
		}
		if (path == "/api/v1/openapi.yaml") {
			connection->send_buf = make_http_response(200, "OK", "application/yaml; charset=utf-8", REST_API_OPENAPI_YAML);
		}
		else {
			connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", REST_API_OPENAPI_JSON);
		}
		return;
	}

	const bool is_companies = path == "/api/v1/companies";
	const bool is_lines = path == "/api/v1/lines";
	const bool is_convoys = path == "/api/v1/convoys";
	const bool is_positions = path == "/api/v1/convoy-positions";
	if (!is_companies && !is_lines && !is_convoys && !is_positions) {
		connection->send_buf = make_error_response(404, "Not Found", "unknown API path");
		return;
	}

	waytype_filter_t waytype_filter;
	line_filter_t line_filter;
	std::string query_error;
	if (is_companies && !query.empty()) {
		connection->send_buf = make_error_response(400, "Bad Request", "query parameters are not supported");
		return;
	}
	if (is_lines && !parse_line_query(query, line_filter, query_error)) {
		connection->send_buf = make_error_response(400, "Bad Request", query_error);
		return;
	}
	if ((is_convoys || is_positions) && !parse_waytype_query(query, waytype_filter, query_error)) {
		connection->send_buf = make_error_response(400, "Bad Request", query_error);
		return;
	}
	if (world == NULL) {
		connection->send_buf = make_error_response(503, "Service Unavailable", "no world is loaded");
		return;
	}

	const uint64 sequence = ++snapshot_sequence;
	const uint32 generated_at = dr_time();
	const uint32 sync_step = world->get_sync_steps();
	const std::string headers = metadata_headers(world_epoch, sequence, sync_step, generated_at);

	if (is_companies) {
		const std::string body = make_companies_json(world, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_lines) {
		const std::string body = make_lines_json(world, line_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_convoys) {
		const std::string body = make_convoys_json(world, waytype_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else {
		const std::string body = make_positions_csv(world, waytype_filter);
		connection->send_buf = make_http_response(200, "OK", "text/csv; charset=utf-8", body, headers);
	}
}

void rest_api_server_t::close_socket(SOCKET socket)
{
	if (socket == INVALID_SOCKET) {
		return;
	}
#if USE_WINSOCK
	closesocket(socket);
#else
	close(socket);
#endif
}

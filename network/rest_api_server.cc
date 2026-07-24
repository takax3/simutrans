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
#include "../dataobj/environment.h"
#include "../dataobj/schedule.h"
#include "../dataobj/translator.h"
#include "../bauer/goods_manager.h"
#include "../boden/grund.h"
#include "../boden/wege/weg.h"
#include "../descriptor/ground_desc.h"
#include "../linehandle_t.h"
#include "../network/network_socket_list.h"
#include "../network/pakset_info.h"
#include "../player/simplay.h"
#include "../player/finance.h"
#include "../obj/roadsign.h"
#include "../simcity.h"
#include "../simconvoi.h"
#include "../simconst.h"
#include "../simdebug.h"
#include "../simhalt.h"
#include "../simline.h"
#include "../simplan.h"
#include "../simunits.h"
#include "../simversion.h"
#include "../simworld.h"
#include "../sys/simsys.h"
#include "../utils/simstring.h"
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

struct way_filter_t {
	waytype_filter_t waytype;
	bool has_bounds;
	sint16 min_x;
	sint16 min_y;
	sint16 max_x;
	sint16 max_y;

	way_filter_t() : has_bounds(false), min_x(0), min_y(0), max_x(0), max_y(0) {}
};

struct stop_tile_filter_t {
	bool has_company_id;
	uint8 company_id;
	bool has_bounds;
	sint16 min_x;
	sint16 min_y;
	sint16 max_x;
	sint16 max_y;

	stop_tile_filter_t() : has_company_id(false), company_id(0), has_bounds(false), min_x(0), min_y(0), max_x(0), max_y(0) {}
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

static bool parse_resource_id(const std::string &value, uint32 &id)
{
	if (value.empty()) {
		return false;
	}
	uint64 parsed = 0;
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] < '0' || value[i] > '9') {
			return false;
		}
		parsed = parsed * 10 + static_cast<unsigned int>(value[i] - '0');
		if (parsed > 0xFFFFFFFFu) {
			return false;
		}
	}
	id = static_cast<uint32>(parsed);
	return id != 0;
}

static bool parse_stop_query(const std::string &query, bool &has_company_id, uint8 &company_id, std::string &error)
{
	if (query.empty()) {
		return true;
	}
	const size_t equals = query.find('=');
	if (equals == std::string::npos || query.find('&') != std::string::npos || query.substr(0, equals) != "company_id") {
		error = "unknown or malformed query parameter";
		return false;
	}
	has_company_id = true;
	if (!parse_company_id(query.substr(equals + 1), company_id)) {
		error = "invalid company_id";
		return false;
	}
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

static bool parse_map_coordinate(const std::string &value, sint16 &coordinate)
{
	if (value.empty()) return false;
	uint32 parsed = 0;
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] < '0' || value[i] > '9') return false;
		parsed = parsed * 10 + static_cast<uint32>(value[i] - '0');
		if (parsed > 32767u) return false;
	}
	coordinate = static_cast<sint16>(parsed);
	return true;
}

static bool parse_way_query(const std::string &query, const koord &map_size,
	way_filter_t &filter, std::string &error)
{
	if (query.empty()) return true;

	bool found_waytype = false;
	bool found_min_x = false;
	bool found_min_y = false;
	bool found_max_x = false;
	bool found_max_y = false;
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
			if (found_waytype) { error = "waytype may only be specified once"; return false; }
			found_waytype = true;
			if (!parse_waytype_name(value, filter.waytype)) { error = "unknown waytype"; return false; }
		}
		else {
			bool *found = NULL;
			sint16 *coordinate = NULL;
			if (name == "min_x") { found = &found_min_x; coordinate = &filter.min_x; }
			else if (name == "min_y") { found = &found_min_y; coordinate = &filter.min_y; }
			else if (name == "max_x") { found = &found_max_x; coordinate = &filter.max_x; }
			else if (name == "max_y") { found = &found_max_y; coordinate = &filter.max_y; }
			else { error = "unknown or malformed query parameter"; return false; }
			if (*found) { error = name + " may only be specified once"; return false; }
			*found = true;
			if (!parse_map_coordinate(value, *coordinate)) { error = "invalid " + name; return false; }
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}

	const unsigned int bound_count = static_cast<unsigned int>(found_min_x) + static_cast<unsigned int>(found_min_y) +
		static_cast<unsigned int>(found_max_x) + static_cast<unsigned int>(found_max_y);
	if (bound_count != 0 && bound_count != 4) {
		error = "min_x, min_y, max_x, and max_y must be specified together";
		return false;
	}
	filter.has_bounds = bound_count == 4;
	if (filter.has_bounds) {
		if (filter.min_x > filter.max_x || filter.min_y > filter.max_y) {
			error = "minimum bounds must not exceed maximum bounds";
			return false;
		}
		if (filter.max_x >= map_size.x || filter.max_y >= map_size.y) {
			error = "bounds are outside the map";
			return false;
		}
	}
	return true;
}

static bool parse_stop_tile_query(const std::string &query, const koord &map_size,
	stop_tile_filter_t &filter, std::string &error)
{
	if (query.empty()) return true;

	bool found_min_x = false;
	bool found_min_y = false;
	bool found_max_x = false;
	bool found_max_y = false;
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
		if (name == "company_id") {
			if (filter.has_company_id) { error = "company_id may only be specified once"; return false; }
			filter.has_company_id = true;
			if (!parse_company_id(value, filter.company_id)) { error = "invalid company_id"; return false; }
		}
		else {
			bool *found = NULL;
			sint16 *coordinate = NULL;
			if (name == "min_x") { found = &found_min_x; coordinate = &filter.min_x; }
			else if (name == "min_y") { found = &found_min_y; coordinate = &filter.min_y; }
			else if (name == "max_x") { found = &found_max_x; coordinate = &filter.max_x; }
			else if (name == "max_y") { found = &found_max_y; coordinate = &filter.max_y; }
			else { error = "unknown or malformed query parameter"; return false; }
			if (*found) { error = name + " may only be specified once"; return false; }
			*found = true;
			if (!parse_map_coordinate(value, *coordinate)) { error = "invalid " + name; return false; }
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}

	const unsigned int bound_count = static_cast<unsigned int>(found_min_x) + static_cast<unsigned int>(found_min_y) +
		static_cast<unsigned int>(found_max_x) + static_cast<unsigned int>(found_max_y);
	if (bound_count != 0 && bound_count != 4) {
		error = "min_x, min_y, max_x, and max_y must be specified together";
		return false;
	}
	filter.has_bounds = bound_count == 4;
	if (filter.has_bounds) {
		if (filter.min_x > filter.max_x || filter.min_y > filter.max_y) {
			error = "minimum bounds must not exceed maximum bounds";
			return false;
		}
		if (filter.max_x >= map_size.x || filter.max_y >= map_size.y) {
			error = "bounds are outside the map";
			return false;
		}
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

static bool convoy_is_waiting(const convoi_t *convoy)
{
	const int state = convoy->get_state();
	return state >= convoi_t::WAITING_FOR_CLEARANCE && state <= convoi_t::CAN_START_TWO_MONTHS && state != convoi_t::SELF_DESTRUCT;
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
	const std::string stop_prefix = "/api/v1/stops/";
	const std::string waiting_suffix = "/passenger-waiting";
	const std::string line_prefix = "/api/v1/lines/";
	const std::string schedule_suffix = "/schedule";
	uint32 resource_id = 0;
	const bool is_waiting_path = path.compare(0, stop_prefix.size(), stop_prefix) == 0 &&
		path.size() > stop_prefix.size() + waiting_suffix.size() &&
		path.substr(path.size() - waiting_suffix.size()) == waiting_suffix &&
		parse_resource_id(path.substr(stop_prefix.size(), path.size() - stop_prefix.size() - waiting_suffix.size()), resource_id);
	const bool is_schedule_path = path.compare(0, line_prefix.size(), line_prefix) == 0 &&
		path.size() > line_prefix.size() + schedule_suffix.size() &&
		path.substr(path.size() - schedule_suffix.size()) == schedule_suffix &&
		parse_resource_id(path.substr(line_prefix.size(), path.size() - line_prefix.size() - schedule_suffix.size()), resource_id);
	return path == "/" ||
		path == "/api/v1/openapi.yaml" ||
		path == "/api/v1/openapi.json" ||
		path == "/api/v1/time" ||
		path == "/api/v1/map-info" ||
		path == "/api/v1/companies" ||
		path == "/api/v1/stops" ||
		path == "/api/v1/stop-tiles" ||
		path == "/api/v1/lines" ||
		path == "/api/v1/ways" ||
		path == "/api/v1/road-signs" ||
		path == "/api/v1/way-topology" ||
		path == "/api/v1/convoys" ||
		path == "/api/v1/convoy-positions" || is_waiting_path || is_schedule_path;
}

static void append_time_json(std::ostringstream &body, karte_t *world)
{
	const settings_t &settings = world->get_settings();
	const uint32 tick = world->get_ticks();
	const uint32 ticks_per_month = world->ticks_per_world_month;
	const uint32 tick_in_month = tick % ticks_per_month;
	const uint16 diagram_ticks_per_month = settings.get_spacing_shift_divisor();
	const uint16 diagram_tick = static_cast<uint16>(
		static_cast<uint64>(tick_in_month) * diagram_ticks_per_month / ticks_per_month);

	// Keep this conversion identical to the OTRP time displayed in the status bar.
	const uint32 seconds_per_diagram_tick = 86400 / diagram_ticks_per_month;
	uint32 diagram_seconds = static_cast<uint32>(diagram_tick) * seconds_per_diagram_tick;
	const uint32 diagram_hour = diagram_seconds / 3600;
	diagram_seconds -= diagram_hour * 3600;
	const uint32 diagram_minute = diagram_seconds / 60;
	const uint32 diagram_second = diagram_seconds % 60;
	char diagram_time[16];
	snprintf(diagram_time, sizeof(diagram_time), "%02u:%02u:%02u",
		diagram_hour, diagram_minute, diagram_second);

	const uint32 current_year_month = world->get_current_month();
	body << "{\"year\":" << current_year_month / 12
		<< ",\"month\":" << current_year_month % 12 + 1
		<< ",\"tick\":" << tick
		<< ",\"ticks_per_month\":" << ticks_per_month
		<< ",\"tick_in_month\":" << tick_in_month
		<< ",\"diagram_tick\":" << diagram_tick
		<< ",\"diagram_ticks_per_month\":" << diagram_ticks_per_month
		<< ",\"diagram_time\":\"" << diagram_time << '"'
		<< ",\"paused\":" << (world->is_paused() ? "true" : "false")
		<< ",\"time_multiplier\":" << world->get_time_multiplier() << '}';
}

static std::string make_time_json(karte_t *world, uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at
		<< ",\"time\":";
	append_time_json(body, world);
	body << "}\n";
	return body.str();
}

static std::string make_map_info_json(karte_t *world, uint64 epoch, uint64 sequence, uint32 generated_at)
{
	const settings_t &settings = world->get_settings();

	uint64 citizens = 0;
	FOR(weighted_vector_tpl<stadt_t *>, const city, world->get_cities()) {
		citizens += city->get_einwohner();
	}

	uint32 company_count = 0;
	uint32 locked_company_count = 0;
	for (uint8 i = 0; i < MAX_PLAYER_COUNT; ++i) {
		if (player_t *company = world->get_player(i)) {
			++company_count;
			if (!company->access_password_hash().empty()) {
				++locked_company_count;
			}
		}
	}

	std::string pak_name;
	const char *copyright = ground_desc_t::outside != NULL ? ground_desc_t::outside->get_copyright() : NULL;
	if (copyright != NULL && STRICMP("none", copyright) != 0) {
		pak_name = copyright;
	}
	else {
		pak_name = env_t::objfilename;
		if (!pak_name.empty()) {
			pak_name.erase(pak_name.length() - 1);
		}
	}

	const int language_id = settings.get_name_language_id();
	const char *name_language = translator::get_langs()[language_id].iso;

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at
		<< ",\"time\":";
	append_time_json(body, world);
	body << ",\"size\":{\"width\":" << world->get_size().x
		<< ",\"height\":" << world->get_size().y << '}'
		<< ",\"settings\":{\"freeplay\":" << (settings.is_freeplay() ? "true" : "false")
		<< ",\"timeline_enabled\":" << (world->get_timeline_year_month() != 0 ? "true" : "false")
		<< ",\"bits_per_month\":" << settings.get_bits_per_month()
		<< ",\"name_language\":\"" << json_escape(name_language) << "\"}"
		<< ",\"counts\":{\"towns\":" << world->get_cities().get_count()
		<< ",\"citizens\":" << citizens
		<< ",\"factories\":" << world->get_fab_list().get_count()
		<< ",\"tourist_attractions\":" << world->get_attractions().get_count()
		<< ",\"convoys\":" << world->convoys().get_count()
		<< ",\"stops\":" << haltestelle_t::get_alle_haltestellen().get_count()
		<< ",\"companies\":" << company_count
		<< ",\"locked_companies\":" << locked_company_count
		<< ",\"playing_clients\":" << socket_list_t::get_playing_clients() << '}'
		<< ",\"compatibility\":{\"engine_revision\":"
		<< OTRP_VERSION_MAJOR * 10000 + OTRP_VERSION_MINOR * 100 + OTRP_VERSION_PATCH
		<< ",\"otrp_version\":\"" QUOTEME(OTRP_VERSION_MAJOR) OTRP_VERSION_MINOR_STRING "\""
		<< ",\"pak_name\":\"" << json_escape(pak_name.c_str()) << '"'
		<< ",\"pakset_checksum\":\"" << pakset_info_t::get_pakset_checksum().get_str() << "\"}"
		<< ",\"server\":{\"network_mode\":" << (env_t::networkmode ? "true" : "false")
		<< ",\"server_mode\":" << (env_t::server ? "true" : "false")
		<< ",\"name\":\"" << json_escape(env_t::server_name.c_str()) << '"'
		<< ",\"comments\":\"" << json_escape(env_t::server_comments.c_str()) << '"'
		<< ",\"pak_url\":\"" << json_escape(env_t::server_pakurl.c_str()) << '"'
		<< ",\"info_url\":\"" << json_escape(env_t::server_infurl.c_str()) << "\"}}\n";
	return body.str();
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
			<< ",\"waiting\":" << (convoy_is_waiting(convoy) ? "true" : "false")
			<< ",\"in_depot\":" << (convoy->in_depot() ? "true" : "false")
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
			<< ",\"current_cash\":" << company->get_finance()->get_account_balance()
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

static void collect_stop_allowed_company_ids(karte_t *world, const haltestelle_t *stop,
	bool allowed_company_ids[MAX_PLAYER_COUNT])
{
	for (uint8 i = 0; i < MAX_PLAYER_COUNT; ++i) {
		const player_t *company = world->get_player(i);
		allowed_company_ids[i] = company != NULL && stop->is_connection_allowed(company);
	}
}

struct halt_id_less_t {
	bool operator()(const halthandle_t &a, const halthandle_t &b) const { return a.get_id() < b.get_id(); }
};

static std::string make_stops_json(karte_t *world, bool has_company_id, uint8 company_id,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::vector<halthandle_t> stops;
	FOR(vector_tpl<halthandle_t>, stop, haltestelle_t::get_alle_haltestellen()) {
		if (!stop.is_bound()) continue;
		bool allowed_company_ids[MAX_PLAYER_COUNT];
		collect_stop_allowed_company_ids(world, stop.get_rep(), allowed_company_ids);
		if (!has_company_id || allowed_company_ids[company_id]) stops.push_back(stop);
	}
	std::sort(stops.begin(), stops.end(), halt_id_less_t());

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps() << ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at << ",\"stops\":[";
	for (size_t i = 0; i < stops.size(); ++i) {
		const haltestelle_t *stop = stops[i].get_rep();
		const koord3d pos = stop->get_basis_pos3d();
		bool allowed_company_ids[MAX_PLAYER_COUNT];
		collect_stop_allowed_company_ids(world, stop, allowed_company_ids);
		if (i) body << ',';
		body << "{\"id\":" << stops[i].get_id() << ",\"name\":\"" << json_escape(stop->get_name())
			<< "\",\"owner_company_id\":";
		if (const player_t *owner = stop->get_owner()) {
			body << static_cast<unsigned int>(owner->get_player_nr());
		}
		else {
			body << "null";
		}
		body << ",\"allowed_company_ids\":[";
		bool first_company = true;
		for (uint8 c = 0; c < MAX_PLAYER_COUNT; ++c) if (allowed_company_ids[c]) {
			if (!first_company) body << ',';
			first_company = false; body << static_cast<unsigned int>(c);
		}
		body << "],\"position\":{\"x\":" << pos.x << ",\"y\":" << pos.y << ",\"z\":" << static_cast<int>(pos.z) << '}'
			<< ",\"passenger_waiting\":" << stop->get_ware_summe(goods_manager_t::passengers)
			<< ",\"passenger_capacity\":" << stop->get_capacity(goods_manager_t::INDEX_PAS)
			<< ",\"arrived_last_month\":" << stop->get_finance_history(1, HALT_ARRIVED)
			<< ",\"departed_last_month\":" << stop->get_finance_history(1, HALT_DEPARTED) << '}';
	}
	body << "]}\n";
	return body.str();
}

struct position_less_t {
	bool operator()(const koord3d &a, const koord3d &b) const {
		if (a.x != b.x) return a.x < b.x;
		if (a.y != b.y) return a.y < b.y;
		return a.z < b.z;
	}
};

struct stop_tiles_snapshot_t {
	uint32 stop_id;
	std::vector<koord3d> tiles;
};

static std::string make_stop_tiles_json(karte_t *world, const stop_tile_filter_t &filter,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::vector<stop_tiles_snapshot_t> stops;
	FOR(vector_tpl<halthandle_t>, stop, haltestelle_t::get_alle_haltestellen()) {
		if (!stop.is_bound()) continue;
		if (filter.has_company_id) {
			bool allowed_company_ids[MAX_PLAYER_COUNT];
			collect_stop_allowed_company_ids(world, stop.get_rep(), allowed_company_ids);
			if (!allowed_company_ids[filter.company_id]) continue;
		}

		stop_tiles_snapshot_t snapshot;
		snapshot.stop_id = stop.get_id();
		FOR(slist_tpl<haltestelle_t::tile_t>, const &tile, stop->get_tiles()) {
			if (tile.grund == NULL) continue;
			const koord3d pos = tile.grund->get_pos();
			if (filter.has_bounds && (pos.x < filter.min_x || pos.x > filter.max_x ||
				pos.y < filter.min_y || pos.y > filter.max_y)) continue;
			snapshot.tiles.push_back(pos);
		}
		if (snapshot.tiles.empty()) continue;
		std::sort(snapshot.tiles.begin(), snapshot.tiles.end(), position_less_t());
		stops.push_back(snapshot);
	}
	std::sort(stops.begin(), stops.end(), [](const stop_tiles_snapshot_t &a, const stop_tiles_snapshot_t &b) {
		return a.stop_id < b.stop_id;
	});

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps() << ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at << ",\"stops\":[";
	for (size_t i = 0; i < stops.size(); ++i) {
		if (i) body << ',';
		body << "{\"stop_id\":" << stops[i].stop_id << ",\"tiles\":[";
		for (size_t j = 0; j < stops[i].tiles.size(); ++j) {
			if (j) body << ',';
			const koord3d pos = stops[i].tiles[j];
			body << "{\"x\":" << pos.x << ",\"y\":" << pos.y << ",\"z\":" << static_cast<int>(pos.z) << '}';
		}
		body << "]}";
	}
	body << "]}\n";
	return body.str();
}

struct waiting_destination_t { uint32 id; uint32 amount; };
struct waiting_destination_less_t {
	bool operator()(const waiting_destination_t &a, const waiting_destination_t &b) const {
		return a.amount != b.amount ? a.amount > b.amount : a.id < b.id;
	}
};

static std::string make_passenger_waiting_json(karte_t *world, const halthandle_t stop,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::vector<waiting_destination_t> destinations;
	FOR(vector_tpl<haltestelle_t::connection_t>, connection, stop->get_pax_connections()) {
		if (!connection.halt.is_bound()) continue;
		const uint32 amount = stop->get_ware_fuer_zwischenziel(goods_manager_t::passengers, connection.halt);
		if (amount > 0) destinations.push_back(waiting_destination_t{connection.halt.get_id(), amount});
	}
	std::sort(destinations.begin(), destinations.end(), waiting_destination_less_t());
	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch << ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence << ",\"generated_at_ms\":" << generated_at
		<< ",\"stop_id\":" << stop.get_id() << ",\"passenger_waiting\":" << stop->get_ware_summe(goods_manager_t::passengers)
		<< ",\"passenger_capacity\":" << stop->get_capacity(goods_manager_t::INDEX_PAS) << ",\"destinations\":[";
	for (size_t i = 0; i < destinations.size(); ++i) {
		if (i) body << ',';
		body << "{\"stop_id\":" << destinations[i].id << ",\"waiting\":" << destinations[i].amount << '}';
	}
	body << "]}\n";
	return body.str();
}

static halthandle_t find_stop(uint32 id)
{
	FOR(vector_tpl<halthandle_t>, stop, haltestelle_t::get_alle_haltestellen()) {
		if (stop.is_bound() && stop.get_id() == id) return stop;
	}
	return halthandle_t();
}

static linehandle_t find_line(karte_t *world, uint32 id)
{
	for (uint8 i = 0; i < MAX_PLAYER_COUNT; ++i) if (const player_t *company = world->get_player(i)) {
		FOR(vector_tpl<linehandle_t>, line, company->simlinemgmt.get_line_list()) {
			if (line.is_bound() && line.get_id() == id) return line;
		}
	}
	return linehandle_t();
}

static std::string make_line_schedule_json(karte_t *world, const linehandle_t line,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch << ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence << ",\"generated_at_ms\":" << generated_at
		<< ",\"line_id\":" << line.get_id() << ",\"entries\":[";
	const schedule_t *schedule = line->get_schedule();
	if (schedule != NULL) for (uint8 i = 0; i < schedule->get_count(); ++i) {
		const schedule_entry_t &entry = schedule->at(i);
		const halthandle_t stop = haltestelle_t::get_halt(entry.pos, line->get_owner());
		if (i) body << ',';
		body << "{\"index\":" << static_cast<unsigned int>(i) << ",\"position\":{\"x\":" << entry.pos.x
			<< ",\"y\":" << entry.pos.y << ",\"z\":" << static_cast<int>(entry.pos.z) << "},\"stop_id\":";
		if (stop.is_bound()) body << stop.get_id(); else body << "null";
		body << '}';
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

struct way_snapshot_t {
	grund_t *ground;
	weg_t *way;
};

struct way_snapshot_less_t {
	bool operator()(const way_snapshot_t &a, const way_snapshot_t &b) const {
		const koord3d a_pos = a.ground->get_pos();
		const koord3d b_pos = b.ground->get_pos();
		if (a_pos.y != b_pos.y) return a_pos.y < b_pos.y;
		if (a_pos.x != b_pos.x) return a_pos.x < b_pos.x;
		if (a_pos.z != b_pos.z) return a_pos.z < b_pos.z;
		return a.way->get_waytype() < b.way->get_waytype();
	}
};

static const char *direction_name(ribi_t::ribi direction)
{
	switch (direction) {
		case ribi_t::north: return "north";
		case ribi_t::east:  return "east";
		case ribi_t::south: return "south";
		case ribi_t::west:  return "west";
		default:            return "unknown";
	}
}

static const char *way_structure_name(const grund_t *ground)
{
	if (ground->ist_bruecke()) return "bridge";
	if (ground->ist_tunnel()) return "tunnel";
	if (ground->get_typ() == grund_t::monorailboden) return "elevated";
	return "surface";
}

static void append_direction_array(std::ostringstream &body, ribi_t::ribi directions)
{
	body << '[';
	bool first = true;
	for (uint8 i = 0; i < 4; ++i) {
		const ribi_t::ribi direction = ribi_t::nesw[i];
		if ((directions & direction) == 0) continue;
		if (!first) body << ',';
		first = false;
		body << '"' << direction_name(direction) << '"';
	}
	body << ']';
}

static std::vector<way_snapshot_t> collect_ways(karte_t *world, const way_filter_t &filter)
{
	std::vector<way_snapshot_t> ways;
	const koord size = world->get_size();
	const sint16 min_x = filter.has_bounds ? filter.min_x : 0;
	const sint16 min_y = filter.has_bounds ? filter.min_y : 0;
	const sint16 max_x = filter.has_bounds ? filter.max_x : size.x - 1;
	const sint16 max_y = filter.has_bounds ? filter.max_y : size.y - 1;
	for (sint16 y = min_y; y <= max_y; ++y) {
		for (sint16 x = min_x; x <= max_x; ++x) {
			planquadrat_t *plan = world->access(x, y);
			for (unsigned int ground_index = 0; ground_index < plan->get_boden_count(); ++ground_index) {
				grund_t *ground = plan->get_boden_bei(ground_index);
				for (uint8 way_index = 0; way_index < 2; ++way_index) {
					weg_t *way = ground->get_weg_nr(way_index);
					if (way != NULL && filter_matches(filter.waytype, way->get_waytype())) {
						ways.push_back(way_snapshot_t{ground, way});
					}
				}
			}
		}
	}
	std::sort(ways.begin(), ways.end(), way_snapshot_less_t());
	return ways;
}

static std::string make_ways_json(karte_t *world, const way_filter_t &filter,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	const std::vector<way_snapshot_t> ways = collect_ways(world, filter);

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at << ",\"ways\":[";
	for (size_t i = 0; i < ways.size(); ++i) {
		grund_t *ground = ways[i].ground;
		weg_t *way = ways[i].way;
		const koord3d pos = ground->get_pos();
		if (i) body << ',';
		body << "{\"position\":{\"x\":" << pos.x << ",\"y\":" << pos.y
			<< ",\"z\":" << static_cast<int>(pos.z) << "},\"waytype\":\""
			<< waytype_name(way->get_waytype()) << "\",\"company_id\":";
		if (const player_t *owner = way->get_owner()) body << static_cast<unsigned int>(owner->get_player_nr());
		else body << "null";
		body << ",\"descriptor_name\":\"" << json_escape(way->get_desc()->get_name()) << '"'
			<< ",\"max_speed_kmh\":" << way->get_max_speed()
			<< ",\"electrified\":" << (way->is_electrified() ? "true" : "false")
			<< ",\"structure\":\"" << way_structure_name(ground) << "\",\"physical_directions\":";
		append_direction_array(body, way->get_ribi_unmasked());
		body << ",\"blocked_directions\":";
		append_direction_array(body, way->get_ribi_masked());
		body << ",\"connections\":[";
		bool first_connection = true;
		const ribi_t::ribi physical = way->get_ribi_unmasked();
		for (uint8 direction_index = 0; direction_index < 4; ++direction_index) {
			const ribi_t::ribi direction = ribi_t::nesw[direction_index];
			if ((physical & direction) == 0) continue;
			grund_t *neighbour = NULL;
			if (!ground->get_neighbour(neighbour, way->get_waytype(), direction) || neighbour == NULL ||
				neighbour->get_weg(way->get_waytype()) == NULL) continue;
			const koord3d neighbour_pos = neighbour->get_pos();
			if (!first_connection) body << ',';
			first_connection = false;
			body << "{\"direction\":\"" << direction_name(direction) << "\",\"position\":{\"x\":"
				<< neighbour_pos.x << ",\"y\":" << neighbour_pos.y << ",\"z\":"
				<< static_cast<int>(neighbour_pos.z) << "}}";
		}
		body << "]}";
	}
	body << "]}\n";
	return body.str();
}

struct road_sign_snapshot_t {
	roadsign_t *road_sign;
};

struct road_sign_snapshot_less_t {
	bool operator()(const road_sign_snapshot_t &a, const road_sign_snapshot_t &b) const {
		const koord3d a_pos = a.road_sign->get_pos();
		const koord3d b_pos = b.road_sign->get_pos();
		if (a_pos.y != b_pos.y) return a_pos.y < b_pos.y;
		if (a_pos.x != b_pos.x) return a_pos.x < b_pos.x;
		if (a_pos.z != b_pos.z) return a_pos.z < b_pos.z;
		if (a.road_sign->get_waytype() != b.road_sign->get_waytype()) return a.road_sign->get_waytype() < b.road_sign->get_waytype();
		const roadsign_desc_t *a_desc = a.road_sign->get_desc();
		const roadsign_desc_t *b_desc = b.road_sign->get_desc();
		return strcmp(a_desc != NULL ? a_desc->get_name() : "", b_desc != NULL ? b_desc->get_name() : "") < 0;
	}
};

static const char *road_sign_kind(const roadsign_desc_t *desc)
{
	if (desc == NULL) return "other";
	if (desc->is_pre_signal()) return "pre_signal";
	if (desc->is_priority_signal()) return "priority_signal";
	if (desc->is_longblock_signal()) return "longblock_signal";
	if (desc->is_signal_type() && desc->is_choose_sign()) return "choose_signal";
	if (desc->is_signal_type()) return "signal";
	if (desc->is_traffic_light()) return "traffic_light";
	if (desc->is_end_choose_signal()) return "end_of_choose";
	if (desc->is_private_way()) return "private_road";
	if (desc->is_single_way()) return "one_way";
	return "other";
}

static const char *road_sign_state(roadsign_t *road_sign)
{
	switch (road_sign->get_state()) {
		case roadsign_t::STATE_RED: return "red";
		case roadsign_t::STATE_GREEN: return "green";
		case roadsign_t::STATE_YELLOW: return "yellow";
	}
	return "red";
}

static std::string make_road_signs_json(karte_t *world, const way_filter_t &filter,
	uint64 epoch, uint64 sequence, uint32 generated_at)
{
	std::vector<road_sign_snapshot_t> road_signs;
	const koord size = world->get_size();
	const sint16 min_x = filter.has_bounds ? filter.min_x : 0;
	const sint16 min_y = filter.has_bounds ? filter.min_y : 0;
	const sint16 max_x = filter.has_bounds ? filter.max_x : size.x - 1;
	const sint16 max_y = filter.has_bounds ? filter.max_y : size.y - 1;
	for (sint16 y = min_y; y <= max_y; ++y) {
		for (sint16 x = min_x; x <= max_x; ++x) {
			planquadrat_t *plan = world->access(x, y);
			for (unsigned int ground_index = 0; ground_index < plan->get_boden_count(); ++ground_index) {
				grund_t *ground = plan->get_boden_bei(ground_index);
				for (uint8 object_index = 0; object_index < ground->get_top(); ++object_index) {
					obj_t *object = ground->obj_bei(object_index);
					if (object == NULL || (object->get_typ() != obj_t::signal && object->get_typ() != obj_t::roadsign)) continue;
					roadsign_t *road_sign = static_cast<roadsign_t *>(object);
					if (filter_matches(filter.waytype, road_sign->get_waytype())) {
						road_signs.push_back(road_sign_snapshot_t{road_sign});
					}
				}
			}
		}
	}
	std::sort(road_signs.begin(), road_signs.end(), road_sign_snapshot_less_t());

	std::ostringstream body;
	body << "{\"api_version\":\"v1\",\"world_epoch\":" << epoch
		<< ",\"sync_step\":" << world->get_sync_steps()
		<< ",\"snapshot_sequence\":" << sequence
		<< ",\"generated_at_ms\":" << generated_at << ",\"road_signs\":[";
	for (size_t i = 0; i < road_signs.size(); ++i) {
		roadsign_t *road_sign = road_signs[i].road_sign;
		const roadsign_desc_t *desc = road_sign->get_desc();
		const koord3d pos = road_sign->get_pos();
		if (i) body << ',';
		body << "{\"position\":{\"x\":" << pos.x << ",\"y\":" << pos.y
			<< ",\"z\":" << static_cast<int>(pos.z) << "},\"waytype\":\""
			<< waytype_name(road_sign->get_waytype()) << "\",\"kind\":\"" << road_sign_kind(desc)
			<< "\",\"directions\":";
		append_direction_array(body, road_sign->get_dir());
		body << ",\"state\":";
		if (desc != NULL && (desc->is_signal_type() || desc->is_traffic_light())) body << '"' << road_sign_state(road_sign) << '"';
		else body << "null";
		body << ",\"company_id\":";
		if (const player_t *owner = road_sign->get_owner()) body << static_cast<unsigned int>(owner->get_player_nr());
		else body << "null";
		body << ",\"descriptor_name\":\"" << json_escape(desc != NULL ? desc->get_name() : "") << "\"}";
	}
	body << "]}\n";
	return body.str();
}

static std::string make_way_topology_csv(karte_t *world, const way_filter_t &filter)
{
	const std::vector<way_snapshot_t> ways = collect_ways(world, filter);
	std::ostringstream body;
	body << "x,y,z,waytype,physical_ribi,blocked_ribi,north_z,east_z,south_z,west_z\r\n";
	for (size_t i = 0; i < ways.size(); ++i) {
		grund_t *ground = ways[i].ground;
		weg_t *way = ways[i].way;
		const koord3d pos = ground->get_pos();
		body << pos.x << ',' << pos.y << ',' << static_cast<int>(pos.z) << ','
			<< waytype_name(way->get_waytype()) << ','
			<< static_cast<unsigned int>(way->get_ribi_unmasked()) << ','
			<< static_cast<unsigned int>(way->get_ribi_masked());

		const ribi_t::ribi physical = way->get_ribi_unmasked();
		for (uint8 direction_index = 0; direction_index < 4; ++direction_index) {
			const ribi_t::ribi direction = ribi_t::nesw[direction_index];
			body << ',';
			if ((physical & direction) == 0) continue;
			grund_t *neighbour = NULL;
			if (ground->get_neighbour(neighbour, way->get_waytype(), direction) && neighbour != NULL &&
				neighbour->get_weg(way->get_waytype()) != NULL) {
				body << static_cast<int>(neighbour->get_pos().z);
			}
		}
		body << "\r\n";
	}
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
			"\"endpoints\":{\"time\":\"/api/v1/time\",\"map_info\":\"/api/v1/map-info\",\"companies\":\"/api/v1/companies\",\"stops\":\"/api/v1/stops\",\"stop_tiles\":\"/api/v1/stop-tiles\",\"lines\":\"/api/v1/lines\",\"ways\":\"/api/v1/ways\",\"road_signs\":\"/api/v1/road-signs\",\"way_topology\":\"/api/v1/way-topology\","
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

	const bool is_time = path == "/api/v1/time";
	const bool is_map_info = path == "/api/v1/map-info";
	const bool is_companies = path == "/api/v1/companies";
	const bool is_stops = path == "/api/v1/stops";
	const bool is_stop_tiles = path == "/api/v1/stop-tiles";
	const bool is_lines = path == "/api/v1/lines";
	const bool is_ways = path == "/api/v1/ways";
	const bool is_road_signs = path == "/api/v1/road-signs";
	const bool is_way_topology = path == "/api/v1/way-topology";
	const bool is_convoys = path == "/api/v1/convoys";
	const bool is_positions = path == "/api/v1/convoy-positions";
	uint32 stop_id = 0;
	uint32 schedule_line_id = 0;
	const std::string stop_prefix = "/api/v1/stops/";
	const std::string waiting_suffix = "/passenger-waiting";
	const bool is_passenger_waiting = path.compare(0, stop_prefix.size(), stop_prefix) == 0 &&
		path.size() > stop_prefix.size() + waiting_suffix.size() &&
		path.substr(path.size() - waiting_suffix.size()) == waiting_suffix &&
		parse_resource_id(path.substr(stop_prefix.size(), path.size() - stop_prefix.size() - waiting_suffix.size()), stop_id);
	const std::string line_prefix = "/api/v1/lines/";
	const std::string schedule_suffix = "/schedule";
	const bool is_line_schedule = path.compare(0, line_prefix.size(), line_prefix) == 0 &&
		path.size() > line_prefix.size() + schedule_suffix.size() &&
		path.substr(path.size() - schedule_suffix.size()) == schedule_suffix &&
		parse_resource_id(path.substr(line_prefix.size(), path.size() - line_prefix.size() - schedule_suffix.size()), schedule_line_id);
	if (!is_time && !is_map_info && !is_companies && !is_stops && !is_stop_tiles && !is_lines && !is_line_schedule && !is_ways && !is_road_signs && !is_way_topology && !is_convoys && !is_positions && !is_passenger_waiting) {
		connection->send_buf = make_error_response(404, "Not Found", "unknown API path");
		return;
	}

	waytype_filter_t waytype_filter;
	line_filter_t line_filter;
	way_filter_t way_filter;
	stop_tile_filter_t stop_tile_filter;
	bool has_stop_company_id = false;
	uint8 stop_company_id = 0;
	std::string query_error;
	if ((is_time || is_map_info || is_companies || is_passenger_waiting || is_line_schedule) && !query.empty()) {
		connection->send_buf = make_error_response(400, "Bad Request", "query parameters are not supported");
		return;
	}
	if (is_lines && !parse_line_query(query, line_filter, query_error)) {
		connection->send_buf = make_error_response(400, "Bad Request", query_error);
		return;
	}
	if (is_stops && !parse_stop_query(query, has_stop_company_id, stop_company_id, query_error)) {
		connection->send_buf = make_error_response(400, "Bad Request", query_error);
		return;
	}
	if (is_stop_tiles && !parse_stop_tile_query(query, world != NULL ? world->get_size() : koord(32767, 32767), stop_tile_filter, query_error)) {
		connection->send_buf = make_error_response(400, "Bad Request", query_error);
		return;
	}
	if ((is_ways || is_road_signs || is_way_topology) && !parse_way_query(query, world != NULL ? world->get_size() : koord(32767, 32767), way_filter, query_error)) {
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
	if (((is_ways || is_road_signs || is_way_topology) && way_filter.has_bounds &&
		(way_filter.max_x >= world->get_size().x || way_filter.max_y >= world->get_size().y)) ||
		(is_stop_tiles && stop_tile_filter.has_bounds &&
		(stop_tile_filter.max_x >= world->get_size().x || stop_tile_filter.max_y >= world->get_size().y))) {
		connection->send_buf = make_error_response(400, "Bad Request", "bounds are outside the map");
		return;
	}

	const uint64 sequence = ++snapshot_sequence;
	const uint32 generated_at = dr_time();
	const uint32 sync_step = world->get_sync_steps();
	const std::string headers = metadata_headers(world_epoch, sequence, sync_step, generated_at);

	if (is_time) {
		const std::string body = make_time_json(world, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_map_info) {
		const std::string body = make_map_info_json(world, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_companies) {
		const std::string body = make_companies_json(world, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_stops) {
		const std::string body = make_stops_json(world, has_stop_company_id, stop_company_id, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_stop_tiles) {
		const std::string body = make_stop_tiles_json(world, stop_tile_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_lines) {
		const std::string body = make_lines_json(world, line_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_line_schedule) {
		const linehandle_t line = find_line(world, schedule_line_id);
		if (!line.is_bound()) connection->send_buf = make_error_response(404, "Not Found", "line not found");
		else connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8",
			make_line_schedule_json(world, line, world_epoch, sequence, generated_at), headers);
	}
	else if (is_ways) {
		const std::string body = make_ways_json(world, way_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_road_signs) {
		const std::string body = make_road_signs_json(world, way_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_way_topology) {
		const std::string body = make_way_topology_csv(world, way_filter);
		connection->send_buf = make_http_response(200, "OK", "text/csv; charset=utf-8", body, headers);
	}
	else if (is_convoys) {
		const std::string body = make_convoys_json(world, waytype_filter, world_epoch, sequence, generated_at);
		connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8", body, headers);
	}
	else if (is_positions) {
		const std::string body = make_positions_csv(world, waytype_filter);
		connection->send_buf = make_http_response(200, "OK", "text/csv; charset=utf-8", body, headers);
	}
	else {
		const halthandle_t stop = find_stop(stop_id);
		if (!stop.is_bound()) connection->send_buf = make_error_response(404, "Not Found", "stop not found");
		else connection->send_buf = make_http_response(200, "OK", "application/json; charset=utf-8",
			make_passenger_waiting_json(world, stop, world_epoch, sequence, generated_at), headers);
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

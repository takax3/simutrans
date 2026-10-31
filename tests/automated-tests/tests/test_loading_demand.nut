// This file is part of the Simutrans project under the Artistic License.
// (see LICENSE.txt)

function demand_wait_until(ticks)
{
	while (world.get_time().ticks < ticks) { sleep() }
}

// A (12): dispatch centre, B (7): collection, C (2): delivery.
// C holds the bus after the first trip so departure is observable.
function demand_fixture(flags = 1 << 24, load = 0, wait = 0, middle_flags = 0, timed = false, tolerance = 0)
{
	local pl = player_x(0)
	ASSERT_TRUE(settings.set_time_based_routing_enabled(good_desc_x.passenger, false))
	ASSERT_EQUAL(command_x(tool_build_way).work(pl, coord3d(4, 2, 0), coord3d(4, 14, 0), "cobblestone_road"), null)
	foreach (y in [2, 7, 12]) {
		ASSERT_EQUAL(command_x(tool_build_station).work(pl, coord3d(4, y, 0), "BusStop"), null)
	}
	ASSERT_EQUAL(command_x.build_depot(pl, coord3d(4, 14, 0), building_desc_x("CarDepot")), null)
	local depot = depot_x(4, 14, 0)
	depot.append_vehicle(pl, convoy_x(0), vehicle_desc_x("Buessig"))
	local cnv = depot.get_convoy_list()[0]
	local sched = schedule_x(wt_road, [
		schedule_entry_x(coord3d(4, 12, 0), load, wait),
		schedule_entry_x(coord3d(4, 7, 0), 0, 0),
		schedule_entry_x(coord3d(4, 2, 0), 100, 0),
	])
	sched.entries[0].flags = flags | (timed ? 16 : 0)
	sched.entries[0].spacing = 4
	sched.entries[0].spacing_shift = 90
	sched.entries[0].delay_tolerance = tolerance
	sched.entries[1].flags = middle_flags
	cnv.change_schedule(pl, sched)
	ASSERT_EQUAL(cnv.get_schedule().entries[0].flags, flags | (timed ? 16 : 0))
	debug.set_game_speed(100)
	return { cnv = cnv, depot = depot, month = world.get_time().ticks_per_month }
}

function demand_start(f)
{
	f.depot.start_convoy(player_x(0), f.cnv)
	local deadline = world.get_time().ticks + f.month
	while (!f.cnv.is_loading()) {
		ASSERT_LESS(world.get_time().ticks, deadline)
		sleep()
	}
}

function demand_assert_waiting(f)
{
	demand_wait_until(world.get_time().ticks + f.month / 8)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	ASSERT_TRUE(f.cnv.is_loading())
}

function demand_assert_departs(f)
{
	local deadline = world.get_time().ticks + f.month / 64
	while (f.cnv.get_schedule().current == 0) {
		ASSERT_LESS(world.get_time().ticks, deadline)
		sleep()
	}
}

function demand_generate(from_y, to_y)
{
	print("Demand passengers " + from_y + " -> " + to_y)
	ASSERT_EQUAL(world.generate_goods(coord(3, from_y), coord(3, to_y), good_desc_x.passenger, 100), 1)
}

function test_loading_demand_pickup()
{
	local f = demand_fixture()
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(7, 2)
	demand_assert_departs(f)
}

function test_loading_demand_off()
{
	local f = demand_fixture(0)
	demand_start(f)
	demand_assert_departs(f)
}

function test_loading_demand_unload()
{
	local f = demand_fixture(1 << 24, 50)
	demand_start(f)
	demand_generate(12, 7)
	demand_assert_departs(f)
}

function test_loading_demand_skip_chain()
{
	local f = demand_fixture(1 << 24, 0, 0, 1 << 15)
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(2, 12)
	demand_assert_departs(f)
}

function test_loading_demand_blocking_stop()
{
	local f = demand_fixture()
	demand_start(f)
	demand_generate(2, 12)
	demand_assert_waiting(f)
}

function demand_other_service(f)
{
	f.depot.append_vehicle(player_x(0), convoy_x(0), vehicle_desc_x("Buessig"))
	local other = f.depot.get_convoy_list()[1]
	other.change_schedule(player_x(0), schedule_x(wt_road, [
		schedule_entry_x(coord3d(4, 7, 0), 100, 0),
		schedule_entry_x(coord3d(4, 2, 0), 0, 0)]))
	// Keep the other service in the depot: its route supplies the loading connection.
}

function test_loading_demand_no_load()
{
	local f = demand_fixture(1 << 24, 0, 0, 4)
	demand_other_service(f)
	demand_start(f)
	demand_generate(7, 2)
	demand_assert_waiting(f)
}

function test_loading_demand_no_unload()
{
	local f = demand_fixture(1 << 24, 50, 0, 8)
	demand_start(f)
	demand_generate(12, 2)
	demand_assert_waiting(f)
	ASSERT_TRUE(f.cnv.get_loading_level() >= 50)
}

function test_loading_demand_unload_all()
{
	local f = demand_fixture(1 << 24, 50, 0, 32)
	demand_start(f)
	demand_generate(12, 2)
	demand_assert_departs(f)
}

function test_loading_demand_loading_limit()
{
	local f = demand_fixture(1 << 24, 50)
	demand_start(f)
	demand_generate(7, 2)
	demand_assert_waiting(f)
}

function test_loading_demand_timeout()
{
	local f = demand_fixture(1 << 24, 50, 8)
	demand_start(f)
	demand_wait_until(world.get_time().ticks + f.month / 4)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	demand_generate(7, 2)
	// No new maximum-wait timer may be started when demand appears.
	demand_assert_departs(f)
}

function demand_set_schedule(f, entries)
{
	local s = schedule_x(wt_road, entries)
	s.entries[0].flags = 1 << 24
	f.cnv.change_schedule(player_x(0), s)
}

function test_loading_demand_waypoint()
{
	local f = demand_fixture()
	demand_set_schedule(f, [schedule_entry_x(coord3d(4, 12, 0), 0, 0),
		schedule_entry_x(coord3d(4, 10, 0), 0, 0),
		schedule_entry_x(coord3d(4, 7, 0), 0, 0),
		schedule_entry_x(coord3d(4, 2, 0), 100, 0)])
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(7, 2)
	demand_assert_departs(f)
}

function test_loading_demand_repeated_stop()
{
	local f = demand_fixture()
	local s = schedule_x(wt_road, [schedule_entry_x(coord3d(4, 12, 0), 0, 0),
		schedule_entry_x(coord3d(4, 12, 0), 0, 0),
		schedule_entry_x(coord3d(4, 7, 0), 0, 0),
		schedule_entry_x(coord3d(4, 2, 0), 100, 0)])
	s.entries[0].flags = 1 << 24
	s.entries[1].flags = 1 << 15
	f.cnv.change_schedule(player_x(0), s)
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(7, 2)
	demand_assert_departs(f)
}

function test_loading_demand_pass_stop()
{
	local f = demand_fixture(1 << 24, 0, 0, (1 << 14) | 4 | 8)
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(2, 12)
	demand_assert_departs(f)
}

function test_loading_demand_depot()
{
	local f = demand_fixture()
	demand_set_schedule(f, [schedule_entry_x(coord3d(4, 12, 0), 0, 0),
		schedule_entry_x(coord3d(4, 14, 0), 0, 0)])
	f.depot.start_convoy(player_x(0), f.cnv)
	ASSERT_TRUE(!f.cnv.is_in_depot())
	demand_wait_until(world.get_time().ticks + f.month / 8)
	ASSERT_TRUE(f.cnv.is_in_depot())
}

function test_loading_demand_full_scan()
{
	local f = demand_fixture(1 << 24, 0, 0, 1 << 15)
	local s = f.cnv.get_schedule()
	s.entries[2].flags = 1 << 15
	f.cnv.change_schedule(player_x(0), s)
	demand_start(f)
	demand_assert_waiting(f)
}

function test_loading_demand_slot_within_tolerance()
{
	local f = demand_fixture(1 << 24, 0, 0, 0, true, 90)
	demand_start(f)
	demand_assert_waiting(f)
	local interval = f.month / 4
	local shift = f.month / 16
	local slot = ((world.get_time().ticks - shift) / interval + 1) * interval + shift
	demand_wait_until(slot + f.month / 32)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	demand_generate(7, 2)
	demand_assert_departs(f)
}

function demand_expired_slot_case(tolerance)
{
	local f = demand_fixture(1 << 24, 0, 0, 0, true, tolerance)
	demand_start(f)
	local interval = f.month / 4
	local shift = f.month / 16
	// Let demand-free slots expire, then introduce demand just after an expiry.
	local slot = (world.get_time().ticks / f.month + 2) * f.month + shift
	local expiry = slot + f.month * tolerance / 1440
	demand_wait_until(expiry + f.month / 64)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	demand_generate(7, 2)
	if (tolerance == 0) {
		demand_wait_until(slot + interval / 2)
		ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
		demand_wait_until(slot + interval + f.month / 32)
		ASSERT_TRUE(f.cnv.get_schedule().current != 0)
	}
	else {
		// With overlapping tolerance windows a later slot is already eligible.
		demand_assert_departs(f)
	}
}

function test_loading_demand_slot_zero_tolerance()
{
	demand_expired_slot_case(0)
}

function test_loading_demand_slot_long_tolerance()
{
	demand_expired_slot_case(720)
}

function test_loading_demand_slot_after_expiry()
{
	local f = demand_fixture(1 << 24, 0, 0, 0, true, 90)
	demand_start(f)
	local interval = f.month / 4
	local shift = f.month / 16
	local slot = (world.get_time().ticks / f.month + 2) * f.month + shift
	demand_wait_until(slot + f.month / 16 + f.month / 64)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	demand_generate(7, 2)
	demand_wait_until(slot + interval - f.month / 32)
	ASSERT_EQUAL(f.cnv.get_schedule().current, 0)
	demand_wait_until(slot + interval + f.month / 32)
	ASSERT_TRUE(f.cnv.get_schedule().current != 0)
}

function test_loading_demand_full_convoy_pickup()
{
	// A second service supplies B's routing connection while the tested bus
	// suppresses unloading at B. Only the tested bus leaves the depot.
	local f = demand_fixture(1 << 24, 100, 0, 8)
	demand_other_service(f)
	demand_start(f)
	demand_generate(7, 2)
	demand_assert_waiting(f)
	demand_generate(12, 2)
	demand_assert_departs(f)
	ASSERT_EQUAL(f.cnv.get_loading_level(), 100)
}

function test_loading_demand_slot_conflict()
{
	// Reuse the existing two-platform, shared-slot-group fixture.
	include("tests/test_loading_departure_slot")
	local f = loading_slot_fixture(0, 0, 4, 90, 90, true, 1 << 24)
	local month = world.get_time().ticks_per_month
	local interval = month / 4
	local shift = month / 16
	foreach (depot in f.depots) { depot.start_all_convoys(player_x(0)) }
	foreach (cnv in f.convoys) { loading_slot_wait_for_stop(cnv) }
	local slot = (world.get_time().ticks / month + 2) * month + shift
	loading_slot_wait_until(slot + month / 32)
	foreach (cnv in f.convoys) { ASSERT_EQUAL(cnv.get_schedule().current, 0) }
	ASSERT_EQUAL(world.generate_goods(coord(3, 7), coord(3, 2), good_desc_x.passenger, 200), 1)
	loading_slot_wait_until(slot + month / 16 - 4096)
	local departed = 0
	foreach (cnv in f.convoys) { if (cnv.get_schedule().current == 1) { departed++ } }
	ASSERT_EQUAL(departed, 1)
	loading_slot_wait_until(slot + interval + month / 32)
	foreach (cnv in f.convoys) { ASSERT_EQUAL(cnv.get_schedule().current, 1) }
}
function test_loading_demand_pickup_at_skippable_stop()
{
	local f = demand_fixture(1 << 24, 0, 0, 1 << 15)
	demand_start(f)
	demand_assert_waiting(f)
	demand_generate(7, 2)
	demand_wait_until(world.get_time().ticks + f.month / 16)
	// Checking demand at A must not make B look empty to the station-skip check.
	ASSERT_TRUE(f.cnv.get_transported_goods()[0] > 0)
}

function test_loading_demand_existing_goods_second_convoy()
{
	include("tests/test_loading_departure_slot")
	local f = loading_slot_fixture(0, 0, 4, 90, 90, true, 1 << 24)
	local month = world.get_time().ticks_per_month
	local interval = month / 4
	local shift = month / 16
	foreach (depot in f.depots) { depot.start_all_convoys(player_x(0)) }
	foreach (cnv in f.convoys) { loading_slot_wait_for_stop(cnv) }
	local slot = (world.get_time().ticks / month + 2) * month + shift
	loading_slot_wait_until(slot + month / 32)
	// Goods wait at the next stop, enough for both buses. The first demand query
	// and the first bus's loading statistics must not hide the remaining goods.
	ASSERT_EQUAL(world.generate_goods(coord(3, 2), coord(3, 7), good_desc_x.passenger, 200), 1)
	loading_slot_wait_until(slot + interval + month / 16)
	foreach (cnv in f.convoys) { ASSERT_TRUE(cnv.get_transported_goods()[0] > 0) }
}

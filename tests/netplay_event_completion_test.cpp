#include "netplay_event_completion.h"
#include "netplay_wire.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } \
} while (false)

int main() {
    using namespace netplay;
    constexpr uint64_t second = 1000000;
    const EventFinishState active = EventFinishState::Active;
    const EventFinishState finished = EventFinishState::Finished;
    const EventFinishState totalled = EventFinishState::Totalled;
    const EventFinishState timed_out = EventFinishState::TimedOut;

    CHECK(event_completion_mode(0x2110) == EventCompletionMode::Race);
    CHECK(event_completion_mode(0x210e) == EventCompletionMode::Race);
    CHECK(event_completion_mode(0x1002105) == EventCompletionMode::Race);
    CHECK(event_completion_mode(0x100210f) == EventCompletionMode::Race);
    CHECK(event_completion_mode(0x1880221e) == EventCompletionMode::Derby);
    CHECK(event_completion_mode(0x18000224) == EventCompletionMode::Battle);
    CHECK(event_completion_mode(0x2b02) == EventCompletionMode::Native);

    EventCompletion race;
    race.reset(EventCompletionMode::Race, 0b0111);
    race.record(2, totalled, second);
    CHECK(!race.update(second));
    CHECK(race.deadline() == 0);

    race.record(1, finished, 2 * second);
    CHECK(race.deadline() == 32 * second);
    CHECK(!race.update(2 * second));
    CHECK(race.state(0) == active);
    race.record(1, totalled, 3 * second);
    CHECK(race.state(1) == finished);

    race.record(0, finished, 30 * second);
    CHECK(race.update(30 * second));
    CHECK(!race.update(31 * second));
    CHECK(race.rank(1) == 1);
    CHECK(race.rank(0) == 2);
    CHECK(race.rank(2) == 0);
    CHECK(race.state(2) == totalled);

    race.reset(EventCompletionMode::Race, 0b0111);
    race.record(0, finished, second);
    race.record(2, totalled, 2 * second);
    race.record(1, finished, 31 * second);
    CHECK(!race.update(31 * second));
    CHECK(race.update(31 * second + EventCompletion::kReportSettleUs));
    CHECK(race.state(1) == timed_out);

    // The guest crossed just before 0:00. Its first three 100 ms reports
    // were lost; the fourth arrived over the long side of a 150 ms path.
    race.reset(EventCompletionMode::Race, 0b11);
    race.record(0, finished, second);
    CHECK(!race.update(31 * second + 400000));
    race.record(1, finished, 31 * second + 450000, 31 * second - 1000);
    CHECK(race.update(31 * second + 450000));
    CHECK(race.rank(1) == 2);

    race.reset(EventCompletionMode::Race, 0b0011);
    race.record(0, totalled, second);
    race.record(1, totalled, second);
    CHECK(race.update(second));

    race.reset(EventCompletionMode::Race, 0b1111);
    race.record(2, finished, 2 * second, second + 8000);
    race.record(1, finished, 2 * second + 100, second + 2000);
    CHECK(race.deadline() == 31 * second + 2000);
    race.record(0, EventFinishState::Withdrawn, 3 * second);
    race.record(3, finished, 31 * second + 50000, 31 * second - 10000);
    CHECK(race.update(31 * second + 50000));
    CHECK(race.rank(1) == 1 && race.rank(2) == 2 && race.rank(3) == 3);
    CHECK(race.rank(0) == 0 && race.state(0) == EventFinishState::Withdrawn);

    // A delayed earlier crossing can shorten a live countdown. A crossing
    // inside the old window but outside the corrected one is a timeout.
    race.reset(EventCompletionMode::Race, 0b0111);
    race.record(0, finished, 2 * second);
    race.record(1, finished, 31 * second + 50000);
    race.record(2, finished, 31 * second + 100000, second);
    CHECK(race.update(31 * second + 100000));
    CHECK(race.rank(2) == 1 && race.rank(0) == 2);
    CHECK(race.state(1) == timed_out && race.rank(1) == 0);

    // Reports for the same wire-clock millisecond tie by grid slot. Packet
    // receipt microseconds and the host's more precise timer cannot break it.
    race.reset(EventCompletionMode::Race, 0b0011);
    race.record(1, finished, 2 * second, second + 100);
    race.record(0, finished, 3 * second, second + 900);
    CHECK(race.update(3 * second));
    CHECK(race.rank(0) == 1 && race.rank(1) == 2);

    EventHostClock clock;
    clock.anchor(55, 5);
    CHECK(clock.host_ms(10) == 60);
    clock.observe(0xfffffff0u, 0x10, 100);
    CHECK(clock.host_ms(0) == 100);
    clock.observe(18, 750, 999);
    CHECK(clock.host_ms(10) == 110);

    EventCompletion derby;
    derby.reset(EventCompletionMode::Derby, 0b0111);
    derby.record(2, totalled, second);
    CHECK(!derby.update(second));
    derby.record(0, totalled, 2 * second);
    CHECK(!derby.update(2 * second));
    CHECK(derby.update(2 * second + 250000));
    CHECK(derby.state(1) == EventFinishState::Survivor);
    CHECK(derby.rank(1) == 1);
    CHECK(derby.state(0) == totalled);

    derby.reset(EventCompletionMode::Derby, 0b0011);
    derby.record(0, totalled, second);
    CHECK(!derby.update(second));
    derby.record(1, totalled, second + 150000);
    CHECK(derby.update(second + 150000));
    CHECK(derby.rank(0) == 0 && derby.rank(1) == 0);
    CHECK(derby.state(0) == totalled && derby.state(1) == totalled);

    derby.reset(EventCompletionMode::Derby, 0b0111);
    derby.record(2, totalled, second);
    CHECK(derby.update(2 * second, true));
    CHECK(derby.state(0) == timed_out);
    CHECK(derby.state(1) == timed_out);
    CHECK(derby.state(2) == totalled);

    EventCompletion objective;
    objective.reset(EventCompletionMode::Native, 0b111);
    objective.record(0, totalled, second);
    objective.record(1, EventFinishState::Withdrawn, second);
    CHECK(!objective.update(second));
    objective.record(2, EventFinishState::Completed, 2 * second);
    CHECK(objective.update(2 * second));
    CHECK(objective.state(0) == totalled && objective.rank(2) == 0);
    objective.reset(EventCompletionMode::Native, 0b111);
    CHECK(objective.update(second, true));
    CHECK(objective.state(2) == EventFinishState::Completed);
    objective.reset(EventCompletionMode::Native, 0b111);
    CHECK(objective.update(second, false, true));

    EventCompletion battle;
    battle.reset(EventCompletionMode::Battle, 0b111);
    battle.record(0, totalled, second);
    battle.record(1, EventFinishState::Completed, second);
    battle.record(2, EventFinishState::Withdrawn, second);
    CHECK(!battle.update(second, true));
    CHECK(battle.state(0) == active && battle.state(1) == active);
    CHECK(battle.update(2 * second, false, true));
    CHECK(battle.state(0) == EventFinishState::BattleScore && battle.state(2) == EventFinishState::Withdrawn);

    Writer battle_wire;
    put_event_results(battle_wire, {{1, EventFinishState::BattleScore, 1, "Guest", 5},
                                    {0, EventFinishState::BattleScore, 2, "Host", -2}});
    Reader battle_reader(battle_wire.b.data(), battle_wire.b.size());
    std::vector<EventResultRow> battle_rows;
    CHECK(get_event_results(battle_reader, battle_rows));
    CHECK(battle_rows.size() == 2 && battle_rows[0].score == 5 && battle_rows[1].score == -2);

    Writer writer;
    put_event_results(writer, {{1, finished, 1, "Guest"}, {2, totalled, 0, "Host"}});
    Reader reader(writer.b.data(), writer.b.size());
    std::vector<EventResultRow> rows;
    CHECK(get_event_results(reader, rows));
    CHECK(rows.size() == 2 && rows[0].rank == 1 && rows[1].state == totalled && rows[1].name == "Host");
    Reader truncated(writer.b.data(), writer.b.size() - 1);
    CHECK(!get_event_results(truncated, rows));
    CHECK(rows.size() == 2);
    Writer duplicate;
    put_event_results(duplicate, {{1, finished, 1, "Guest"}, {1, totalled, 0, "Host"}});
    Reader invalid(duplicate.b.data(), duplicate.b.size());
    CHECK(!get_event_results(invalid, rows));
}

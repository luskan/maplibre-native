#include "../../src/mln/util/tile_trace.cpp"
#include <cassert>
#include <iostream>
#include <thread>

namespace t = mln::tiletrace;
namespace l = mln::layouttiming;

void dump(const char* label)
{
  std::cout << label << '\n' << l::snapshotJSON(t::session()) << '\n';
}

int main()
{
  static_assert(sizeof(l::Profile) < 2048);
  t::setTestTime(100);
  t::configure(true, true);
  auto trace = t::create(1, 2, 3, 10, 571, 337, 10, 0, 2);
  trace.id = trace.publication = t::nextID();
  trace.kind = t::Kind::Publication;
  trace.time[t::Delivered] = 100;
  assert(!l::makeSeed(trace, 4).generation);
  l::configure(true, true);
  t::setTestTime(110);
  const auto seed = l::makeSeed(trace, 4);
  l::Tracker tracker;
  tracker.start(seed, 200);
  tracker.enter(200);
  tracker.add(l::Phase::Parse, {210, 10, true}, {310, 80, true});
  tracker.leave(320, l::Gap::Dependencies, 0, true);
  tracker.enter(820);
  tracker.add(l::Phase::Callback, {830, 90, true}, {850, 100, true});
  tracker.add(l::Phase::Finalize, {855, 100, true}, {905, 130, true});
  l::Group group;
  group.name[0] = 'x'; group.features = 100; group.parseOrdinal = 1;
  group.work = {90, 60, 1, true};
  tracker.addGroup(group);
  const auto first = tracker.result(920);
  auto accepted = trace;
  accepted.kind = t::Kind::Layout;
  accepted.id = accepted.generation = t::nextID();
  accepted.time[t::Layout] = 1040;
  t::setTestTime(1040);
  l::publish(accepted, first, l::Disposition::Accepted, 1000, 9, 9);
  dump("first");

  tracker.leave(925, l::Gap::Coalescing, 2, true);
  tracker.enter(1000);
  tracker.add(l::Phase::Parse, {1020, 140, true}, {1120, 230, true});
  const auto second = tracker.result(1140);
  assert(first.work[0].wall == 100 && second.work[0].wall == 200);
  accepted.id = accepted.generation = t::nextID();
  accepted.time[t::Layout] = 1200;
  l::publish(accepted, second, l::Disposition::Accepted, 1190, 10, 10);
  dump("reparse");
  tracker.leave(1150, l::Gap::Dependencies, 0, true);
  l::publish(trace, tracker.finish(1300), l::Disposition::Replaced);
  dump("terminal");
  assert(!tracker.active());

  tracker.start(seed, 200); tracker.enter(200);
  tracker.add(l::Phase::Parse, {210, 20, true}, {260, 19, true});
  accepted.id = accepted.generation = t::nextID();
  l::publish(accepted, tracker.result(280), l::Disposition::Accepted, 290, 4, 4);
  dump("cpu_missing");
  tracker.add(l::Phase::Finalize, {300, 20, false}, {250, 0, false});
  accepted.id = accepted.generation = t::nextID();
  l::publish(accepted, tracker.result(310), l::Disposition::Accepted, 320, 4, 4);
  dump("bad_clock");

  tracker.start(seed, 200); tracker.enter(200);
  t::setTestTime(220);
  try
  {
    l::WorkScope work(tracker, l::Phase::Parse);
    t::setTestTime(250);
    throw 1;
  }
  catch (...) {}
  assert(tracker.terminalReason() == l::Disposition::Error);
  tracker.leave(260, l::Gap::Idle, 0, false);
  l::publish(trace, tracker.finish(260), l::Disposition::Error);
  dump("exception");

  tracker.start(seed, 200); tracker.enter(200);
  for (unsigned i = 1; i <= 12; ++i)
  {
    group.work.wall = i;
    tracker.addGroup(group);
  }
  accepted.id = accepted.generation = t::nextID();
  l::publish(accepted, tracker.result(300), l::Disposition::Accepted, 310, 4, 4);
  dump("groups");

  l::configure(true, true);
  auto current = first;
  t::setTestTime(110);
  current.seed = l::makeSeed(trace, 4);
  for (unsigned i = 0; i < 129; ++i)
  {
    accepted.id = accepted.generation = t::nextID();
    l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4);
  }
  dump("bounded");
  const auto previousLoss = t::collector().lost.load();
  {
    std::unique_lock lock(l::timingStore().mutex);
    std::thread writer([&] { l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4); });
    writer.join();
    std::thread reader([&] { assert(l::snapshotJSON(t::session()).find("false") != std::string::npos); });
    reader.join();
  }
  assert(t::collector().lost.load() == previousLoss);
  dump("contended");
  tracker.start(current.seed, 200);
  assert(tracker.active());
  l::configure(false);
  assert(!tracker.active());
  { l::WorkScope work(tracker, l::Phase::Parse); }
  assert(tracker.result(300).work[0].calls == 0);
  l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4);
  assert(!l::makeSeed(trace, 4).generation);
  dump("disabled");
  l::configure(true, true);
  assert(!tracker.active());
  l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4);
  dump("old_observer");
  current.seed = l::makeSeed(trace, 4);
  tracker.start(current.seed, 200);
  t::configure(false); t::configure(true);
  assert(!tracker.active());
  l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4);
  dump("capture_toggle");
  t::configure(true, true);
  l::publish(accepted, current, l::Disposition::Accepted, 1000, 4, 4);
  dump("capture_reset");
  trace.session = t::session();
  l::seedTestHook = [] { l::configure(true, true); };
  assert(!l::makeSeed(trace, 4).generation);
  l::seedTestHook = [] { t::configure(false); t::configure(true); };
  assert(!l::makeSeed(trace, 4).generation);
  l::seedTestHook = nullptr;
  assert(l::makeSeed(trace, 4).generation);
}

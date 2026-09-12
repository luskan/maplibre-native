#include <mln/test/util.hpp>
#include <mln/style/native_tile_request_state.hpp>
#include <mln/util/tile_trace.hpp>

#include <barrier>
#include <future>

using namespace mln;

namespace {
NativeRequestBinding binding(std::string key = "full-input-key")
{
  return {std::move(key), std::make_shared<const int>(1)};
}
}

TEST(NativeTileRequest, EqualKeysRetainTheOriginalOwnedSnapshot)
{
  NativeRequestState state({});
  const CanonicalTileID tile(3, 4, 4);
  auto firstInputs = std::make_shared<const int>(1);
  auto secondInputs = std::make_shared<const int>(2);
  auto first = state.resolve(tile, [&](const auto&) { return NativeRequestBinding{"key", firstInputs}; });
  auto second = state.resolve(tile, [&](const auto&) { return NativeRequestBinding{"key", secondInputs}; });
  ASSERT_TRUE(first.ticket.isCurrent());
  EXPECT_EQ(first.ticket, second.ticket);
  EXPECT_EQ(firstInputs, second.ticket.binding().inputs);
  auto changed = state.resolve(tile, [&](const auto&) { return NativeRequestBinding{"different-key", secondInputs}; });
  EXPECT_FALSE(first.ticket.isCurrent());
  EXPECT_TRUE(changed.ticket.isCurrent());
  EXPECT_NE(first.ticket.contentEpoch(), changed.ticket.contentEpoch());
  EXPECT_EQ(first.ticket.sourceEpoch(), changed.ticket.sourceEpoch());
  EXPECT_EQ(tile, changed.ticket.tileID());
  EXPECT_EQ(secondInputs, changed.ticket.binding().inputs);
}

TEST(NativeTileRequest, InvalidationDuringResolutionRejectsTheStaleSnapshot)
{
  NativeRequestState state({});
  const CanonicalTileID tile(3, 4, 4);
  std::promise<void> entered, resume;
  auto wait = resume.get_future();
  auto result = std::async(std::launch::async, [&] {
    return state.resolve(tile, [&](const auto&) {
      entered.set_value();
      wait.wait();
      return binding();
    });
  });
  entered.get_future().wait();
  state.invalidate(tile);
  resume.set_value();
  EXPECT_FALSE(result.get().ticket);
  EXPECT_TRUE(state.resolve(tile, [](const auto&) { return binding(); }).ticket.isCurrent());
}

TEST(NativeTileRequest, ClearAndRegionInvalidationRetireTheRightInputs)
{
  NativeRequestState state({});
  const CanonicalTileID first(5, 4, 4), other(5, 20, 20);
  auto a = state.resolve(first, [](const auto&) { return binding(); }).ticket;
  auto b = state.resolve(other, [](const auto&) { return binding(); }).ticket;
  state.invalidateRegion(LatLngBounds(first));
  EXPECT_FALSE(a.isCurrent());
  EXPECT_TRUE(b.isCurrent());
  auto next = state.resolve(first, [](const auto&) { return binding(); }).ticket;
  EXPECT_TRUE(next.isCurrent());
  EXPECT_NE(a, next);
  state.clear();
  EXPECT_FALSE(next.isCurrent());
  EXPECT_FALSE(b.isCurrent());
}

TEST(NativeTileRequest, SourceEpochsDoNotDependOnDiagnosticCapture)
{
  NativeRequestState first({}), second({});
  const CanonicalTileID tile(0, 0, 0);
  auto a = first.resolve(tile, [](const auto&) { return binding(); }).ticket;
  auto b = second.resolve(tile, [](const auto&) { return binding(); }).ticket;
  EXPECT_NE(a.sourceEpoch(), b.sourceEpoch());
  const auto capture = tiletrace::enabled();
  tiletrace::configure(capture, true);
  EXPECT_TRUE(a.isCurrent());
  EXPECT_TRUE(b.isCurrent());
  EXPECT_EQ(a, first.resolve(tile, [](const auto&) { return binding(); }).ticket);
  first.retire();
  EXPECT_FALSE(a.isCurrent());
  EXPECT_TRUE(b.isCurrent());
  bool called = false;
  EXPECT_FALSE(first.resolve(tile, [&](const auto&) { called = true; return binding(); }).ticket);
  EXPECT_FALSE(called);
}

TEST(NativeTileRequest, EmptyOrFailedBindingsHaveErrorIdentityAndCanRecover)
{
  NativeRequestState state({});
  const CanonicalTileID tile(0, 0, 0);
  auto empty = state.resolve(tile, [](const auto&) { return NativeRequestBinding{}; });
  ASSERT_TRUE(empty.error);
  EXPECT_TRUE(empty.ticket.isCurrent());
  EXPECT_TRUE(empty.ticket.binding().key.empty());
  auto failed = state.resolve(tile, [](const auto&) -> NativeRequestBinding { throw std::runtime_error("failed"); });
  EXPECT_TRUE(failed.error);
  EXPECT_EQ(empty.ticket, failed.ticket);
  auto recovered = state.resolve(tile, [](const auto&) { return binding(); });
  EXPECT_FALSE(recovered.error);
  EXPECT_TRUE(recovered.ticket.isCurrent());
  EXPECT_FALSE(empty.ticket.isCurrent());
  NativeRequestTicket absent;
  EXPECT_FALSE(absent.isCurrent());
  EXPECT_EQ(0u, absent.sourceEpoch());
  EXPECT_THROW(absent.binding(), std::logic_error);
}

TEST(NativeTileRequest, RegistryDoesNotOwnApplicationSnapshots)
{
  NativeRequestState state({});
  std::weak_ptr<const void> weak;
  {
    auto inputs = std::make_shared<const int>(1);
    weak = inputs;
    auto ticket = state.resolve(CanonicalTileID(0, 0, 0), [&](const auto&) {
      return NativeRequestBinding{"key", inputs};
    }).ticket;
    inputs.reset();
    EXPECT_FALSE(weak.expired());
  }
  EXPECT_TRUE(weak.expired());
  for (uint32_t x = 0; x < 70; ++x)
  {
    EXPECT_TRUE(state.resolve(CanonicalTileID(7, x, 0), [](const auto&) { return binding(); }).ticket.isCurrent());
  }
}

TEST(NativeTileRequest, DiscardedEqualKeySnapshotCanReenterInvalidation)
{
  NativeRequestState state({});
  const CanonicalTileID tile(0, 0, 0);
  auto first = state.resolve(tile, [](const auto&) { return binding(); }).ticket;
  bool released = false;
  auto second = state.resolve(tile, [&](const auto&) {
    auto inputs = std::shared_ptr<const int>(new int(2), [&](const int* value) {
      delete value;
      state.invalidate(tile);
      released = true;
    });
    return NativeRequestBinding{"full-input-key", std::move(inputs)};
  }).ticket;
  EXPECT_TRUE(released);
  EXPECT_EQ(first, second);
  EXPECT_FALSE(second.isCurrent());
}

TEST(NativeTileRequest, ConcurrentValidityReadsAndInvalidationAreSafe)
{
  NativeRequestState state({});
  const CanonicalTileID tile(3, 4, 4);
  auto ticket = state.resolve(tile, [](const auto&) { return binding(); }).ticket;
  std::barrier start(5);
  std::array<std::future<bool>, 4> readers;
  for (auto& reader : readers)
  {
    reader = std::async(std::launch::async, [&] {
      start.arrive_and_wait();
      while (ticket.isCurrent()) std::this_thread::yield();
      return ticket.binding().key == "full-input-key" && ticket.tileID() == tile;
    });
  }
  start.arrive_and_wait();
  state.invalidate(tile);
  for (auto& reader : readers) EXPECT_TRUE(reader.get());
}

// forza - nitrous (forza_nitrous, default off): hold D-pad Down for a torque multiplier from the
// "Nitrous" tiers of the Fuel System upgrade. The tank drains while it is used and refills slowly.
// Content gates it: without a patch DB that adds the tiers (Fuel System levels 4-8, part id
// engine*1000+level) no car ever has a tier, and nothing here does anything. docs/nitrous.md.
// tu_address_map: TU4 addresses (the base game build gets an inert pad filter).
//
// What the game gives us (TU4):
//  - sub_82CBCAAC creates one engine part record per installed part: r5 = part enum (16 =
//    Fuel System), r7 = the part's id in its List_UpgradeEngine* table. Fuel System ids are
//    engine*1000+level, so id % 1000 in 4..8 is nitrous tier 1..5.
//  - the car's setup copies the engine's nitrous block into the car at 82DF8790..82DF87C0
//    (car+1768..1788; FH2 reads them, FH1 never does). A mid-function hook right after
//    (82DF87C4, r31 = the car) records the tier the part records just announced.
//  - sub_82DF5A70 computes the engine's torque: curve value * car[+2644] (the engine torque
//    scale) * car[+420] * car[+424] * ... It runs for every car. For a car with a tier and a
//    held button we multiply car[+2644] for the duration of the call and put it back.
#include "../platform/guest_addr.h"

#include <cstddef>
#include <cstdint>

namespace forza {
#if FORZA_TU4
uint16_t NitrousPad(uint16_t buttons);
#else
uint16_t NitrousPad(uint16_t) { return 0; }
#endif
}

#if FORZA_TU4
#include "../platform/guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(forza_nitrous, false, "Game",
                    "Nitrous: hold D-pad Down with a Fuel System nitrous tier installed");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_tank_seconds, 6.0, "Game", "Nitrous: seconds of use per full tank");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_refill_seconds, 30.0, "Game",
                      "Nitrous: seconds to refill an empty tank while idle");

static_assert(offsetof(PPCContext, r3) == 0, "hooks recover ctx from &ctx.r3");

namespace {

constexpr uint32_t kEngineTorqueScaleOffset = 2644;  // car+2644, read by sub_82DF5A70
constexpr uint16_t kDpadDown = 0x0002;
constexpr int kFuelSystemPartEnum = 16;
constexpr float kTierScale[] = {1.5f, 2.0f, 2.5f, 3.0f, 4.0f};  // tiers 1..5
constexpr int kTiers = 5;

using Clock = std::chrono::steady_clock;

struct State {
  int tier = 0;
  double tank = 1.0;  // 0..1
  Clock::time_point last{};
  bool boosting = false;
};

std::mutex lock;
std::unordered_map<uint32_t, State> cars;  // guest car address -> state
std::atomic<bool> any{false};
std::atomic<bool> held{false};
thread_local int pending_tier = 0;  // set by the part records of the car being built

bool Enabled() { return REXCVAR_GET(forza_nitrous); }

// Fuel System part ids are engine*1000+level; tiers are levels 4..8.
int TierOfPart(uint32_t part_id) {
  const uint32_t level = part_id % 1000;
  return level >= 4 && level <= 8 ? int(level) - 3 : 0;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t v) {
  uint8_t* p = forza::GuestPtr(base, address);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Advance one car's tank to now; true while the shot is on.
bool Step(State& s, Clock::time_point now) {
  double dt = s.last.time_since_epoch().count() ? std::chrono::duration<double>(now - s.last).count() : 0.0;
  s.last = now;
  dt = std::clamp(dt, 0.0, 0.1);
  const double tank_s = std::max(0.5, REXCVAR_GET(forza_nitrous_tank_seconds));
  const double refill_s = std::max(1.0, REXCVAR_GET(forza_nitrous_refill_seconds));
  if (held.load(std::memory_order_relaxed) && s.tank > 0.0) {
    s.tank = std::max(0.0, s.tank - dt / tank_s);
    s.boosting = s.tank > 0.0;
  } else {
    s.boosting = false;
    s.tank = std::min(1.0, s.tank + dt / refill_s);
  }
  return s.boosting;
}

}  // namespace

// Player 1's pad, every XamInputGetState (kernel/input_autopress.cpp): remember the button and,
// while some car has a tier, hide it from the game.
uint16_t forza::NitrousPad(uint16_t buttons) {
  if (!Enabled()) { held = false; return 0; }
  held.store((buttons & kDpadDown) != 0, std::memory_order_relaxed);
  return any.load(std::memory_order_relaxed) ? kDpadDown : 0;
}

// A part record is being made: remember the Fuel System's tier for the car that follows.
extern "C" REX_FUNC(__imp__sub_82CBCAAC);
extern "C" REX_FUNC(sub_82CBCAAC) {
  if (int32_t(ctx.r5.u32) == kFuelSystemPartEnum) pending_tier = Enabled() ? TierOfPart(ctx.r7.u32) : 0;
  __imp__sub_82CBCAAC(ctx, base);
}

// hints_tu4/nitrous.toml: after the car copied the engine's nitrous block (r31 = the car).
void ForzaNitrousInit_82DF87C4(PPCRegister& r3) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  const uint32_t car = ctx.r31.u32;
  std::lock_guard<std::mutex> guard(lock);
  if (!Enabled() || pending_tier <= 0) {
    cars.erase(car);
  } else {
    State& s = cars[car];
    s.tier = pending_tier;
    s.tank = 1.0;
    s.last = {};
    REXLOG_INFO("NITROUS: car {:08X} tier {} (x{})", car, s.tier, kTierScale[s.tier - 1]);
  }
  any.store(!cars.empty(), std::memory_order_relaxed);
}

// The engine's torque: scale car+2644 for this call when the car's shot is on.
extern "C" REX_FUNC(__imp__sub_82DF5A70);
extern "C" REX_FUNC(sub_82DF5A70) {
  const uint32_t car = ctx.r3.u32;
  uint32_t saved = 0;
  bool scaled = false;
  if (any.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> guard(lock);
    const auto it = cars.find(car);
    if (it != cars.end() && Enabled()) {
      const bool was = it->second.boosting;
      if (Step(it->second, Clock::now())) {
        saved = Load32(base, car + kEngineTorqueScaleOffset);
        const float scale = std::bit_cast<float>(saved) * kTierScale[it->second.tier - 1];
        Store32(base, car + kEngineTorqueScaleOffset, std::bit_cast<uint32_t>(scale));
        scaled = true;
      }
      if (was != it->second.boosting)
        REXLOG_INFO("NITROUS: car {:08X} {} (tank {:.0f}%)", car, it->second.boosting ? "on" : "off",
                    it->second.tank * 100.0);
    }
  }
  __imp__sub_82DF5A70(ctx, base);
  if (scaled) Store32(base, car + kEngineTorqueScaleOffset, saved);
}
#endif  // FORZA_TU4

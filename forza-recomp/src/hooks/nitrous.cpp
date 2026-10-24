// forza - nitrous (forza_nitrous, default off): hold D-pad Down for a torque multiplier from the
// "Nitrous" tiers of the Fuel System upgrade. The tank drains while it is used and refills slowly.
// Content gates it: without a patch DB that adds the tiers (Fuel System levels 4-8, part id
// engine*1000+level) no car ever has a tier, and nothing here does anything. docs/nitrous.md.
// tu_address_map: TU4 addresses (the base game build gets an inert pad filter).
//
// What the game gives us (TU4):
//  - the player's vehicle: root = *(0x832DF024), list = *(*(root + 4) + 4), the vector at list+0x80,
//    the slot whose vehicle+0x2DD0 is 0 (this console's player). Its car is *(vehicle+0x2D90) and its
//    45 installed part ids start at vehicle+0x2E58 (the same blocks world_challenge.cpp and
//    native_car_tuning.cpp read). Fuel System is part enum 16; its ids are engine*1000+level, so
//    id % 1000 in 4..8 is nitrous tier 1..5.
//  - the engine's torque (sub_82DF5A70, r31 = the car) loads car[+2644] (the engine torque scale)
//    twice, at 82DF5AAC and 82DF5AE0, into f0 and multiplies it into the result. Mid-function hooks
//    right after each load scale f0 while the player's car has a tier and the shot is on. The
//    function is only entered indirectly, so it is hooked in the middle and not overridden.
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
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(forza_nitrous, false, "Game",
                    "Nitrous: hold D-pad Down with a Fuel System nitrous tier installed");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_tank_seconds, 6.0, "Game", "Nitrous: seconds of use per full tank");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_refill_seconds, 30.0, "Game",
                      "Nitrous: seconds to refill an empty tank while idle");

static_assert(offsetof(PPCContext, r3) == 0, "hooks recover ctx from &ctx.r3");

namespace {

constexpr uint32_t kCarRoot = FORZA_ADDR(0x832DF024);
constexpr uint32_t kVehicleCar = 0x2D90, kVehicleKind = 0x2DD0, kVehicleParts = 0x2E58;
constexpr uint32_t kVehicleEngineObject = 0x50;  // the torque's r31 is vehicle+0x50 (seen in the log)
constexpr int kPartCount = 45;
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
State player;  // the player's car (one tank)
std::atomic<bool> any{false};
std::atomic<bool> held{false};
uint32_t diag_car = 0;
uint32_t known_car = 0;  // the player's car last seen with its part ids logged

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
bool Plausible(uint32_t address) { return address >= 0x00100000 && address < 0xF0000000; }

// The tier of the player's car when `car` is it (0 otherwise); logs the part ids once per car.
int PlayerTier(uint8_t* base, uint32_t car) {
  const bool diag = diag_car != car;  // log what the lookup sees once per car (r31 of the torque)
  if (diag) {
    diag_car = car;
    REXLOG_INFO("NITROUS: torque hook running for car {:08X}", car);
  }
  const uint32_t root = Load32(base, kCarRoot);
  if (!Plausible(root)) { if (diag) REXLOG_INFO("NITROUS: root {:08X} not plausible", root); return 0; }
  const uint32_t holder = Load32(base, root + 4);
  if (!Plausible(holder)) { if (diag) REXLOG_INFO("NITROUS: holder {:08X} not plausible", holder); return 0; }
  const uint32_t list = Load32(base, holder + 4);
  if (!Plausible(list)) { if (diag) REXLOG_INFO("NITROUS: list {:08X} not plausible", list); return 0; }
  const uint32_t first = Load32(base, list + 0x80), end = Load32(base, list + 0x84);
  if (!Plausible(first) || end < first || end - first > 4 * 64) {
    if (diag) REXLOG_INFO("NITROUS: vehicle vector {:08X}..{:08X} not plausible", first, end);
    return 0;
  }
  for (uint32_t slot = first; slot < end; slot += 4) {
    const uint32_t vehicle = Load32(base, slot);
    if (diag && Plausible(vehicle))
      REXLOG_INFO("NITROUS: vehicle {:08X} kind {} car {:08X}", vehicle, Load32(base, vehicle + kVehicleKind),
                  Load32(base, vehicle + kVehicleCar));
    if (!Plausible(vehicle) || Load32(base, vehicle + kVehicleKind) != 0) continue;
    if (vehicle + kVehicleEngineObject != car) continue;
    const uint32_t fuel = Load32(base, vehicle + kVehicleParts + 4 * kFuelSystemPartEnum);
    const int tier = TierOfPart(fuel);
    if (known_car != car) {
      known_car = car;
      std::string ids;
      for (int i = 0; i < kPartCount; ++i) ids += std::to_string(Load32(base, vehicle + kVehicleParts + 4 * i)) + " ";
      REXLOG_INFO("NITROUS: player car {:08X} fuel part {} tier {}; parts {}", car, fuel, tier, ids);
    }
    return tier;
  }
  return 0;
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

// hints_tu4/nitrous.toml: the engine torque has just loaded car[+2644] into f0 (r31 = the car). The
// first hook (82DF5AB0) steps the tank, the second (82DF5AE4, the same call) reuses the result.
namespace {
bool on_this_call = false;  // the first hook decided; the second hook of the same call follows it
float scale_this_call = 1.0f;

void ScaleF0(PPCRegister& r3, bool first) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  if (first) {
    on_this_call = false;
    static bool said = false;
    if (!said) {
      said = true;
      REXLOG_INFO("NITROUS: the torque hook is live (forza_nitrous {})", Enabled() ? "on" : "off");
    }
    if (!Enabled()) {
      any.store(false, std::memory_order_relaxed);
      return;
    }
    uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    const uint32_t car = ctx.r31.u32;
    std::lock_guard<std::mutex> guard(lock);
    const int tier = PlayerTier(base, car);
    static uint32_t calls = 0, hits = 0;
    static Clock::time_point said_at{};
    ++calls;
    if (tier > 0) ++hits;
    if (Clock::now() - said_at > std::chrono::seconds(5)) {
      said_at = Clock::now();
      REXLOG_INFO("NITROUS: torque calls {} (tier hits {}), last car {:08X}", calls, hits, car);
      calls = hits = 0;
    }
    if (tier <= 0) {
      if (player.tier > 0) REXLOG_INFO("NITROUS: car {:08X} has no tier now", car);
      player = State{};
      any.store(false, std::memory_order_relaxed);
      return;
    }
    if (player.tier != tier) {
      player = State{};
      player.tier = tier;
      REXLOG_INFO("NITROUS: car {:08X} tier {} (x{})", car, tier, kTierScale[tier - 1]);
    }
    any.store(true, std::memory_order_relaxed);
    const bool was = player.boosting;
    on_this_call = Step(player, Clock::now());
    scale_this_call = kTierScale[tier - 1];
    if (was != player.boosting)
      REXLOG_INFO("NITROUS: {} (tank {:.0f}%)", player.boosting ? "on" : "off", player.tank * 100.0);
  }
  if (on_this_call) ctx.f0.f64 *= double(scale_this_call);
}
}  // namespace

void ForzaNitrousTorque1_82DF5AB0(PPCRegister& r3) { ScaleF0(r3, true); }
void ForzaNitrousTorque2_82DF5AE4(PPCRegister& r3) { ScaleF0(r3, false); }
#endif  // FORZA_TU4

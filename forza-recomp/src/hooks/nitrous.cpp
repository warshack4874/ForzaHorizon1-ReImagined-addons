// forza - nitrous (forza_nitrous, default off): hold D-pad Down for a torque multiplier from the
// "Nitrous" tiers of the Fuel System upgrade. The tank drains while it is used and refills slowly.
// Content gates it: without a patch DB that adds the tiers (Fuel System levels 4-8, part id
// engine*1000+level) no car ever has a tier, and nothing here does anything. docs/nitrous.md.
// tu_address_map: TU4 addresses (the base game build gets an inert pad filter).
//
// What the game gives us (TU4):
//  - the player's vehicle: root = *(0x832DF024), list = *(*(root + 4) + 4), the vector at list+0x80,
//    the slot whose vehicle+0x2DD0 is 0 (this console's player; the blocks world_challenge.cpp and
//    native_car_tuning.cpp read). Its 45 installed part ids start at vehicle+0x2E58; the Fuel System
//    is at index 18 (the Charger's 2410007 = engine 2410, level 7, seen in the log). Ids are
//    engine*1000+level, so id % 1000 in 4..8 is nitrous tier 1..5.
//  - the engine object is vehicle+0x50 (the engine torque code's r31); its engine torque scale is at
//    +2644. While the shot is on this code multiplies that float by the tier's factor and puts the
//    original back when it ends. It runs on the pad's clock (XamInputGetState), no code hooks.
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
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(forza_nitrous, false, "Game",
                    "Nitrous: hold D-pad Down with a Fuel System nitrous tier installed");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_tank_seconds, 6.0, "Game", "Nitrous: seconds of use per full tank");
REXCVAR_DEFINE_DOUBLE(forza_nitrous_refill_seconds, 30.0, "Game",
                      "Nitrous: seconds to refill an empty tank while idle");

namespace {

constexpr uint32_t kCarRoot = FORZA_ADDR(0x832DF024);
constexpr uint32_t kVehicleKind = 0x2DD0, kVehicleParts = 0x2E58;
constexpr uint32_t kVehicleEngineObject = 0x50;
constexpr uint32_t kEngineTorqueScale = 2644;
constexpr int kPartCount = 45;
constexpr int kFuelSystemPartIndex = 18;
constexpr uint16_t kDpadDown = 0x0002;
constexpr float kTierScale[] = {1.5f, 2.0f, 2.5f, 3.0f, 4.0f};  // tiers 1..5

using Clock = std::chrono::steady_clock;

struct State {
  uint32_t vehicle = 0;
  int tier = 0;
  double tank = 1.0;  // 0..1
  Clock::time_point last{};
  bool boosting = false;
  bool applied = false;  // the engine's torque scale is currently raised
  uint32_t saved = 0, scaled = 0;  // its original and raised bits
};

std::mutex lock;
State player;
bool held = false;

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
bool Plausible(uint32_t address) { return address >= 0x00100000 && address < 0xF0000000; }

// This console's player's vehicle (kind 0), 0 when there is none.
uint32_t PlayerVehicle(uint8_t* base) {
  const uint32_t root = Load32(base, kCarRoot);
  if (!Plausible(root)) return 0;
  const uint32_t holder = Load32(base, root + 4);
  if (!Plausible(holder)) return 0;
  const uint32_t list = Load32(base, holder + 4);
  if (!Plausible(list)) return 0;
  const uint32_t first = Load32(base, list + 0x80), end = Load32(base, list + 0x84);
  if (!Plausible(first) || end < first || end - first > 4 * 64) return 0;
  for (uint32_t slot = first; slot < end; slot += 4) {
    const uint32_t vehicle = Load32(base, slot);
    if (Plausible(vehicle) && Load32(base, vehicle + kVehicleKind) == 0) return vehicle;
  }
  return 0;
}

// Put the engine's torque scale back (only if the game has not changed it meanwhile).
void Release(uint8_t* base) {
  if (!player.applied) return;
  const uint32_t address = player.vehicle + kVehicleEngineObject + kEngineTorqueScale;
  if (Load32(base, address) == player.scaled) Store32(base, address, player.saved);
  player.applied = false;
}

// Advance the tank to now; true while the shot is on.
bool Step(State& s, Clock::time_point now) {
  double dt = s.last.time_since_epoch().count() ? std::chrono::duration<double>(now - s.last).count() : 0.0;
  s.last = now;
  dt = std::clamp(dt, 0.0, 0.1);
  const double tank_s = std::max(0.5, REXCVAR_GET(forza_nitrous_tank_seconds));
  const double refill_s = std::max(1.0, REXCVAR_GET(forza_nitrous_refill_seconds));
  if (held && s.tank > 0.0) {
    s.tank = std::max(0.0, s.tank - dt / tank_s);
    return s.tank > 0.0;
  }
  s.tank = std::min(1.0, s.tank + dt / refill_s);
  return false;
}

}  // namespace

// Player 1's pad, every XamInputGetState (kernel/input_autopress.cpp). The button is never hidden
// from the game; this only runs the tank and raises the engine's torque scale while the shot is on.
uint16_t forza::NitrousPad(uint16_t buttons) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return 0;
  uint8_t* base = kernel->memory()->virtual_membase();
  std::lock_guard<std::mutex> guard(lock);
  if (!Enabled()) {
    Release(base);
    player = State{};
    held = false;
    return 0;
  }
  const bool down = (buttons & kDpadDown) != 0;
  if (down != held) REXLOG_INFO("NITROUS: D-pad Down {}", down ? "pressed" : "released");
  held = down;

  const uint32_t vehicle = PlayerVehicle(base);
  const uint32_t fuel = vehicle ? Load32(base, vehicle + kVehicleParts + 4 * kFuelSystemPartIndex) : 0;
  const int tier = vehicle ? TierOfPart(fuel) : 0;
  if (vehicle != player.vehicle || tier != player.tier) {
    Release(base);
    player = State{};
    player.vehicle = vehicle;
    player.tier = tier;
    if (vehicle) {
      std::string ids;
      for (int i = 0; i < kPartCount; ++i) ids += std::to_string(Load32(base, vehicle + kVehicleParts + 4 * i)) + " ";
      REXLOG_INFO("NITROUS: player vehicle {:08X} fuel part {} tier {}; parts {}", vehicle, fuel, tier, ids);
    }
  }
  if (tier <= 0) return 0;

  const bool was = player.boosting;
  player.boosting = Step(player, Clock::now());
  const uint32_t address = vehicle + kVehicleEngineObject + kEngineTorqueScale;
  if (player.boosting && !player.applied) {
    player.saved = Load32(base, address);
    player.scaled = std::bit_cast<uint32_t>(std::bit_cast<float>(player.saved) * kTierScale[tier - 1]);
    Store32(base, address, player.scaled);
    player.applied = true;
    REXLOG_INFO("NITROUS: on x{} (torque scale {} -> {})", kTierScale[tier - 1], std::bit_cast<float>(player.saved),
                std::bit_cast<float>(player.scaled));
  } else if (!player.boosting && player.applied) {
    Release(base);
  }
  if (was != player.boosting && !player.boosting)
    REXLOG_INFO("NITROUS: off (tank {:.0f}%)", player.tank * 100.0);
  return 0;
}
#endif  // FORZA_TU4

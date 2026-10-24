# Nitrous (optional)

Hold **D-pad Down** to multiply the engine's torque while a nitrous tank has charge. Off by
default: `--forza_nitrous=true` (TU4 builds only; the base game build ignores it).

## Content gates it

There is no nitrous in Forza Horizon 1. The recompiled game keeps the car setup that copies a
nitrous block (on/off, torque scale, rpm range, buildup, tank time) into each car, but nothing
fills it and nothing reads it back. This feature supplies both ends from game data:

- **Tiers** are extra levels 4-8 of the Fuel System (`List_UpgradeEngineFuelSystem`, part id
  `engine*1000+level`) with `Upgrades` tiles for the three fuel system variants (types 4, 44,
  45): Nitrous 1.5x, 2x, 2.5x, 3x, 4x. A media.zip/DLC patch DB adds them; without one no car has a
  tier and the hook never acts.
- The player's vehicle is found the way the head-to-head challenges find it; its 45 installed part
  ids start at vehicle+0x2E58 and the Fuel System is at index 18. `id % 1000` in 4..8 is the tier.
- While the shot is on, the engine's torque scale (engine object vehicle+0x50, offset 2644) is
  multiplied by the tier's factor and put back when it ends. No code is hooked: it runs from the
  controller read (`kernel/input_autopress.cpp`) and D-pad Down is never hidden from the game.

## Tank

`--forza_nitrous_tank_seconds` (default 6) of use per full tank; `--forza_nitrous_refill_seconds`
(default 30) to refill an empty tank while the button is up. The state is per car.

## Known limits

- Not fully validated in game. The first versions hooked the engine torque functions
  (82DF5A70 and the four other places that read car+2644); in the log those run a few times a
  second for every car and almost never for the player's, so they are not the driving physics.
  Watch the log for `NITROUS:` lines; `on x3 (torque scale a -> b)` shows the write happened.
- The player's tank only; no HUD gauge yet (the log reports on/off and tank level).
- Time is host time, not game time.

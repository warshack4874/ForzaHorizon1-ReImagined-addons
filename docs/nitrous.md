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
- A car's tier is read while its parts are built (`sub_82CBCAAC`, part enum 16) and
  remembered for the car (`ForzaNitrousInit_82DF87C4`, after 82DF8790..87C0).
- The boost scales the engine torque scale at car+2644 for the duration of `sub_82DF5A70`.

## Tank

`--forza_nitrous_tank_seconds` (default 6) of use per full tank; `--forza_nitrous_refill_seconds`
(default 30) to refill an empty tank while the button is up. The state is per car.

## Known limits

- Not validated in game. TU4 addresses were found by comparing against the FH2 Fast & Furious
  build and by reading the code: 82CBCAAC (part records), 82DF87C4 (car setup), 82DF5A70 (engine
  torque), car+2644. Watch the log for `NITROUS:` lines.
- Any car whose parts include a nitrous tier gets the shot when the button is held, not only the
  player's. Tiers only come from the player's garage in practice.
- No HUD gauge yet; the log reports on/off and tank level.
- Time is host time, not game time.

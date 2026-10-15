# ts-liftable-axles (ETS2/ATS 1.61)

## Build

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target ts-liftable-axles
```

Install: copy `build/Release/ts-liftable-axles.dll` to
`<ETS2>/bin/win_x64/plugins/ts-liftable-axles.dll`.

Caution: `build/` keeps absolute paths in CMakeCache — after copying/moving the
project folder, delete `build/` and reconfigure or it compiles the old tree.

## Runtime layout found in eurotrucks2.exe 1.61 (re/ has the analysis scripts)

Trailer chain: `game_actor + 0xC8` = first trailer, `trailer + 0x1060` = next.

| offset | meaning |
| --- | --- |
| trailer + 0x150 | wheel-dyn object (per-wheel physics) |
| wheel_dyn + 0x10 | back-pointer to the owning trailer (sanity check) |
| wheel_dyn + 0x2B8 | wheel count (u32), 2 slots per axle |
| wheel_dyn + 0x2BC | wheel mask (u32) built once at setup — a secondary consumer effect, NOT the lift gate |
| trailer + 0x1F8 | chassis model object (refcounted), arg 2 of the controller init |
| trailer + 0x200 | axle/chassis dyn: +0x5C8 uint8_t* per-axle liftable flags, +0x5D0 u64 count, +0x5E8 u64[] per-axle tokens, +0x678 wheel count |
| trailer + 0x6A0 | lift controller (1.61) / **moved to +0x6C8 in 1.61.1.9s**, see below — only created when a flag byte is nonzero |
| trailer + 0xFB8 | must be non-null for the trailer lift key to work |
| game_actor + 0x018 | truck physics object (truck + 0x1580 = first trailer) |
| game_actor + 0x248 | lift bookkeeping byte: bit0 truck axles up, bit1 trailer axles up |

### 1.61.1.9s (exe 53,308,304 bytes, built 2026-10-13) — verified deltas

`.pdata` moved to VA `0x1436e3000` (120,273 entries); re-derive function ranges
from the PE headers, old addresses are invalid. Only ONE offset the plugin uses
changed:

- **`trailer + 0x6A0` -> `trailer + 0x6C8`** (lift controller field). Proven by
  the trailer init `0x14085fca0` (`lea rcx,[r14+0x6c8]` after the ctor call, then
  `mov rcx,[r14+0x6c8]` + init) and both set_state wrappers `0x1407868b0` /
  `0x140649840` reading `[trailer+0x6c8]`.
- Unchanged (verified in code): `game_actor+0xC8`, `+0x248` byte (toggle at
  `0x1409f6660`), `trailer+0x1060` (self-walk `mov rcx,[rcx+0x1060]` in
  `0x1409e5cf0` next to `+0x150`), `trailer+0x150`, `+0x1F8`, `+0x200`,
  `+0xFB8`, `axle_data+0x5C8/+0x5D0` (init `0x140a33f33` iterates them) and the
  whole controller layout (`+0x08/+0x18/+0x20/+0x38/+0x3C`, states 0-3).
- New lift-controller VAs: ctor `0x140a33e40`, init `0x140a33eb0` (mask-build
  loop at `0x140a33f30`), set_state `0x140a34080`, update tail `0x140a34187`
  (state transitions 1->0 / 2->3 at `0x140a34226`/`0x140a34232`).
- Console executor pattern still unique, new VA `0x1401e0cc0`.
- base_ctrl pattern still unique (resolves at file offset `0x5becb3`).

Lift controller (allocated 0x40 bytes, plain pointer field):

| offset | meaning |
| --- | --- |
| + 0x00 | chassis model object |
| + 0x08 | axle count (u32) = wheel count / 2 |
| + 0x10 | embedded dyn-array (vptr / data / size / cap): +0x18 float* progress |
| + 0x18 | per-axle float lift progress 0..1 |
| + 0x20 | u64 progress count = axle count |
| + 0x38 | **u32 per-axle lift mask** — the real selection gate |
| + 0x3C | state byte: 0 down, 1 lowering, 2 raising, 3 up |

Key functions (VA, base 0x140000000):

- `0x141d1e158` engine operator new(size)
- `0x140a33370` lift controller ctor (sets array vptr, mask=0, state=0)
- `0x140a333e0` controller init(ctrl, chassis): fills count/progress array and
  builds `+0x38` mask from `axle_data+0x5C8` flag bytes
- `0x140a335b0` controller set_state(dl): writes `+0x3C`, snaps progress
- `0x140a33670` controller update(dt): animates progress of masked axles only;
  transitions `+0x3C` 2->3 at progress 1.0, 1->0 at 0.0
- `0x140879df0` field-store helper (ptr* field, obj): destroys the old object
  via array dtor `0x1402574b0` + free `0x141d1e3e4`, then stores the new pointer
- `0x14085f710` trailer init path: at `0x14085f96a` checks "any flag" and only
  then allocates + constructs + stores + inits the controller at trailer+0x6A0
- `0x140789770` "any liftable axle in chain": scans trailers, true iff some
  controller has a bit set in `+0x38`
- `0x1409f5572` input toggle; writes `controller+0x3C` = 1/2 on every trailer
- `0x140677a50` reads axle_data flag byte by axle index
- `0x1406779d0` "any flag set" on axle_data +0x5C0 array
- `0x14078cebe` wheel-position pass: `flag[axle] != 0 && state != 0` applies
  `1.0 - progress[axle]` to the wheel
- `0x1405dc8d0` "can lift trailer" check; `0x1405dc900` "is lifted"
- `0x140905210` / `0x14090baf0` build/consume `wheel_dyn+0x2BC` — secondary
  effect only, rewriting it does NOT change which axles physically lift

`accessory_chassis_data` attribute table (1.61): powered_axle @ 0x548,
liftable_axle @ 0x5E8, steerable_axle @ 0x610 (token arrays, not bools).

## Axle lift feature

No keybind needed: the plugin rewrites `controller+0x38` (axle mask) and the
`axle_data+0x5C8` flag bytes every frame from the per-slot axle selection, so
the game's own trailer lift key raises exactly the checked axles.

Safety rules learned from testing:

- Only **natively liftable** axles may be selected (`selection &= orig_flags`).
  Forcing a non-liftable axle to rise crashes the wheel/suspension update.
- Nothing is written while `controller+0x3C != 0` AND the applied mask is
  nonzero (raising/raised/lowering); flipping flags mid-air leaves mixed
  progress state that hangs the game. The UI locks the checkboxes in that
  state and asks to lower the axles.
- `state != 0` with an empty applied mask is a desync (native toggle flipped
  the state but nothing can animate — mask 0 never transitions). The plugin
  writes state=0 to recover instead of leaving the trailer "raising" forever.
- Selection persists in `%APPDATA%\ts-liftable-axles\settings.ini`
  `[axle_lift] trailer_N` (-1 = native).

Reverse-gear assist ("van der leest" style, `reverse_lift` setting, on by
default): telemetry `truck.light.reverse` / `truck.engine.gear` < 0 mark
reverse. On the engage edge, if every trailer controller is at state 0 and any
trailer has a liftable axle, the effective selection becomes the lowest
native_axles bit per trailer and `controller+0x3C` = 2 plus
`game_actor+0x248` bit1 are written — the native raise path then runs. On
disengage, controllers that are up get `+0x3C` = 1 and the bit is cleared;
the saved selection resumes once the axles reach the ground. If axles were
already up when reverse engaged, nothing is overridden.

Creating a controller for trailers with zero liftable axles was implemented
(game alloc `0x141d1e158` + ctor `0x140a33370` + field-store `0x140879df0` +
init `0x140a333e0`, chassis object at trailer+0x1F8) and worked, but was
removed: with no natively liftable axle there is nothing safe to select.

## Trailer covers (g_trailer_cables_mode)

Cover mods (Librelato sider, graneleiro, etc.) rig door/tarp/curtain animations
to the native `g_trailer_cables_mode` cvar, so typing
`g_trailer_cables_mode N` in the console opens/closes them (values are
mod-defined; typically 0 = all open, 1 = first trailer closed, 2+ = all closed).

Decoupling the cover from the ropes was tried and does NOT work on the tested
mods: calling `0x140876ac0` (named-node flag setter for the trlr_cables nodes:
node[0x142d4df98].bit = !flag, node[0x142d4dfa0].bit = flag, via lookup
`0x1406260e0` on `obj+0x120`/`+0x190`) and calling the vehicle connector-state
setter `0x1408735e0` (writes `vehicle+0xE65`, calls attach/detach virtuals on
the `+0xE70` object list) both run without error but produce no cover movement —
the cover pose follows the rope objects themselves (trailer+0x1080 list),
which only `g_trailer_cables_mode` create/destroy.

Console line executor (1.61, ETS2): `0x1401e0c20`
`execute_line(char** text, int queue_ctx)` — rcx is a `char**` whose first
qword is the command text (only +0 is read), edx is the queue context and the
game passes -1 for direct execution (call site `0x1404267fa`). The command
queue (`exec` handler `0x1401cc3a0` -> stream exec `0x1401e09f0`) funnels into
the same function; `[cmd] '%s' - unknown command` is logged inside it at
`0x1401e1040`. Plugin calls it with `"g_trailer_cables_mode N"` — identical to
typing in the console, no separate apply needed.

Pattern (unique in 1.61):
`40 55 41 54 41 55 48 8D AC 24 00 F4 FF FF 48 81 EC 00 0D 00 00 44 8B EA 4C 8B E1`

Other cable internals (noted): rope rebuild `0x140876770`, per-chain build
`0x1408758b0`, per-trailer `0x140875930`, mode apply `0x140875830` (r9d=mode,
5 = read cvar via `0x1401cbd70`). Cvar descriptors live in a static array at
`0x142d475c0` (stride 0x140, name at +0x08, value string ~+0x28).

Keybinds: `covers_toggle` cycles `0 -> 1 -> ... -> min(trailer_count,2) -> 0`
(open all at once, then close one trailer per press) and `covers_next` jumps
straight to mode 0 (open all), in settings.ini `[keybinds]`. The plugin tracks
the last mode it sent; a manual console change desyncs it by one press.

# Space Invaders (Terminal, C)

A classic Space Invaders clone that runs directly in a Linux terminal. Same
game logic and grid across all versions (movement, collisions, guard-island
shields) — only the renderer differs between them. No external libraries
required — just a C standard library and POSIX terminal control.

Four destructible **guard islands** sit between you and the invaders — duck
behind them for cover. Each one erodes block by block as it takes hits (from
either side) and its color shifts green → yellow → red as it takes damage.

Destroy all the invaders before they reach the bottom, or before your 3
lives run out.

## Controls (all versions)

- `a` / left arrow — move left
- `d` / right arrow — move right
- `space` — shoot
- `q` — quit

## Build

```
make
```

Builds all three versions. `make ver1` / `make ver2` / `make ver3` to build
just one.

## Versions

### ver1 — colored panel

`./ver1` — a `btop`-style rounded, colored border panel with plain
single-character glyphs (`W` invaders, `A` ship, `#` shields). Requires a
256-color terminal window at least 62x26.

### ver2 — pixel-art sprites

`./ver2` — same panel, but invaders, the ship, and shields are drawn as
small sprites using half-block characters (▀▄█) to pack two vertical
"pixels" into each terminal cell. Same size requirement as ver1.

### ver3 — blit (Sixel bitmap)

`./ver3` — the play field is a real bitmap image, rendered as actual pixel
sprites and blitted into the terminal via the Sixel graphics protocol.
Requires a **Sixel-capable terminal** (xterm built with Sixel support and
`-ti vt340`, mlterm, foot, wezterm, or similar — check your terminal's docs).
If you just see garbled escape-code text instead of graphics, your terminal
doesn't support Sixel.

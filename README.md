# Space Invaders (Terminal, C)

A classic Space Invaders clone that runs directly in a Linux terminal, styled
as a colored, rounded-border panel in the spirit of `btop`. Invaders, the
player ship, and the guard islands are drawn as small pixel-art sprites using
half-block characters (▀▄█) to pack two vertical "pixels" into each terminal
cell. No external libraries required — just a C standard library and POSIX
terminal control.

## Build

```
make
```

## Run

```
./invaders
```

Requires a 256-color terminal window at least 62x26.

## Controls

- `a` / left arrow — move left
- `d` / right arrow — move right
- `space` — shoot
- `q` — quit

Four destructible **guard islands** sit between you and the invaders — duck
behind them for cover. Each one erodes block by block as it takes hits (from
either side) and its color shifts green → yellow → red as it takes damage.

Destroy all the invaders before they reach the bottom, or before your 3
lives run out.

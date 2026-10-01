# Ferz — CMD Chess GUI (v14)

A small graphical chess environment built with SDL2, supporting two UCI
engines simultaneously, a built-in engine (StrongEngine), opening book,
move analysis, ponder, real tournament manager and themes.

## Compilation (Windows, MinGW64)

```bat
gcc -O3 -DUSE_BOOK -o chess_gui_v14.exe chess_gui_v14.c chess.res -lSDL2 -lSDL2_image -lSDL2_ttf -lm -lcomdlg32 -lshell32 -lole32 -mwindows
```

Required DLLs (in the folder with the .exe): `SDL2.dll`,
`SDL2_image.dll`, `SDL2_ttf.dll`, `libpng16-16.dll`, `libfreetype-6.dll`,
`libharfbuzz-0.dll`, `zlib1.dll` plus the MinGW runtime DLLs, and the
`pieces/` folder with the piece graphics. `book.h` is only needed at
compile time.

## Opening names in saved PGN

Saved games carry `[ECO]` and `[Opening]` tags. The data lives in
`eco_openings.tsv` (lichess-org/chess-openings) and is turned into the
`eco.h` header by:

```bat
python make_eco.py
```

`eco.h` is only needed at compile time, and is optional — without it the
program still builds and simply leaves those two tags out.

## License

GPL-3.0 — free to use and modify, but distributed changes must remain
under GPL. See `LICENSE`.

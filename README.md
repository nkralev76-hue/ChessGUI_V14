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

## License

GPL-3.0 — free to use and modify, but distributed changes must remain
under GPL. See `LICENSE`.

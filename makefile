all: donut-nes donut-nes.exe

donut-nes: donut-nes-cli.c donut-nes.h
	musl-gcc -static -O2 -std=c99 -Wall -Wextra -Wpedantic -o donut-nes-cli donut-nes-cli.c

donut-nes.exe: donut-nes-cli.c donut-nes.h
	i686-w64-mingw32-gcc -static -O2 -std=c99 -Wall -Wextra -Wpedantic -o donut-nes-cli donut-nes-cli.c

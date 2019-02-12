all: donut donut.exe

donut: donut.c
	gcc -o donut -O3 -std=c90 -Wall -static donut.c

donut.exe: donut.c
	i686-w64-mingw32-gcc -static -O3 -std=c90 -Wall -o donut.exe donut.c


#test:
#	musl-gcc -static -O3 -std=c90 -Wall -o donut donut.c
#	gcc -o donut -O3 -std=c90 -Wall -static -g -pg -fprofile-arcs -ftest-coverage donut.c
#	afl-gcc -o donut -O3 -std=c90 -Wall -static donut.c

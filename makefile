donut: donut.c
	gcc -o donut -O3 -Wall -static donut.c

#	musl-gcc -static -O3 -std=c11 -Wall -o donut donut.c
#	afl-gcc -o donut -O3 -std=c11 -Wall -static donut.c
#	gcc -o donut -O3 -Wall -static -g -pg -fprofile-arcs -ftest-coverage donut.c

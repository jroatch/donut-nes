#include <stdio.h>   /* I/O */
#include <errno.h>   /* errno */
#include <stdlib.h>  /* exit(), strtol() */
#include <stdbool.h> /* bool */
#include <stddef.h>  /* null */
#include <stdint.h>  /* uint8_t */
#include <string.h>  /* memcpy() */
#include <getopt.h>  /* getopt_long() */

const char *VERSION_TEXT = "Donut 1.7\n";
const char *HELP_TEXT =
    "Donut NES CHR Codec\n"
    "\n"
    "Usage:\n"
    "  donut [options] [--] INPUT... OUTPUT\n"
    "  donut -d [options] [--] INPUT... OUTPUT\n"
    "  donut [-d] [options] -o OUTPUT INPUT...\n"
    "  donut -h | --help\n"
    "  donut --version\n"
    "\n"
    "Options:\n"
    "  -h --help              show this help message and exit\n"
    "  --version              show program's version number and exit\n"
    "  -d, --decompress       decompress the input files\n"
    "  -o FILE, --output=FILE\n"
    "                         output to FILE instead of last positional argument\n"
    "                         FILE can be \'-\' to indicate stdin and stdout\n"
    "  -f, --force            overwrite output without prompting\n"
    "  -q, --quiet            suppress messages and completion stats\n"
    "  --no-bit-flip          don't encode blocks that requires rotation\n"
    "  --cycle-limit INT      limits the 6502 decoding time for each encoded block\n"
;

/* According to a strace of cat on my system, and a quick dd of dev/zero:
   131072 is the optimal block size,
   but that's 2 times the size of the entire 6502 address space!
   The usual data input is going to be 512 tiles of NES gfx data. */
#define BUF_IO_SIZE 8192
#define BUF_GAP_SIZE 128
#define BUF_TEMP_SIZE 256
#define BUF_TOTAL_SIZE ((BUF_IO_SIZE+BUF_GAP_SIZE)*2+BUF_TEMP_SIZE)

static uint8_t byte_buffer[BUF_TOTAL_SIZE];

#define OUTPUT_BEGIN (byte_buffer)
#define INPUT_BEGIN (byte_buffer + BUF_IO_SIZE + BUF_GAP_SIZE)
#define TEMP_BEGIN (byte_buffer + BUF_TOTAL_SIZE - BUF_TEMP_SIZE)

int popcount8(uint8_t x) {
    /* would be nice if I could get this to compile to 1 CPU instruction. */
    x = (x & 0x55 ) + ((x >>  1) & 0x55 );
    x = (x & 0x33 ) + ((x >>  2) & 0x33 );
    x = (x & 0x0f ) + ((x >>  4) & 0x0f );
    return (int)x;
}

typedef struct byte_range {
    uint8_t *begin;    /* first valid byte */
    uint8_t *end;      /* one past the last valid byte */
                       /* length is infered as end - begin. */
} byte_range;

typedef struct buffer_pointers {
    byte_range source;
    byte_range destination;
} buffer_pointers;

uint64_t flip_plane_bits_135(uint64_t plane) {
    uint64_t result = 0;
    uint64_t t;
    int i;
    if (plane == 0xffffffffffffffff)
        return plane;
    if (plane == 0x0000000000000000)
        return plane;
    for (i = 0; i < 8; ++i) {
        t = plane >> i;
        t &= 0x0101010101010101;
        t *= 0x0102040810204080;
        t >>= 56;
        t &= 0xff;
        result |= t << (i*8);
    }
    return result;
}

int pack_pb8(uint8_t *buffer_ptr, uint64_t plane, uint8_t top_value) {
    uint8_t pb8_ctrl;
    uint8_t pb8_byte;
    uint8_t c;
    uint8_t *p;
    int i;
    p = buffer_ptr;
    ++p;
    pb8_ctrl = 0;
    pb8_byte = top_value;
    for (i = 0; i < 8; ++i) {
        c = plane >> (8*(7-i));
        if (c != pb8_byte) {
            *p = c;
            ++p;
            pb8_byte = c;
            pb8_ctrl |= 0x80>>i;
        }
    }
    *buffer_ptr = pb8_ctrl;
    return p - buffer_ptr;
}

uint64_t read_plane(uint8_t *p) {
    return  ((uint64_t)*(p+0) << (8*0)) |
            ((uint64_t)*(p+1) << (8*1)) |
            ((uint64_t)*(p+2) << (8*2)) |
            ((uint64_t)*(p+3) << (8*3)) |
            ((uint64_t)*(p+4) << (8*4)) |
            ((uint64_t)*(p+5) << (8*5)) |
            ((uint64_t)*(p+6) << (8*6)) |
            ((uint64_t)*(p+7) << (8*7));
}

void write_plane(uint8_t *p, uint64_t plane) {
    *(p+0) = (plane >> (8*0)) & 0xff;
    *(p+1) = (plane >> (8*1)) & 0xff;
    *(p+2) = (plane >> (8*2)) & 0xff;
    *(p+3) = (plane >> (8*3)) & 0xff;
    *(p+4) = (plane >> (8*4)) & 0xff;
    *(p+5) = (plane >> (8*5)) & 0xff;
    *(p+6) = (plane >> (8*6)) & 0xff;
    *(p+7) = (plane >> (8*7)) & 0xff;
}

int cblock_cost(uint8_t *p, int l) {
    int cycles;
    uint8_t block_header;
    uint8_t plane_def;
    int pb8_count;
    bool decode_only_1_pb8_plane;
    uint8_t short_defs[4] = {0x00, 0x55, 0xaa, 0xff};
    if (l < 1)
        return 0;
    block_header = *p;
    --l;
    if (block_header >= 0xc0)
        return 0;
    if (block_header == 0x2a)
        return 1269;
    cycles = 1281;
    if (block_header & 0xc0)
        cycles += 640;
    if (block_header & 0x20)
        cycles += 4;
    if (block_header & 0x10)
        cycles += 4;
    if (block_header & 0x02) {
        if (l < 1)
            return 0;
        plane_def = *(p+1);
        --l;
        cycles += 5;
        decode_only_1_pb8_plane = ((block_header & 0x04) && (plane_def != 0x00));
    } else {
        plane_def = short_defs[(block_header & 0x0c) >> 2];
        decode_only_1_pb8_plane = false;
    }
    pb8_count = popcount8(plane_def);
    cycles += (block_header & 0x01) ? (pb8_count * 614) : (pb8_count * 75);
    if (!decode_only_1_pb8_plane) {
        l -= pb8_count;
        cycles += l * 6;
    } else {
        --l;
        cycles += pb8_count;
        cycles += (l * 6 * pb8_count);
    }
    return cycles;
}

bool all_pb8_planes_match(uint8_t *p, int pb8_length, int number_of_pb8) {
    int i, c, l;
    l = number_of_pb8*pb8_length;
    for (c = 0, i = pb8_length; i < l; ++i, ++c) {
        if (c >= pb8_length) {
            c = 0;
        }
        if (*(p + c) != *(p + i)) {
            return false;
        }
    }
    return true;
}

void decompress_blocks(buffer_pointers *result_p, bool allow_partial, bool last_block) {
    buffer_pointers p;
    uint64_t plane;
    uint64_t prev_plane = 0;
    int i, j, l;
    uint8_t block_header;
    uint8_t plane_def;
    uint8_t pb8_flags;
    uint8_t pb8_byte;
    uint8_t short_defs[4] = {0x00, 0x55, 0xaa, 0xff};
    bool less_then_74_bytes_left;
    bool decode_only_1_pb8_plane;
    uint8_t *single_pb8_plane_ptr;
    p = *(result_p);
    while (p.source.begin - p.destination.end >= 64) {
        less_then_74_bytes_left = (p.source.end - p.source.begin < 74);
        if ((less_then_74_bytes_left) && ((!last_block) || (p.source.end - p.source.begin < 1))) {
            return;
        }
        block_header = *(p.source.begin);
        ++(p.source.begin);
        if (block_header >= 0xc0) {
            *(result_p) = p;
            continue;
        }
        if (block_header == 0x2a) {
            l = p.source.end - p.source.begin;
            if (less_then_74_bytes_left && (l < 64)) {
                if (!allow_partial)
                    return;
                memset(p.destination.end, 0x00, 64);
            } else {
                l = 64;
            }
            memmove(p.destination.end, p.source.begin, l);
            p.source.begin += l;
            p.destination.end += 64;
        } else {
            single_pb8_plane_ptr = NULL;
            if (block_header & 0x02) {
                if (less_then_74_bytes_left && (p.source.end - p.source.begin < 1)) {
                    if (!allow_partial)
                        return;
                    plane_def = 0x00;
                } else {
                    plane_def = *(p.source.begin);
                    ++(p.source.begin);
                }
                decode_only_1_pb8_plane = ((block_header & 0x04) && (plane_def != 0x00));
                single_pb8_plane_ptr = p.source.begin;
            } else {
                plane_def = short_defs[(block_header & 0x0c) >> 2];
                decode_only_1_pb8_plane = false;
            }
            for (i = 0; i < 8; ++i) {
                if ((((i & 1) == 0) && (block_header & 0x20)) || ((i & 1) && (block_header & 0x10))) {
                    plane = 0xffffffffffffffff;
                } else {
                    plane = 0x0000000000000000;
                }
                if (plane_def & 0x80) {
                    if (decode_only_1_pb8_plane) {
                        p.source.begin = single_pb8_plane_ptr;
                    }
                    if (less_then_74_bytes_left && (p.source.end - p.source.begin < 1)) {
                        if (!allow_partial)
                            return;
                        pb8_flags = 0x00;
                        plane_def = 0x00;
                    } else {
                        pb8_flags = *(p.source.begin);
                        ++p.source.begin;
                    }
                    pb8_byte = (uint8_t)plane;
                    for (j = 0; j < 8; ++j) {
                        if (pb8_flags & 0x80) {
                            if (less_then_74_bytes_left && (p.source.end - p.source.begin < 1)) {
                                if (!allow_partial)
                                    return;
                                pb8_flags = 0x00;
                                plane_def = 0x00;
                            } else {
                                pb8_byte = *(p.source.begin);
                                ++p.source.begin;
                            }
                        }
                        pb8_flags <<= 1;
                        plane <<= 8;
                        plane |= pb8_byte;
                    }
                    if (block_header & 0x01) {
                        plane = flip_plane_bits_135(plane);
                    }
                }
                plane_def <<= 1;
                if (i & 1) {
                    if (block_header & 0x80) {
                        prev_plane ^= plane;
                    }
                    if (block_header & 0x40) {
                        plane ^= prev_plane;
                    }
                    write_plane(p.destination.end, prev_plane);
                    p.destination.end += 8;
                    write_plane(p.destination.end, plane);
                    p.destination.end += 8;
                } else {
                    prev_plane = plane;
                }
            }
        }
        *(result_p) = p;
    }
    return;
}

void compress_blocks(buffer_pointers *result_p, bool use_bit_flip, int cycle_limit){
    buffer_pointers p;
    uint64_t block[8];
    uint64_t plane;
    uint64_t plane_predict;
    int shortest_length;
    int least_cost;
    int a, i, r, l;
    uint8_t temp_cblock[74];
    uint8_t *temp_p;
    uint8_t plane_def;
    uint8_t short_defs[4] = {0x00, 0x55, 0xaa, 0xff};
    bool planes_match;
    bool pb8_planes_match;
    uint64_t first_non_zero_plane;
    int number_of_pb8_planes;
    int first_pb8_length;
    p = *(result_p);
    while ((p.source.end - p.source.begin >= 64) && (p.source.begin - p.destination.end >= 65)) {
        *(p.destination.end) = 0x2a;
        memmove(p.destination.end + 1, p.source.begin, 64);
        shortest_length = 65;
        least_cost = 1269;
        for (i = 0; i < 8; ++i) {
            block[i] = read_plane(p.source.begin);
            p.source.begin += 8;
        }
        for (r = 0; r < 2; ++r) {
            if (r == 1) {
                if (use_bit_flip) {
                    for (i = 0; i < 8; ++i) {
                        block[i] = flip_plane_bits_135(block[i]);
                    }
                } else {
                    break;
                }
            }
            for (a = 0; a < 0xc; ++a) {
                temp_p = temp_cblock + 2;
                plane_def = 0x00;
                number_of_pb8_planes = 0;
                for (i = 0; i < 8; ++i) {
                    plane = block[i];
                    if ((i & 1) == 0) {
                        plane_predict = (a & 0x2) ? 0xffffffffffffffff : 0x0000000000000000;
                        if (a & 0x8) {
                            plane ^= block[i+1];
                        }
                    } else {
                        plane_predict = (a & 0x1) ? 0xffffffffffffffff : 0x0000000000000000;
                        if (a & 0x4) {
                            plane ^= block[i-1];
                        }
                    }
                    plane_def <<= 1;
                    if (plane != plane_predict) {
                        l = pack_pb8(temp_p, plane, (uint8_t)plane_predict);
                        temp_p += l;
                        plane_def |= 1;
                        if (number_of_pb8_planes == 0) {
                            planes_match = true;
                            first_non_zero_plane = plane;
                            first_pb8_length = l;
                        } else if (first_non_zero_plane != plane) {
                            planes_match = false;
                        }
                        ++number_of_pb8_planes;
                    }
                }
                temp_cblock[0] = r | (a<<4) | 0x02;
                temp_cblock[1] = plane_def;
                l = temp_p - temp_cblock;
                temp_p = temp_cblock;
                if (number_of_pb8_planes <= 1) {
                    planes_match = false;
                    pb8_planes_match = false;
                } else {
                    if (first_pb8_length * number_of_pb8_planes == l-2) {
                        pb8_planes_match = all_pb8_planes_match(temp_p+2, first_pb8_length, number_of_pb8_planes);
                    } else {
                        pb8_planes_match = false;
                    }
                }
                if (pb8_planes_match) {
                    *(temp_p + 0) = r | (a<<4) | 0x06;
                    l = 2 + first_pb8_length;
                } else if (planes_match) {
                    *(temp_p + 0) = r | (a<<4) | 0x06;
                    l = 2 + pack_pb8(temp_p+2, first_non_zero_plane, ~(uint8_t)first_non_zero_plane);
                } else {
                    for (i = 0; i < 4; ++i) {
                        if (plane_def == short_defs[i]) {
                            ++temp_p;
                            *(temp_p + 0) = r | (a<<4) | (i << 2);
                            --l;
                            break;
                        }
                    }
                }
                if (l <= shortest_length) {
                    i = cblock_cost(temp_p, l);
                    if ((i <= cycle_limit) && ((l < shortest_length) || (i < least_cost))) {
                        memmove(p.destination.end, temp_p, l);
                        shortest_length = l;
                        least_cost = i;
                    }
                }
            }
        }
        p.destination.end += shortest_length;
        *(result_p) = p;
    }
    return;
}

int main (int argc, char **argv)
{
    int c;
    bool have_input_filenames;
    bool have_output_filename;
    char *input_filename = NULL;
    char *output_filename = NULL;
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    bool decompress = false;
    bool force_overwrite = false;
    bool quiet_flag = false;
    bool no_bit_flip_blocks = false;

    int total_bytes_in = 0;
    int total_bytes_out = 0;
    /*float total_bytes_ratio = 0.0;*/
    int i;
    int number_of_stdin_args = 0;

    buffer_pointers p = {NULL};
    size_t l;

    int cycle_limit = 10000;

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    while (1) {
        static struct option long_options[] =
        {
            {"help",        no_argument,        NULL,   'h'},
            {"version",     no_argument,        NULL,   'v'},
            {"decompress",  no_argument,        NULL,   'd'},
            {"output",      required_argument,  NULL,   'o'},
            {"force",       no_argument,        NULL,   'f'},
            {"quiet",       no_argument,        NULL,   'q'},
            {"no-bit-flip", no_argument,        NULL,   'b'+256},
            {"cycle-limit", required_argument, NULL,   'y'+256},
            {NULL, 0, NULL, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long(argc, argv, "hvdo:fq",
                        long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
        case 'h':
            fputs(HELP_TEXT, stdout);
            exit(EXIT_SUCCESS);

        break; case 'v':
            fputs(VERSION_TEXT, stdout);
            exit(EXIT_SUCCESS);

        break; case 'd':
            decompress = true;

        break; case 'o':
            output_filename = optarg;

        break; case 'f':
            force_overwrite = true;

        break; case 'q':
            quiet_flag = true;
            opterr = 0;

        break; case 'b'+256:
            no_bit_flip_blocks = true;

        break; case 'y'+256:
            cycle_limit = strtol(optarg, NULL, 0);
            if (cycle_limit < 1269) {
                fputs("Invalid parameter for --cycle-limit. Must be a integer >= 1269.\n", stderr);
                exit(EXIT_FAILURE);
            }

        break; case '?':
            /* getopt_long already printed an error message. */
            exit(EXIT_FAILURE);

        break; default:
        break;
        }
    }

    if (quiet_flag) {
        fclose(stderr);
    }

    if (output_filename == NULL) {
        if (argc - optind >= 2) {
            output_filename = argv[argc-1];
            --argc;
        }
    }

    have_input_filenames = (argc - optind > 0);
    have_output_filename = (output_filename != NULL);
    if (!have_input_filenames) {
        if (!quiet_flag) {
            if (!have_output_filename)
                fputs("Input and output filenames required. Try --help for more info.\n", stderr);
            else
                fputs("Input filenames required. Try --help for more info.\n", stderr);
        }
        exit(EXIT_FAILURE);
    } else if (!have_output_filename) {
        if (!quiet_flag)
            fputs("Output file required. Try --help for more info.\n", stderr);
        exit(EXIT_FAILURE);
    }

    for (i = optind; i < argc; ++i){
        if (strcmp(argv[i], "-") == 0) {
            ++number_of_stdin_args;
        }
    }

    if (strcmp(output_filename, "-") == 0) {
        output_file = stdout;
    } else {
        fclose(stdout);
        if (!force_overwrite) {
            /* open output for read to check for file existence. */
            output_file = fopen(output_filename, "rb");
            if (output_file != NULL) {
                fclose(output_file);
                output_file = NULL;
                if (number_of_stdin_args <= 0) {
                    if (!quiet_flag) {
                        fputs(output_filename, stderr);
                        fputs(" already exists; do you wish to overwrite (y/N) ? ", stderr);
                        c = fgetc(stdin);
                        if (c != 'y' && c != 'Y'){
                            fputs("    not overwritten\n", stderr);
                            exit(EXIT_FAILURE);
                        }
                    } else {
                        exit(EXIT_FAILURE);
                    }
                } else {
                    if (!quiet_flag) {
                        fputs(output_filename, stderr);
                        fputs(" already exists; not overwritten\n", stderr);
                    }
                    exit(EXIT_FAILURE);
                }
            } else if ((output_file == NULL) && (errno == ENOENT)) {
                errno = 0;
            }
        }
        if (errno == 0) {
            output_file = fopen(output_filename, "wb");
        }
        if (output_file == NULL) {
            if (!quiet_flag)
                perror(output_filename);
            exit(EXIT_FAILURE);
        }
        setvbuf(output_file, NULL, _IONBF, 0);
    }

    p.source.begin = INPUT_BEGIN;
    p.source.end = INPUT_BEGIN;
    p.destination.begin = OUTPUT_BEGIN;
    p.destination.end = OUTPUT_BEGIN;
    if (number_of_stdin_args <= 0) {
        fclose(stdin);
    }
    while ((optind < argc) && (!ferror(output_file))) {
        input_filename = argv[optind];
        if ((number_of_stdin_args > 0) && (strcmp(input_filename, "-") == 0)) {
            input_file = stdin;
        } else {
            input_file = fopen(input_filename, "rb");
        }
        if (input_file != NULL) {
            setvbuf(input_file, NULL, _IONBF, 0);
            while(!feof(input_file) && !ferror(input_file) && !ferror(output_file)) {
                l = (size_t)(p.source.end - p.source.begin);
                if (l <= BUF_GAP_SIZE) {
                    if (l > 0) {
                        memmove(INPUT_BEGIN, p.source.begin, l);
                    }
                    p.source.begin = INPUT_BEGIN;
                    p.source.end = INPUT_BEGIN + l;

                    l = fread(p.source.end, sizeof(uint8_t), (size_t)BUF_IO_SIZE, input_file);
                    if (ferror(input_file)) {
                        perror(input_filename);
                        errno = 0;
                        break;
                    }
                    if (l == 0) {
                        continue;
                    }
                    total_bytes_in += l;
                    p.source.end = p.source.end + l;
                }

                if (decompress) {
                    decompress_blocks(&p, false, false);
                } else {
                    compress_blocks(&p, !no_bit_flip_blocks, cycle_limit);
                }

                while ((p.destination.end - p.destination.begin) >= BUF_IO_SIZE) {
                    l = fwrite(p.destination.begin, sizeof(uint8_t), (size_t)BUF_IO_SIZE, output_file);
                    if (ferror(output_file)) {
                        perror(output_filename);
                        exit(EXIT_FAILURE);
                    }
                    total_bytes_out += l;
                    p.destination.begin = p.destination.begin + l;
                }
                if (p.destination.begin > OUTPUT_BEGIN) {
                    l = (size_t)(p.destination.end - p.destination.begin);
                    if (l > 0) {
                        memmove(OUTPUT_BEGIN, p.destination.begin, l);
                    }
                    p.destination.begin = OUTPUT_BEGIN;
                    p.destination.end = OUTPUT_BEGIN + l;
                }
            }
            if (input_file == stdin) {
                --number_of_stdin_args;
                if (number_of_stdin_args <= 0) {
                    fclose(input_file);
                } else {
                    clearerr(input_file);
                }
            } else {
                fclose(input_file);
            }
        } else {
            if (!quiet_flag)
                perror(input_filename);
            errno = 0;
        }
        ++optind;
    }
    if (p.source.end - p.source.begin > 0) {
        if (decompress) {
            decompress_blocks(&p, true, true);
        } else {
            compress_blocks(&p, !no_bit_flip_blocks, cycle_limit);
        }
    }
    l = (size_t)(p.destination.end - p.destination.begin);
    if (l > 0) {
        l = fwrite(p.destination.begin, sizeof(uint8_t), l, output_file);
        total_bytes_out += l;
    }
    /* print totals here */
    /* print("<total> :{:>6.1%} ({} => {} bytes, {})".format(ratio, r, w, output_file.file_name), file=sys.stderr) */

    /* print("{} :{:>6.1%} ({} => {} bytes)".format(output_file.file_name ,ratio, r, w), file=sys.stderr) */
    /*if (!quiet_flag) {
        if (decompress) {
            if (total_bytes_out == 0) {
                total_bytes_ratio = 0.0;
            } else {
                total_bytes_ratio = 1.0 - (total_bytes_in / total_bytes_out);
            }
        } else {
            if (total_bytes_in == 0) {
                total_bytes_ratio = 0.0;
            } else {
                total_bytes_ratio = 1.0 - (total_bytes_out / total_bytes_in);
            }
        }
        fputs(output_filename, stderr);
        fputs(" :-99.8% (8192 => 8320 bytes)", stderr);
    }*/

    if (output_file != NULL) {
        fclose(output_file);
    }

    exit(EXIT_SUCCESS);
}

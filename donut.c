#include <stdio.h>   /* I/O */
#include <errno.h>   /* errno */
#include <stdlib.h>  /* exit() */
#include <stdbool.h> /* bool */
#include <stddef.h>  /* null */
#include <stdint.h>  /* uint8_t */
#include <string.h>  /* memcpy() */
#include <getopt.h>  /* getopt_long() */

const char *VERSION_TEXT = "Donut 1.4\n";
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
;

#define BUF_IO_SIZE 8192
#define BUF_GAP_SIZE 256
#define BUF_TOTAL_SIZE ((BUF_IO_SIZE+BUF_GAP_SIZE)*2)

uint8_t byte_buffer[BUF_TOTAL_SIZE];

#define INPUT_BEGIN (byte_buffer + BUF_TOTAL_SIZE - BUF_IO_SIZE)
#define OUTPUT_BEGIN (byte_buffer)

int popcount8(uint8_t x) {
    // would be nice if I could get this to compile to 1 CPU instruction.
    x = (x & 0x55 ) + ((x >>  1) & 0x55 );
    x = (x & 0x33 ) + ((x >>  2) & 0x33 );
    x = (x & 0x0f ) + ((x >>  4) & 0x0f );
    return (int)x;
}

typedef struct byte_range {
    uint8_t *begin;    // first valid byte
    uint8_t *end;      // one past the last valid byte
                    // length is infered as end - begin.
} byte_range;

typedef struct buffer_pointers {
    byte_range source;
    byte_range destination;
} buffer_pointers;

uint64_t flip_plane_bits_135(uint64_t plane) {
    uint64_t result = 0;
    int i;
    for (i = 0; i < 64; ++i) {
        result |= ((plane >> i) & 1) << (((i & 0x07) << 3) | ((i & 0x38) >> 3));
    }
    return result;
}

uint64_t unpack_pb8_unchecked(uint8_t **buffer_ptr, uint8_t top_value) {
    uint64_t plane;
    uint8_t *ptr;
    uint8_t pb8_ctrl;
    uint8_t pb8_byte;
    int i;
    ptr = *buffer_ptr;
    pb8_ctrl = *(ptr);
    ++ptr;
    plane = 0;
    pb8_byte = top_value;
    for (i = 0; i < 8; ++i) {
        if (pb8_ctrl & 0x80) {
            pb8_byte = *(ptr);
            ++ptr;
        }
        pb8_ctrl <<= 1;
        plane <<= 8;
        plane |= pb8_byte;
    }
    *buffer_ptr = ptr;
    return plane;
}

bool unpack_pb8(byte_range *buffer, uint64_t *plane, uint8_t top_value) {
    uint8_t pb8_ctrl;
    int l;
    l = buffer->end - buffer->begin;
    if (l < 1) {
        return true;
    }
    pb8_ctrl = *(buffer->begin);
    if (l < popcount8(pb8_ctrl)+1){
        return true;
    }
    *plane = unpack_pb8_unchecked(&(buffer->begin), top_value);
    return false;
}

void decompress_blocks(buffer_pointers *result_p) {
    buffer_pointers p;
    uint64_t plane_l;
    uint64_t plane_m;
    int i, j;
    uint8_t block_header;
    uint8_t plane_def;
    uint8_t short_defs[4] = {0x00, 0x55, 0xaa, 0xff};
    p = *(result_p);
    while (p.source.begin - p.destination.end >= 73) {
        if (p.source.end - p.source.begin < 1) {
            return;
        }
        block_header = *(p.source.begin);
        ++(p.source.begin);
        if (block_header >= 0xc0) {
            if (p.source.end - p.source.begin < 64) {
                return;
            }
            memmove(p.destination.end, p.source.begin, 64);
            p.source.begin += 64;
            p.destination.end += 64;
        } else {
            plane_def = block_header & 0x03;
            if (plane_def == 0) {
                if (p.source.end - p.source.begin < 1) {
                    return;
                }
                plane_def = *(p.source.begin);
                ++(p.source.begin);
            } else {
                plane_def = short_defs[plane_def];
            }
            for (j = 0; j < 4; ++j) {
                if (block_header & 0x10) {
                    plane_l = 0xffffffffffffffff;
                } else {
                    plane_l = 0x0000000000000000;
                }
                if (block_header & 0x20) {
                    plane_m = 0xffffffffffffffff;
                } else {
                    plane_m = 0x0000000000000000;
                }
                if (plane_def & 0x80) {
                    if (unpack_pb8(&(p.source), &plane_l, (uint8_t)plane_l) == true) {
                        return;
                    }
                    if (block_header & 0x08) {
                        plane_l = flip_plane_bits_135(plane_l);
                    }
                }
                plane_def <<= 1;
                if (plane_def & 0x80) {
                    if (unpack_pb8(&(p.source), &plane_m, (uint8_t)plane_m) == true) {
                        return;
                    }
                    if (block_header & 0x08) {
                        plane_m = flip_plane_bits_135(plane_m);
                    }
                }
                plane_def <<= 1;
                if (block_header & 0x40) {
                    plane_l ^= plane_m;
                }
                if (block_header & 0x80) {
                    plane_m ^= plane_l;
                }
                /*if (p.source.begin - p.destination.end < 16) {
                    return;
                }*/
                for (i = 0; i < 8; ++i) {
                    *(p.destination.end) = (plane_l >> (i*8)) & 0xff;
                    ++(p.destination.end);
                }
                for (i = 0; i < 8; ++i) {
                    *(p.destination.end) = (plane_m >> (i*8)) & 0xff;
                    ++(p.destination.end);
                }
            }
        }
        *(result_p) = p;
    }
}

void decompress_blocks_fast(buffer_pointers *p) {
    uint8_t *block_offset_end;
    uint64_t plane_l;
    uint64_t plane_m;
    int i;
    uint8_t block_header;
    uint8_t plane_def;
    uint8_t short_defs[4] = {0x00, 0x55, 0xaa, 0xff};
    while ((p->destination.end <= p->source.begin-64) && (p->source.begin <= p->source.end-73)) {
        block_offset_end = p->destination.end + 64;
        block_header = *(p->source.begin);
        ++p->source.begin;
        if (block_header >= 0xc0) {
            memmove(p->destination.end, p->source.begin, 64);
            p->source.begin += 64;
            p->destination.end = block_offset_end;
        } else {
            plane_def = block_header & 0x03;
            if (plane_def == 0) {
                plane_def = *(p->source.begin);
                ++p->source.begin;
            } else {
                plane_def = short_defs[plane_def];
            }
            while (p->destination.end < block_offset_end) {
                if (block_header & 0x10) {
                    plane_l = 0xffffffffffffffff;
                } else {
                    plane_l = 0x0000000000000000;
                }
                if (block_header & 0x20) {
                    plane_m = 0xffffffffffffffff;
                } else {
                    plane_m = 0x0000000000000000;
                }
                if (plane_def & 0x80) {
                    plane_l = unpack_pb8_unchecked(&(p->source.begin), (uint8_t)plane_l);
                    if (block_header & 0x08) {
                        plane_l = flip_plane_bits_135(plane_l);
                    }
                }
                plane_def <<= 1;
                if (plane_def & 0x80) {
                    plane_m = unpack_pb8_unchecked(&(p->source.begin), (uint8_t)plane_m);
                    if (block_header & 0x08) {
                        plane_m = flip_plane_bits_135(plane_m);
                    }
                }
                plane_def <<= 1;
                if (block_header & 0x40) {
                    plane_l ^= plane_m;
                }
                if (block_header & 0x80) {
                    plane_m ^= plane_l;
                }
                for (i = 0; i < 8; ++i) {
                    *(p->destination.end) = (plane_l >> (i*8)) & 0xff;
                    ++p->destination.end;
                }
                for (i = 0; i < 8; ++i) {
                    *(p->destination.end) = (plane_m >> (i*8)) & 0xff;
                    ++p->destination.end;
                }
            }
        }
    }
    return;
}

void compress_blocks(buffer_pointers *p, bool use_bit_flip){
    int i,k;
    while ((p->destination.end <= p->source.begin-73) && (p->source.begin <= p->source.end-64)) {
        *(p->destination.end) = 0x0f;
        ++p->destination.end;
        for (i = 0; i < 8; ++i) {
            *(p->destination.end) = 0xff;
            ++p->destination.end;
            for (k = 0; k < 8; ++k) {
                *(p->destination.end + k) = *(p->source.begin + 8 - 1 - k);
            }
            p->source.begin += 8;
            //flip_plane_bits_135(p->destination.end);
            p->destination.end += 8;
        }
    }
    return;
}

void compress_blocks_raw_only(buffer_pointers *p, bool use_bit_flip){
//    fputs("doing a compress block\n", stderr);
    while ((p->destination.end <= p->source.begin-65) && (p->source.begin <= p->source.end-64)) {
        *(p->destination.end) = 0xc0;
        ++p->destination.end;
        memmove(p->destination.end, p->source.begin, 64);
        p->source.begin += 64;
        p->destination.end += 64;
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
    FILE *input_file;
    FILE *output_file;
    bool decompress = false;
    bool force_overwrite = false;
    bool quiet_flag = false;
    bool no_bit_flip_blocks = false;

    int debug_total_in_bytes = 0;
    int debug_total_out_bytes = 0;
    int i;
    int number_of_stdin_args = 0;

    buffer_pointers p = {NULL};
    /*buffer_pointers p_base = {NULL};*/
    size_t l;

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

        break; case '?':
            /* getopt_long already printed an error message. */
            exit(EXIT_FAILURE);

        break; default:
        break;
        }
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

    if (strcmp(output_filename, "-") == 0) {
        output_file = stdout;
    } else {
        fclose(stdout);
        if (force_overwrite == true) {
            output_file = fopen(output_filename, "wb");
        } else {
            output_file = fopen(output_filename, "wbx");
            if ((errno == EEXIST) && (!quiet_flag) && (argv[optind][0] != '-')) {
                fputs(output_filename, stderr);
                fputs(" already exists; do you wish to overwrite (y/N) ? ", stderr);
                c = fgetc(stdin);
                if (c == 'y' || c == 'Y'){
                    errno = 0;
                    output_file = fopen(output_filename, "wb");
                } else {
                    fputs("    not overwritten\n", stderr);
                    exit(EXIT_FAILURE);
                }
            }
        }
        if (output_file == NULL) {
            if (!quiet_flag)
                perror(output_filename);
            exit(EXIT_FAILURE);
        }
    }

    p.source.begin = INPUT_BEGIN;
    p.source.end = INPUT_BEGIN;
    p.destination.begin = OUTPUT_BEGIN;
    p.destination.end = OUTPUT_BEGIN;
    setvbuf(output_file, NULL, _IONBF, 0);
    for (i = optind; i < argc; ++i){
        if (strcmp(argv[i], "-") == 0) {
            ++number_of_stdin_args;
        }
    }
    if (number_of_stdin_args == 0) {
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
                /*fprintf (stderr, "in:%d out:%d|\t",
                        debug_total_in_bytes,
                        debug_total_out_bytes
                );*/

                l = (size_t)(p.source.end - p.source.begin);
                if (l <= BUF_GAP_SIZE) {
                    if (l > 0) {
                        memmove(INPUT_BEGIN - l, p.source.begin, l);
                    }
                    p.source.begin = INPUT_BEGIN - l;
                    p.source.end = INPUT_BEGIN;

                    l = fread(p.source.end, sizeof(uint8_t), (size_t)BUF_IO_SIZE, input_file);
                    if (ferror(input_file)) {
                        perror(input_filename);
                        errno = 0;
                        break;
                    }
                    if (l == 0) {
                        continue;
                    }
                    //fprintf(stderr, "%ld\t", l);
                    debug_total_in_bytes += l;
                    p.source.end = p.source.end + l;
                }

                /*fprintf (stderr, "read:[%ld %ld %ld %ld]\t",
                        p.destination.begin - OUTPUT_BEGIN,
                        p.destination.end - OUTPUT_BEGIN,
                        p.source.begin - OUTPUT_BEGIN,
                        p.source.end - OUTPUT_BEGIN
                );*/

                if (decompress) {
                    decompress_blocks_fast(&p);
                    //decompress_blocks(&p);
                } else {
                    compress_blocks(&p, !no_bit_flip_blocks);
                }

                /*fprintf (stderr, "proc:[%ld %ld %ld %ld]\t",
                        p.destination.begin - OUTPUT_BEGIN,
                        p.destination.end - OUTPUT_BEGIN,
                        p.source.begin - OUTPUT_BEGIN,
                        p.source.end - OUTPUT_BEGIN
                );*/

                while ((p.destination.end - p.destination.begin) >= BUF_IO_SIZE) {
                    l = fwrite(p.destination.begin, sizeof(uint8_t), (size_t)BUF_IO_SIZE, output_file);
                    if (ferror(output_file)) {
                        perror(output_filename);
                        exit(EXIT_FAILURE);
                    }
                    //fprintf(stderr, "%ld\n", l);
                    debug_total_out_bytes += l;
                    // check for file error
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

                /*fprintf (stderr, "write:[%ld %ld %ld %ld]\n",
                        p.destination.begin - OUTPUT_BEGIN,
                        p.destination.end - OUTPUT_BEGIN,
                        p.source.begin - OUTPUT_BEGIN,
                        p.source.end - OUTPUT_BEGIN
                );*/
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
            decompress_blocks(&p);
        } else {
            compress_blocks(&p, !no_bit_flip_blocks);
        }
        l = (size_t)(p.destination.end - p.destination.begin);
        l = fwrite(p.destination.begin, sizeof(uint8_t), l, output_file);
        debug_total_out_bytes += l;
        /*p.destination.begin = p.destination.begin + l;
        l = (size_t)(p.destination.end - p.destination.begin);
        if (l > 0) {
            memmove(p_base.destination.begin, p.destination.begin, l);
        }
        p.destination.begin = p_base.destination.begin;
        p.destination.end = p_base.destination.begin + l;*/
    }

    if (output_file != NULL) {
        fclose(output_file);
    }

    exit(EXIT_SUCCESS);
}

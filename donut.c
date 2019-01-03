#include <stdio.h>   /* I/O */
#include <errno.h>   /* errno */
#include <stdlib.h>  /* exit() */
#include <stdbool.h> /* bool */
#include <stddef.h>  /* null */
#include <stdint.h>  /* uint8_t */
#include <string.h>  /* strcmp() */
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

#define BUF_NUMBER_OF_BLOCKS 512
#define BUF_UNCOMPRESSED_SIZE (BUF_NUMBER_OF_BLOCKS*16)
#define BUF_MAX_COMPRESSED_SIZE (BUF_NUMBER_OF_BLOCKS*17)
#define BUF_SIZE (BUF_NUMBER_OF_BLOCKS*16*2)

uint8_t popcount8(uint8_t x) {
    x = (x & 0x55 ) + ((x >>  1) & 0x55 );
    x = (x & 0x33 ) + ((x >>  2) & 0x33 );
    x = (x & 0x0f ) + ((x >>  4) & 0x0f );
    return x;
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

uint64_t unpack_pb8(uint8_t **buffer_ptr, uint8_t top_value) {
    uint64_t plane = 0;
    uint8_t *ptr = *buffer_ptr;
    uint8_t pb8_ctrl;
    int i;
    pb8_ctrl = *(ptr);
    ++ptr;
    for (i=0;i<8;++i) {
        if (pb8_ctrl & 0x80) {
            top_value = *(ptr);
            ++ptr;
        }
        pb8_ctrl <<= 1;
        plane <<= 8;
        plane |= top_value;
    }
    *buffer_ptr = ptr;
    return plane;
}

void decompress_blocks(buffer_pointers *p){
    int i;
    uint8_t *block_offset_end;
    uint8_t block_header;
    uint8_t plane_def;
    uint8_t pb8_byte;
    bool is_M_plane = false;
    uint64_t plane;
    while ((p->destination.end <= p->source.begin-64) && (p->source.begin <= p->source.end-73)) {
        block_offset_end = p->destination.end + 64;
        block_header = *(p->source.begin);
        ++p->source.begin;
        if (block_header >= 0xc0) {
            memcpy(p->destination.end, p->source.begin, 64);
            p->source.begin += 64;
            p->destination.end = block_offset_end;
        } else {
            plane_def = block_header & 0x03;
            if (plane_def == 0) {
                plane_def = *(p->source.begin);
                ++p->source.begin;
            } else {
                plane_def = (plane_def<<6) | (plane_def<<4) | (plane_def<<2) | (plane_def);
            }
            while (p->destination.end < block_offset_end) {
                if (((block_header & 0x20) && is_M_plane) || ((block_header & 0x10) && !is_M_plane)) {
                    plane = 0xffffffffffffffff;
                    pb8_byte = 0xff;
                } else {
                    plane = 0x0000000000000000;
                    pb8_byte = 0x00;
                }
                if (plane_def & 0x80) {
                    plane = unpack_pb8(&(p->source.begin), pb8_byte);
                    if (block_header & 0x08) {
                        plane = flip_plane_bits_135(plane);
                    }
                }
                plane_def <<= 1;
                *(p->destination.end+0) = (plane >>  0) & 0xff;
                *(p->destination.end+1) = (plane >>  8) & 0xff;
                *(p->destination.end+2) = (plane >> 16) & 0xff;
                *(p->destination.end+3) = (plane >> 24) & 0xff;
                *(p->destination.end+4) = (plane >> 32) & 0xff;
                *(p->destination.end+5) = (plane >> 40) & 0xff;
                *(p->destination.end+6) = (plane >> 48) & 0xff;
                *(p->destination.end+7) = (plane >> 56) & 0xff;
                p->destination.end += 8;
                /*if (is_M_plane) {
                    if (block_header & 0x40) {
                        for(i = 1; i <= 8; i++){
                            *(p->destination.end-8-i) ^= *(p->destination.end-i);
                        }
                    }
                    if (block_header & 0x80) {
                        for(i = 1; i <= 8; i++){
                            *(p->destination.end-i) ^= *(p->destination.end-8-i);
                        }
                    }
                }*/
                is_M_plane = !is_M_plane;
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
        memcpy(p->destination.end, p->source.begin, 64);
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

    uint8_t byte_buffer[BUF_SIZE] = {0};
    buffer_pointers p = {NULL};
    buffer_pointers p_base = {NULL};
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
        if (force_overwrite == true) {
            output_file = fopen(output_filename, "wb");
        } else {
            output_file = fopen(output_filename, "wbx");
            if ((errno == EEXIST) && (!quiet_flag)) {
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

    if (decompress) {
        p_base.source.begin = byte_buffer + BUF_SIZE - BUF_MAX_COMPRESSED_SIZE;
        p_base.source.end = byte_buffer + BUF_SIZE;
        p_base.destination.begin = byte_buffer;
        p_base.destination.end = byte_buffer + BUF_UNCOMPRESSED_SIZE;
    }else {
        p_base.source.begin = byte_buffer + BUF_SIZE - BUF_UNCOMPRESSED_SIZE;
        p_base.source.end = byte_buffer + BUF_SIZE;
        p_base.destination.begin = byte_buffer;
        p_base.destination.end = byte_buffer + BUF_MAX_COMPRESSED_SIZE;
    }
    p.source.begin = p_base.source.begin;
    p.source.end = p_base.source.begin;
    p.destination.begin = p_base.destination.begin;
    p.destination.end = p_base.destination.begin;
    setvbuf(output_file, NULL, _IONBF, 0);
    while (optind < argc) {
        input_filename = argv[optind];
        if (strcmp(input_filename, "-") == 0) {
            input_file = stdin;
        } else {
            input_file = fopen(input_filename, "rb");
        }
        if (input_file != NULL) {
            setvbuf(input_file, NULL, _IONBF, 0);
            while(!feof(input_file)) {
                l = (size_t)(p_base.source.end - p.source.end);
                l = fread(p.source.end, sizeof(uint8_t), l, input_file);
                // check for file error
                if (l == 0) {
                    continue;
                }
                //fputs("doing a compress block\n", stderr);
                p.source.end = p.source.end + l;
                if (decompress) {
                    decompress_blocks(&p);
                } else {
                    compress_blocks(&p, !no_bit_flip_blocks);
                }
                l = (size_t)(p.destination.end - p.destination.begin);
                l = fwrite(p.destination.begin, sizeof(uint8_t), l, output_file);
                // check for file error
                p.destination.begin = p.destination.begin + l;
                l = (size_t)(p.destination.end - p.destination.begin);
                if (l > 0) {
                    memcpy(p_base.destination.begin, p.destination.begin, l);
                }
                p.destination.begin = p_base.destination.begin;
                p.destination.end = p_base.destination.begin + l;
                l = (size_t)(p.source.end - p.source.begin);
                if (l > 0) {
                    memcpy(p_base.source.begin, p.source.begin, l);
                }
                p.source.begin = p_base.source.begin;
                p.source.end = p_base.source.begin + l;
            }
            if (input_file != stdin) {
                fclose(input_file);
            }
        } else {
            if (!quiet_flag)
                perror(input_filename);
            errno = 0;
        }
        ++optind;
    }
    if (output_file != stdout) {
        fclose(output_file);
    }

    exit(EXIT_SUCCESS);
}

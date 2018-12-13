#include <stdio.h>   /* I/O */
#include <errno.h>   /* errno */
#include <stdlib.h>  /* exit() */
#include <stdbool.h> /* bool */
#include <stddef.h>  /* null */
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

int main (int argc, char **argv)
{
    int c;
    bool have_input_filenames;
    bool have_output_filename;
    char *input_filename = NULL;
    char *output_filename = NULL;
    FILE * input_file;
    FILE * output_file;
    bool force_overwrite = false;
    bool quiet_flag = false;
    bool no_bit_flip_blocks = false;

    while (1) {
        static struct option long_options[] =
        {
            {"help",        no_argument,        NULL,   'h'},
            {"version",     no_argument,        NULL,   'v'},
            {"quiet",       no_argument,        NULL,   'q'},
            {"decompress",  no_argument,        NULL,   'd'},
            {"force",       no_argument,        NULL,   'f'},
            {"output",      required_argument,  NULL,   'o'},
            {"no-bit-flip", no_argument,        NULL,   'b'+256},
            {NULL, 0, NULL, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long(argc, argv, "hvqdfo:",
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

        break; case 'q':
            quiet_flag = true;

        break; case 'f':
            force_overwrite = true;

        break; case 'o':
            output_filename = optarg;

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
        if (!have_output_filename)
            fputs("Input and output filenames required. Try --help for more info.\n", stderr);
        else
            fputs("Input filenames required. Try --help for more info.\n", stderr);
        exit(EXIT_FAILURE);
    } else if (!have_output_filename) {
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
            if (errno == EEXIST) {
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
            perror(output_filename);
            exit(EXIT_FAILURE);
        }
    }
    while (optind < argc) {
        input_filename = argv[optind];
        if (strcmp(input_filename, "-") == 0) {
            input_file = stdin;
        } else {
            input_file = fopen(input_filename, "rb");
        }
        if (input_file != NULL) {
            // do procsessing
            if (input_file != stdin) {
                fclose(input_file);
            }
        } else {
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

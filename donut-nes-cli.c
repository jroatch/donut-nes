/* Standard headers that do not require the C runtime */
#include <stddef.h>
#include <limits.h>
#include <float.h>
#include <stdarg.h>
#include <stdint.h>       // C99
#include <stdbool.h>      // C99
//#include <iso646.h>       // don't use this
//#include <stdalign.h>     // C11
//#include <stdnoreturn.h>  // C11

#define DONUT_NES_IMPLEMENTATION
#include "donut-nes.h"

#include <stdio.h>   /* I/O */
#include <errno.h>   /* errno */
#include <stdlib.h>  /* exit(), strtol() */
#include <string.h>  /* memcpy() */
#include <getopt.h>  /* getopt_long() */

const char *PROGRAM_NAME = "donut-nes";
const char *USAGE_TEXT =
	"donut-nes - A NES CHR Codec\n"
	"\n"
	"Usage:\n"
	"  donut-nes [-d] [options] INPUT [-o] OUTPUT\n"
	"\n"
	"Options:\n"
	"  -h, --help             show this help message and exit\n"
	"  -z, --compress         compress input file [default action]\n"
	"  -d, --decompress       decompress input file\n"
	"  -o FILE, --output=FILE\n"
	"                         output to FILE instead of second positional argument\n"
	"  -c --stdout            use standard input/output when filenames are absent\n"
	"  -f, --force            overwrite output without prompting\n"
	"  -q, --quiet            suppress error messages\n"
	"  -v, --verbose          show completion stats\n"
//	"  --no-bit-flip          don't encode bit rotated blocks\n"
//	"  --cycle-limit INT      limits the 6502 decoding time for each encoded block\n"
;

static int verbosity_level = 0;
static void fatal_error(const char *msg)
{
	if (verbosity_level >= 0)
		fputs(msg, stderr);
	exit(EXIT_FAILURE);
}

static void fatal_perror(const char *filename)
{
	if (verbosity_level >= 0)
		perror(filename);
	exit(EXIT_FAILURE);
}

// According to a strace of cat on my system, and a quick dd of dev/zero:
// 131072 is the optimal block size. That's 2 times the size of the entire
// 6502 address space, so that should be enough
#define BUF_IO_SIZE 131072
#define BUF_GAP_SIZE 512

int main (int argc, char **argv)
{
	int c;
	char *input_filename = NULL;
	char *output_filename = NULL;
	FILE *input_file = NULL;
	FILE *output_file = NULL;
	bool decompress = false;
	bool force_overwrite = false;
	bool use_stdio_for_data = false;
//	bool no_bit_flip_blocks = false;
//	bool interleaved_dont_care_bits = false;
	uint8_t input_buffer[BUF_IO_SIZE+BUF_GAP_SIZE];
	int input_buffer_length = 0;
	uint8_t output_buffer[BUF_IO_SIZE+BUF_GAP_SIZE];
	int output_buffer_length = 0;

	int total_bytes_in = 0;
	int total_bytes_out = 0;
	float total_bytes_ratio = 0.0;

	int i, l;

//	int cycle_limit = 10000;

	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	while (1) {
		static struct option long_options[] =
		{
			{"help",        no_argument,       NULL, 'h'},
			{"compress",    no_argument,       NULL, 'z'},
			{"decompress",  no_argument,       NULL, 'd'},
			{"output",      required_argument, NULL, 'o'},
			{"stdout",      no_argument,       NULL, 'c'},
			{"force",       no_argument,       NULL, 'f'},
			{"verbose",     no_argument,       NULL, 'v'}, /* to be used */
			{"quiet",       no_argument,       NULL, 'q'},
//			{"no-bit-flip", no_argument,       NULL, 'b'+256},
//			{"cycle-limit", required_argument, NULL, 'y'+256},
//			{"interleaved-dont-care-bits", no_argument, NULL, 'd'+256},
			{NULL, 0, NULL, 0}
		};
		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long(argc, argv, "hVzdo:cfvq",
						long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			fputs(USAGE_TEXT, stdout);
			exit(EXIT_SUCCESS);

		break; case 'z':
			decompress = false;

		break; case 'd':
			decompress = true;

		break; case 'o':
			output_filename = optarg;

		break; case 'c':
			use_stdio_for_data = true;

		break; case 'f':
			force_overwrite = true;

		break; case 'v':
			if (verbosity_level >= 0)
				++verbosity_level;

		break; case 'q':
			verbosity_level = -1;
			opterr = 0;

//		break; case 'b'+256:
//			no_bit_flip_blocks = true;

//		break; case 'y'+256:
//			cycle_limit = strtol(optarg, NULL, 0);

//		break; case 'd'+256:
//			interleaved_dont_care_bits = true;

		break; case '?':
			/* getopt_long already printed an error message. */
			exit(EXIT_FAILURE);

		break; default:
		break;
		}
	}

	if (verbosity_level < 0) {
		fclose(stderr);
	}

//	if (cycle_limit < 1268) {
//		fatal_error("Invalid parameter for --cycle-limit. Must be a integer >= 1268.\n");
//	}

	if ((input_filename == NULL) && (optind < argc)) {
		input_filename = argv[optind];
		++optind;
	}

	if ((output_filename == NULL) && (optind < argc)) {
		output_filename = argv[optind];
		++optind;
	}

	if ((input_filename == NULL) && (output_filename == NULL) && (!use_stdio_for_data)) {
		fatal_error("Input and output filenames required. Try --help for more info.\n");
	}

	if (input_filename == NULL) {
		if (use_stdio_for_data) {
			input_file = stdin;
		} else {
			fatal_error("input filename required. Try --help for more info.\n");
		}
	}

	if (output_filename == NULL) {
		if (use_stdio_for_data) {
			output_file = stdout;
		} else {
			fatal_error("output filename required. Try --help for more info.\n");
		}
	}

	if (output_filename != NULL) {
		fclose(stdout);
		if (!force_overwrite) {
			/* open output for read to check for file existence. */
			output_file = fopen(output_filename, "rb");
			if (output_file != NULL) {
				fclose(output_file);
				output_file = NULL;
				if (verbosity_level >= 0) {
					fputs(output_filename, stderr);
					fputs(" already exists;", stderr);
					if (!use_stdio_for_data) {
						fputs(" do you wish to overwrite (y/N) ? ", stderr);
						c = fgetc(stdin);
						if (c != '\n') {
							while (true) {
								if (fgetc(stdin) == '\n')
									break; /* read until the newline */
							}
						}
						if (c == 'y' || c == 'Y') {
							force_overwrite = true;
						} else {
							fputs("    not overwritten\n", stderr);
						}
					} else {
						fputs(" not overwritten\n", stderr);
					}
				}
			}
		}
		if ((errno == ENOENT) || (force_overwrite)) {
			/* "No such file or directory" means the name is usable */
			errno = 0;
			output_file = fopen(output_filename, "wb");
			if (output_file == NULL) {
				fatal_perror(output_filename);
			}
			setvbuf(output_file, NULL, _IONBF, 0);
		} else {
			/* error message printed above */
			exit(EXIT_FAILURE);
		}
	} else {
		output_filename = "<stdout>";
	}

	if (input_filename != NULL) {
		fclose(stdin);
		input_file = fopen(input_filename, "rb");
		if (input_file == NULL) {
			fatal_perror(input_filename);
		}
		setvbuf(input_file, NULL, _IONBF, 0);
	} else {
		input_filename = "<stdin>";
	}

	bool done = false;
	while(1) {
		if (input_buffer_length < BUF_GAP_SIZE) {
			l = fread(input_buffer + input_buffer_length, sizeof(uint8_t), BUF_IO_SIZE, input_file);
			total_bytes_in += l;
			if (ferror(input_file)) {
				fatal_perror(input_filename);
			}
			input_buffer_length += l;
		}

		if (decompress) {
			l = donut_decompress(output_buffer + output_buffer_length, BUF_IO_SIZE+BUF_GAP_SIZE - output_buffer_length, input_buffer, input_buffer_length, &i);
		} else {
			l = donut_compress(output_buffer + output_buffer_length, BUF_IO_SIZE+BUF_GAP_SIZE - output_buffer_length, input_buffer, input_buffer_length, &i);
		}
		output_buffer_length += l;
		if ((l == 0) && (i == 0))
			done = true;

		if (input_buffer_length - i > 0) {
			memmove(input_buffer, input_buffer + i, input_buffer_length - i);
		}
		input_buffer_length -= i;

		if (output_buffer_length >= BUF_IO_SIZE) {
			l = fwrite(output_buffer, sizeof(uint8_t), BUF_IO_SIZE, output_file);
			total_bytes_out += l;
			if (ferror(output_file)) {
				fatal_perror(output_filename);
			}
			if (output_buffer_length - l > 0) {
				memmove(output_buffer, output_buffer + l, output_buffer_length - l);
			}
			output_buffer_length -= l;
		}

		if (feof(input_file) || done) {
			if (output_buffer_length) {
				l = fwrite(output_buffer, sizeof(uint8_t), output_buffer_length, output_file);
				total_bytes_out += l;
				if (ferror(output_file)) {
					fatal_error(output_filename);
				}
			}
			break;
		}
	}

	if (input_file != NULL) {
		fclose(input_file);
	}

	if (output_file != NULL) {
		fclose(output_file);
	}

	if (verbosity_level >= 1) {
		if (decompress) {
			if (total_bytes_out != 0) {
				total_bytes_ratio = (1.0 - ((float)total_bytes_in / (float)total_bytes_out))*100.0;
			}
		} else {
			if (total_bytes_in != 0) {
				total_bytes_ratio = (1.0 - ((float)total_bytes_out / (float)total_bytes_in))*100.0;
			}
		}
		fprintf (stderr, "%s :%#5.1f%% (%d => %d bytes)\n", output_filename, total_bytes_ratio, total_bytes_in, total_bytes_out);
	}

	exit(EXIT_SUCCESS);
}

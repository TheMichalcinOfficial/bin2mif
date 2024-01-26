#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      // dprintf
#include <unistd.h>     // STDOUT_FILENO, STDERR_FILENO
#include <fcntl.h>      // open, close
#include <sys/stat.h>   // struct stat, fstat

#include <stdbool.h>    // bool
#include <string.h>     // strcmp, memcpy
#include <stdint.h>     // uint8_t, UINT8_MAX
#include <stdlib.h>     // EXIT_SUCCESS, NULL, strtol, strtoull

#include <err.h>        // err, errx, warn, warnx
#include <errno.h>      // errno, ERANGE

#include <getopt.h>     // getopt_long

//////////////////////////////////// Typedefs /////////////////////////////////

typedef uint8_t byte;

/////////////////////////////////// Constants /////////////////////////////////

#define INPUT_BUFFER_SIZE 128   // words

static const char *HELP_MESSAGE = \
    "Usage: bin2mif [OPTIONS] [in_file]\n"
    "-w, --width <WIDTH>\thas to be a multiple of 8\t\t(default is 8 bits)\n"
    "-d, --depth <DEPTH>\tnumber of words, each <WIDTH> bits wide"
        "\t(default is the input file size)\n"
    "-o, --output <FILE>\twrite output to file\t\t\t(default is stdout)\n"
    "-h, --help\t\tview this message\n";

static struct option LONG_OPTIONS[] = {
    /*   NAME      ARGUMENT           FLAG  SHORTNAME */
        {"width",  required_argument, NULL, 'w'},
        {"depth",  required_argument, NULL, 'd'},
        {"output", required_argument, NULL, 'o'},
        {"help",   no_argument,       NULL, 'h'},
        {NULL,     0,                 NULL, 0}
    };

static const char *OPTSTRING = "w:d:o:h";

//////////////////////////////////// Errors ///////////////////////////////////

#define BAD_NUMBER_FORMAT    1
#define OVERFLOW_ERROR       2
#define INVALID_ARGUMENTS    3
#define FILE_OPEN_FAILURE    4
#define FILE_CLOSE_FAILURE   5
#define GENRATOR_FAILURE     6
#define GENERATOR_EARLY_STOP 7

static const char *ERROR_MSG[] =
{
    "no error",
    "bad number format",
    "integer variable range overflow",
    "invalid command line arguments",
    "failed to open file \"%s\"",
    "failed to close file \"%s\"",
    "failed to generate .mif file",
    "%lld words were requested, but only %lld could be generated",
    NULL
};

////////////////////////////////// Utilities //////////////////////////////////

byte str_to_byte(const char *str)
{
    char *end = NULL;
    long num = strtol(str, &end, 10);

    if (*end != '\0') { errx(BAD_NUMBER_FORMAT, ERROR_MSG[BAD_NUMBER_FORMAT]); }

    if (num < 0 || num > UINT8_MAX) { errx(OVERFLOW_ERROR, ERROR_MSG[OVERFLOW_ERROR]); }

    return (byte) num;
}

long long str_to_ll(const char *str)
{
    errno = 0;

    char *end = NULL;
    long long num = strtoll(str, &end, 10);

    if (*end != '\0') { errx(BAD_NUMBER_FORMAT, ERROR_MSG[BAD_NUMBER_FORMAT]); }

    if (errno == ERANGE) { err(OVERFLOW_ERROR, ERROR_MSG[OVERFLOW_ERROR]); }

    return num;
}

static inline bool safe_close(int *fd)
{
    if (fd == NULL || *fd == -1) { return true; }
    
    bool retval = (close(*fd) == 0);
    *fd = -1;
    return retval;
}

ssize_t read_aligned(int fd, void *dest, size_t nwords, byte word_size,
                     void *put_aside, byte *remainder_len)
{
    size_t nbytes = nwords * word_size;

    if (put_aside != NULL && remainder_len != NULL && *remainder_len > 0)
    {
        memcpy(dest, put_aside, *remainder_len);
        dest += *remainder_len;
    }

    ssize_t bytes_read = read(fd, dest, nbytes - *remainder_len);
    if (bytes_read < 0) { return -1; }
    bytes_read += *remainder_len;

    ssize_t words_read = bytes_read / word_size;
    *remainder_len = bytes_read % word_size;

    memcpy(dest + nbytes - *remainder_len, put_aside, *remainder_len);
    return words_read;
}

unsigned int num_len(unsigned long long num, byte base)
{
    unsigned int len = 1;
    num /= base;
    while (num > 0) { num /= base; ++len; }
    return len;
}

/*
* Return value:
* -1 if an error is encountered
* -2 if the file is not a regular file
* <n> where <n> is the size of the file in bytes
*/
off_t file_size(int fd)
{
    struct stat file_stat;

    if (fstat(fd, &file_stat) == -1) { return -1; }
    else if (!S_ISREG(file_stat.st_mode)) { return -2; }

    return file_stat.st_size;
}

////////////////////////////////// Generator //////////////////////////////////

static inline bool generate_mif_header(int out_fd, long long depth, byte width)
{
    static const char *ADDRESS_RADIX = "HEX";
    static const char *DATA_RADIX    = "HEX";

    return (dprintf(out_fd, "DEPTH = %lld;\n"
                            "WIDTH = %d;\n"
                            "ADDRESS_RADIX = %s;\n"
                            "DATA_RADIX = %s;\n"
                            "CONTENT\n"
                            "BEGIN\n",
                    depth, width, ADDRESS_RADIX, DATA_RADIX) > 0);
}

long long generate_mif_content(int in_fd, int out_fd, long long depth, byte width)
{
    const byte word_size = width / 8;
    // const size_t buffer_size = INPUT_BUFFER_SIZE * word_size;
    const unsigned int addr_repr_width = num_len(depth - 1, 16);

    byte buffer[INPUT_BUFFER_SIZE][word_size];
    ssize_t words_read = 0;

    byte put_aside_buffer[word_size];
    byte remainder_len = 0;

    size_t word_idx = 0;
    for (long long addr = 0; addr < depth; ++addr)
    {
        if (words_read == 0)
        {
            word_idx = 0;
            words_read = read_aligned(in_fd, buffer, INPUT_BUFFER_SIZE,
                                      word_size, put_aside_buffer,
                                      &remainder_len
            );
            if (words_read < 0)
            {
                warn("reading binary words from file");
                return addr;
            }
        }
        if (words_read == 0 && remainder_len == 0)
        {
            warnx("unexpected EOF");
            return addr;
        }

        if (dprintf(out_fd, "%0*llx : ", addr_repr_width, addr) < 0)
        {
            warn("writing record to output");
            return addr;
        }
        for (short byte_idx = word_size - 1; byte_idx >= 0; --byte_idx)
        {
            if (dprintf(out_fd, byte_idx == 0 ? "%02x;\n" : "%02x",
                        buffer[word_idx][byte_idx]) < 0)
            {
                warn("writing record to output");
                return addr;
            }
        }
        ++word_idx;
        --words_read;
    }

    return depth;
}

long long generate_mif(int in_fd, int out_fd, long long depth, byte width)
{
    const long long bytes_requested = depth * width / 8;
    off_t in_file_size = file_size(in_fd);

    // Argument validation
    if (in_file_size == -1)                     // system error
    {
        warn("getting file size");
        return -1;
    }
    if (in_file_size == -2 && depth < 0)        // reading from stdin; depth unknown
    {
        errno = EINVAL;
        warn("memory depth has to be given when reading from stdin");
        return -1;
    }
    if (depth < 0) { depth = in_file_size; }    // desired depth equals the file size
    else if (in_file_size != -2 &&
             in_file_size < bytes_requested)    // file is too short
    {
        warnx("%lld bytes were requested, but the input file only contains %ld",
             bytes_requested, in_file_size);
    }

    if (!generate_mif_header(out_fd, depth, width)) { return -1; }

    // Fill in the content
    long long word_count = generate_mif_content(in_fd, out_fd, depth, width);
    if (word_count < 0) { return -1; }

    // End file
    if (dprintf(out_fd, "END;\n") < 0)
    {
        warn("ending .mif file");
        return -1;
    }

    return word_count;
}

//////////////////////////////////// Main /////////////////////////////////////

int main(int argc, char *argv[])
{
    // Command line parameters
    long long depth = -1;
    byte width = 8;
    const char *in_filename = "-";
    const char *out_filename = NULL;

    // Parse command line arguments
    char chr = '\0';
    while ((chr = getopt_long(argc, argv, OPTSTRING, LONG_OPTIONS, NULL)) >= 0)
    {
        switch (chr)
        {
        case 'w':
            width = str_to_byte(optarg);
            break;

        case 'd':
            depth = str_to_ll(optarg);
            break;

        case 'o':
            out_filename = optarg;
            break;

        case 'h':
            (void) dprintf(STDERR_FILENO, HELP_MESSAGE);
            return EXIT_SUCCESS;

        case '?':
        default:
            (void) dprintf(STDERR_FILENO, "\n%s", HELP_MESSAGE);
            return INVALID_ARGUMENTS;
        }
    }

    if (optind < argc && argc - optind == 1) { in_filename = argv[optind++]; }
    else if (optind < argc) { err(INVALID_ARGUMENTS, ERROR_MSG[INVALID_ARGUMENTS]); }

    // Open files
    int in_fd = (strcmp(in_filename, "-") != 0
                 ? open(in_filename, O_RDONLY)
                 : STDIN_FILENO
    );

    if (in_fd < 0)
    {
        err(FILE_OPEN_FAILURE, ERROR_MSG[FILE_OPEN_FAILURE], in_filename);
    }

    int out_fd = (out_filename != NULL
                  ? open(out_filename, O_WRONLY | O_TRUNC | O_CREAT, 0666)
                  : STDOUT_FILENO
    );

    if (out_fd < 0)
    {
        int saved_errno = errno;
        (void) safe_close(&in_fd);

        errno = saved_errno;
        err(FILE_OPEN_FAILURE, ERROR_MSG[FILE_OPEN_FAILURE], out_filename);
    }

    // Generate .mif file
    long long words_written = generate_mif(in_fd, out_fd, depth, width);
    if (words_written != depth)
    {
        int saved_errno = errno;
        (void) safe_close(&in_fd);
        (void) safe_close(&out_fd);

        errno = saved_errno;
        if (words_written < 0) { exit(GENRATOR_FAILURE); }
    }

    int retval = 0;

    // Free resources
    if (in_filename != NULL && !safe_close(&in_fd))
    {
        retval = FILE_CLOSE_FAILURE;
        warn("closing file %s", in_filename);
    }

    if (out_filename != NULL && !safe_close(&out_fd))
    {
        retval = FILE_CLOSE_FAILURE;
        warn("closing file %s", out_filename);
    }

    return retval;
}

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>      // dprintf
#include <unistd.h>     // STDOUT_FILENO
#include <fcntl.h>      // open, close

#include <stdbool.h>    // bool
#include <string.h>     // strcmp, memcpy
#include <stdint.h>     // uint8_t, UINT8_MAX
#include <stdlib.h>     // NULL, strtol, strtoull

#include <err.h>        // err, errx, warn, warnx
#include <errno.h>      // errno, ERANGE

//////////////////////////////////// Typedefs /////////////////////////////////

typedef uint8_t byte;

/////////////////////////////////// Constants /////////////////////////////////

#define INPUT_BUFFER_SIZE 128   // words

static const char *HELP_MESSAGE = \
    "Usage: bin2mif <DEPTH> <WIDTH> <in_file> <out_file>\n"
    "DEPTH - number of words, each <WIDTH> bits wide\n"
    "WIDTH - has to be a multiple of 8\n";

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
    "failed to open file",
    "failed to generate .mif file",
    "%lld words were requested, but only %lld could be generated"
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
        else if (words_read == 0 && remainder_len == 0)
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
    long long depth = 0;
    byte width = 0;
    const char *in_filename = NULL;
    const char *out_filename = NULL;

    // Parse command line arguments
    if (argc < 3 || argc > 5 ||
        strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        (void) dprintf(STDOUT_FILENO, HELP_MESSAGE);
    }
    if (argc < 2) { errx(INVALID_ARGUMENTS, ERROR_MSG[INVALID_ARGUMENTS]); }
    
    if (argc >= 3)
    {
        depth = str_to_ll(argv[1]);
        width = str_to_uint8_t(argv[2]);
    }
    if (argc >= 4)  { in_filename  = argv[3]; }
    if (argc == 5)  { out_filename = argv[4]; }

    // Open files
    int in_fd = in_filename != NULL ? open(in_filename, O_RDONLY) : STDIN_FILENO;
    if (in_fd < 0) { err(FILE_OPEN_FAILURE, ERROR_MSG[FILE_OPEN_FAILURE]); }

    int out_fd = (out_filename != NULL
                  ? open(out_filename, O_WRONLY | O_TRUNC | O_CREAT, 0666)
                  : STDOUT_FILENO
    );
    if (out_fd < 0)
    {
        int saved_errno = errno;
        (void) safe_close(&in_fd);

        errno = saved_errno;
        err(FILE_OPEN_FAILURE, ERROR_MSG[FILE_OPEN_FAILURE]);
    }

    // Generate .mif file
    long long words_written = generate_mif(in_fd, out_fd, depth, width);
    if (words_written != depth)
    {
        int saved_errno = errno;
        (void) safe_close(&in_fd);
        (void) safe_close(&out_fd);

        errno = saved_errno;
        if (words_written < 0) { errx(GENRATOR_FAILURE, ERROR_MSG[GENRATOR_FAILURE]); }
        else
        {
            errx(GENERATOR_EARLY_STOP, ERROR_MSG[GENERATOR_EARLY_STOP],
                 depth, words_written);
        }
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

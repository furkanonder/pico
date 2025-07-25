#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

typedef char* string;

/**
 * Macro to convert a letter to its corresponding Control key code
 * - toupper(k) converts the input character to uppercase (A-Z)
 * - Subtracts 'A' (65) to get position in alphabet (0-25)
 * - Adds 1 to get Control key code (1-26)
 * Available keys: Only works for letters A-Z (case insensitive)
 * Examples: CONTROL_KEY('a') -> 1 (Ctrl+A), CONTROL_KEY('q') -> 17 (Ctrl+Q)
 */
#define CONTROL_KEY(k) (toupper(k) - 'A' + 1)

struct termios orig_termios;   // Saved original terminal settings for restoration on exit

static inline void clear_screen(void) {
    // Clear the screen and move the cursor to the top-left corner
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

void fatal(const string s) {
    clear_screen();
    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        fatal("Failed to restore terminal state");
    }
}

void enable_raw_mode() {
    // Save current terminal state for later restoration
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        fatal("Failed to get terminal attributes");
    }
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;

    // Configure local mode flags (c_lflag)
    raw.c_lflag &= ~(ECHO   |   // Disable character echoing
                    ICANON  |   // Disable canonical mode (read byte-by-byte)
                    IEXTEN  |   // Disable extended functions (disable Ctrl-V)
                    ISIG);      // Disable signal generation (disable Ctrl-C and Ctrl-Z)
    // Configure input mode flags (c_iflag)
    raw.c_iflag &= ~(BRKINT     |   // Disable break signaling
                    ICRNL       |   // Disable CR-to-NL conversion
                    INPCK       |   // Disable parity checking
                    ISTRIP      |   // Disable 8th bit stripping
                    IXON);          // Disable flow control (disable Ctrl-S and Ctrl-Q)
    // Configure output mode flags (c_oflag)
    raw.c_oflag &= ~(OPOST);   // Disable output processing
    // Configure control mode flags (c_cflag)
    raw.c_cflag |= CS8;        // Set 8-bit character size
    // Configure control characters
    raw.c_cc[VMIN] = 0;        // Non-blocking read (return immediately)
    raw.c_cc[VTIME] = 1;       // 100ms timeout for read operations

    // Apply the new terminal settings
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        fatal("Failed to enable special input mode");
    }
}

int read_key() {
    int bytes_read;
    char ch;

    bytes_read = read(STDIN_FILENO, &ch, 1);
    if (bytes_read == -1 && errno != EAGAIN) {
        perror("Failed to read from stdin");
        exit(EXIT_FAILURE);
    }
    if (bytes_read == 0) {
        return -1;
    }

    return ch;
}

int main() {
    enable_raw_mode();
    int c;

    while (1) {
        c = read_key();
        if (c != -1) {
            if (c == CONTROL_KEY('q')) {
                exit(0);
            }
            else if (isprint(c)) {
                printf("Key: %d - %c\r\n", c, c);
            }
            else {
                printf("%d\r\n", c);
            }
        }
    }

    return 0;
}

#define _DEFAULT_SOURCE

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>

typedef char* string;
typedef char** string_list;

/**
 * Macro to convert a letter to its corresponding Control key code
 * - toupper(k) converts the input character to uppercase (A-Z)
 * - Subtracts 'A' (65) to get position in alphabet (0-25)
 * - Adds 1 to get Control key code (1-26)
 * Available keys: Only works for letters A-Z (case insensitive)
 * Examples: CONTROL_KEY('a') -> 1 (Ctrl+A), CONTROL_KEY('q') -> 17 (Ctrl+Q)
 */
#define CONTROL_KEY(k) (toupper(k) - 'A' + 1)

// Custom arrow key codes (beyond ASCII range to avoid collisions)
#define ARROW_UP    500     // "\x1b[A" - Up arrow sequence
#define ARROW_DOWN  501     // "\x1b[B" - Down arrow sequence
#define ARROW_LEFT  502     // "\x1b[D" - Left arrow sequence
#define ARROW_RIGHT 503     // "\x1b[C" - Right arrow sequence

#define ENTER       13      // \r (Carriage Return)
#define BACKSPACE   127     // DEL (Delete)
#define ESC         '\x1b'  // Escape character (0x1B) - starts ANSI sequences

#define INITIAL_CAP 128     // Initial buffer capacity for new lines

// Represents a single line of text in the editor using a doubly-linked list
typedef struct line {
    string text;            // Dynamic buffer containing line text
    int len;                // Current length of text in line
    int cap;                // Allocated capacity of line buffer
    struct line* next;      // Pointer to next line (forward traversal)
    struct line* prev;      // Pointer to previous line (backward traversal)
} line_t;

// GLOBAL STATE VARIABLES

line_t* first_line = NULL;     // Head of doubly-linked list containing all file lines
line_t* current_line = NULL;   // Pointer to the line where cursor is currently positioned
string filename = NULL;        // Name of currently opened file

struct termios orig_termios;   // Saved original terminal settings for restoration on exit

int term_rows = 24, term_cols = 80; // Terminal dimensions (updated dynamically)
int cursor_row = 0, cursor_col = 0; // Cursor position in FILE coordinates (0-based)
int viewport_col = 0;               // First visible column number (horizontal scroll offset)
int viewport_row = 0;               // First visible line number (vertical scroll offset)
int max_viewport_col = 0;           // Maximum horizontal scroll position (term_cols - margin)

volatile sig_atomic_t resize_pending = 0;  // Flag to indicate terminal resize is pending

static inline void clear_screen(void) {
    // Clear the screen and move the cursor to the top-left corner
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

static inline void move_cursor_to(int row, int col) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    write(STDOUT_FILENO, buf, strlen(buf));
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

void get_window_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback to default dimensions if ioctl fails
        term_rows = 24;
        term_cols = 80;
    }
    else {
        term_rows = ws.ws_row - 1;  // Reserve bottom row for status line
        term_cols = ws.ws_col;
    }
    max_viewport_col = term_cols - 10;  // Leave margin for horizontal scrolling
}

void handle_sigwinch(int sig) {
    (void)sig;  // Suppress unused parameter warning
    resize_pending = 1;
}

void setup_signals() {
    if (signal(SIGWINCH, handle_sigwinch) == SIG_ERR) {
        fatal("Failed to set up SIGWINCH handler");
    }
}

line_t* new_line() {
    line_t* line = malloc(sizeof(line_t));
    if (line == NULL) {
        fatal("Failed to allocate memory for line");
    }
    line->text = malloc(INITIAL_CAP * sizeof(char));
    if (line->text == NULL) {
        free(line);
        fatal("Failed to allocate memory for line content");
    };
    line->text[0] = '\0';
    line->len = 0;
    line->cap = INITIAL_CAP;
    line->next = line->prev = NULL;
    return line;
}

line_t* delete_line(line_t* line_to_delete, line_t** first_line_ptr) {
    line_t* move_to = NULL;
    // Determine where to move cursor after deletion
    if (line_to_delete->prev != NULL) {
        move_to = line_to_delete->prev;
    }
    else if (line_to_delete->next != NULL) {
        move_to = line_to_delete->next;
        // Update the first line if deleting the first_line
        *first_line_ptr = move_to;
    }
    else {
        // This is the only line - don't delete it, just clear it
        line_to_delete->len = 0;
        line_to_delete->text[0] = '\0';
        return line_to_delete;
    }
    // Update linked list connections
    if (line_to_delete->prev != NULL) {
        line_to_delete->prev->next = line_to_delete->next;
    }
    if (line_to_delete->next != NULL) {
        line_to_delete->next->prev = line_to_delete->prev;
    }
    // Update first_line pointer if we're deleting the first line
    if (*first_line_ptr == line_to_delete) {
        *first_line_ptr = line_to_delete->next;
    }
    // Free allocated memory
    free(line_to_delete->text);
    free(line_to_delete);
    return move_to;
}

void link_new_line(line_t* current_line, line_t* new_line) {
    new_line->prev = current_line;
    new_line->next = current_line->next;
    // Update next line's back pointer if it exists
    if (current_line->next != NULL) {
        current_line->next->prev = new_line;
    }
    current_line->next = new_line;
}

void expand_line(line_t* line, int needed) {
    if (line->len + needed >= line->cap) {
        line->cap = (line->len + needed) * 2;
        string new_content = realloc(line->text, line->cap);
        if (!new_content) {
            fatal("Failed to reallocate line capacity");
        }
        line->text = new_content;
    }
}

void insert_newline() {
    line_t* new = new_line();
    // If cursor is not at end of line, split the line
    if (cursor_col < current_line->len) {
        // Split line: move tail text to new line
        int tail_len = current_line->len - cursor_col;
        expand_line(new, tail_len);
        // Copy text from cursor position to end of line
        memcpy(new->text, current_line->text + cursor_col, tail_len);
        new->len = tail_len;
        new->text[tail_len] = '\0';
        // Truncate current line at cursor position
        current_line->len = cursor_col;
        current_line->text[current_line->len] = '\0';
    }
    // Link the new line into file structure
    link_new_line(current_line, new);
    current_line = new;
    cursor_col = 0;
    cursor_row++;
}

int get_total_lines() {
    int total_lines = 0;
    line_t* count_line = first_line;
    while (count_line) {
        total_lines++;
        count_line = count_line->next;
    }
    return total_lines;
}

void delete_char() {
    // Character deletion (cursor in middle or end of line)
    if (cursor_col > 0 && current_line->len > 0) {
        memmove(&current_line->text[cursor_col - 1], &current_line->text[cursor_col],
                current_line->len - cursor_col);
        cursor_col--;
        current_line->text[--current_line->len] = '\0';
    }
    // line merging (cursor at beginning of line)
    else if (cursor_col == 0 && cursor_row > 0) {
        line_t* prev = current_line->prev;
        if (prev != NULL) {
            int prev_len = prev->len;
            expand_line(prev, current_line->len);
            // Copy current line's content to end of previous line
            memcpy(&prev->text[prev_len], current_line->text, current_line->len);
            // Update previous line metadata
            prev->len += current_line->len;
            prev->text[prev->len] = '\0';
            // Remove current line from file structure
            current_line = delete_line(current_line, &first_line);
            cursor_row--;
            cursor_col = prev_len;  // Position cursor at merge point
        }
    }
}

void insert_char(char c) {
    expand_line(current_line, 1);
    // Shift text right from cursor position
    memmove(&current_line->text[cursor_col + 1], &current_line->text[cursor_col],
            current_line->len - cursor_col);
    current_line->text[cursor_col++] = c;
    current_line->text[++current_line->len] = '\0';
}

void read_file(string filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        fatal("read_file: fopen failed");
    }
    line_t* current_line = first_line;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            line_t* new = new_line();
            link_new_line(current_line, new);
            current_line = new;
        }
        else {
            expand_line(current_line, 1);
            current_line->text[current_line->len++] = c;
            current_line->text[current_line->len] = '\0';
        }
    }
    fclose(f);
    current_line = first_line; // Reset to first line
}

void save_file() {
    FILE* f = fopen(filename, "w");
    if (!f) {
        fatal("save_file: fopen failed");
    }
    line_t* line = first_line;
    if (line) {
        do {
            fwrite(line->text, 1, line->len, f);
              // Only add newline if there's another line
            line = line->next;
            if (line) {
                fputc('\n', f);
            }
        } while (line);
    }
    fclose(f);
}

void draw_status() {
   move_cursor_to(term_rows + 1, 1);    // Move cursor to the status line (bottom row of terminal)
   write(STDOUT_FILENO, "\033[2K", 4);  // Clear the entire status line first
   write(STDOUT_FILENO, "\033[7m", 4);  // Reverse video (inverted colors)
   // Prepare and write the status message
   char status[128];
   int len = snprintf(status, sizeof(status), "Line: %d Col: %d [%dx%d]", cursor_row + 1,
                      cursor_col + 1, term_cols, term_rows);
   write(STDOUT_FILENO, status, len);
   // Fill the rest of the status line with spaces to ensure full background
   for (int i = len; i < term_cols; i++) {
       write(STDOUT_FILENO, " ", 1);
   }
   write(STDOUT_FILENO, "\033[0m", 4); // Reset formatting back to normal (disable reverse video)
}

void check_scroll() {
    // Adjust vertical viewport to keep cursor visible
    if (cursor_row < viewport_row) {
        viewport_row = cursor_row;  // Scroll up
    }
    else if (cursor_row >= viewport_row + term_rows) {
        viewport_row = cursor_row - term_rows + 1;  // Scroll down
    }
    // Adjust horizontal viewport for long lines
    if (current_line) {
        if (cursor_col < viewport_col) {
            viewport_col = cursor_col;  // Scroll left
        }
        else if (cursor_col > viewport_col + max_viewport_col) {
            viewport_col = cursor_col - max_viewport_col;  // Scroll right
        }
        if (viewport_col < 0) {
            viewport_col = 0;  // Prevent negative scrolling
        }
    }
}

void draw_rows() {
    int screen_row = 0;                // Current row being rendered (0 to term_rows - 1)
    line_t* temp_curr = first_line;    // Line iterator
    current_line = first_line;         // Reset current_line pointer

    // Skip lines above the viewport (scroll offset)
    for (int i = 0; i < viewport_row && temp_curr; i++) {
        temp_curr = temp_curr->next;
    }
    // Render visible lines within the terminal window
    while (temp_curr && screen_row < term_rows) {
        int line_len = temp_curr->len;
        // Calculate horizontal clipping boundaries
        int start = viewport_col;
        if (start > line_len) {
            start = line_len;  // Don't start beyond line end
        }
        int end = start + term_cols;
        if (end > line_len) {
            end = line_len;    // Don't read beyond line end
        }
        // Calculate and write visible portion
        int visible_len = end - start;
        if (visible_len > 0) {
            write(STDOUT_FILENO, temp_curr->text + start, visible_len);
        }
        write(STDOUT_FILENO, "\r\n", 2);  // Move to next row
        // Update current_line when we find the cursor's line
        if (cursor_row == viewport_row + screen_row) {
            current_line = temp_curr;
        }
        temp_curr = temp_curr->next;
        screen_row++;
    }
    // Fill remaining screen rows with tildes
    while (screen_row < term_rows) {
        write(STDOUT_FILENO, "~\r\n", 3);
        screen_row++;
    }
    // Clamp cursor position to valid file bounds
    int total_lines = get_total_lines();
    if (cursor_row >= total_lines) {
        cursor_row = total_lines - 1;  // Don't go past last line
    }
    if (cursor_row < 0) {
        cursor_row = 0;               // Don't go before first line
    }
    if (current_line && cursor_col > current_line->len) {
        cursor_col = current_line->len;  // Don't go past line end
    }

}

void process_input(int c) {
    if (c == CONTROL_KEY('q')) {
        exit(0);
    }
    else if (c == CONTROL_KEY('s')) {
        save_file();
    }
    else {
        switch (c) {
            case ARROW_UP:
                if (cursor_row > 0) {
                    cursor_row--;
                }
                break;
            case ARROW_DOWN:
                if (current_line && current_line->next) {
                    cursor_row++;
                }
                break;
            case ARROW_LEFT:
                if (cursor_col > 0) {
                    cursor_col--;
                }
                else if (cursor_row > 0) {
                    // Wrap to end of previous line
                    cursor_row--;
                    cursor_col = current_line->prev->len;
                }
                break;
            case ARROW_RIGHT:
                if (current_line && cursor_col < current_line->len) {
                    cursor_col++;
                }
                else if (current_line && current_line->next) {
                    // Wrap to start of next line
                    cursor_row++;
                    cursor_col = 0;
                }
                break;
            case ENTER:
                insert_newline();
                break;
            case BACKSPACE:
                delete_char();
                break;
            default:
                if (isprint(c)) {
                    insert_char(c);
                }
                break;
        }
    }
}

int read_key() {
    char ch;
    int bytes_read = read(STDIN_FILENO, &ch, 1);

    if (bytes_read == -1 && errno != EAGAIN) {
        fatal("Failed to read from stdin");
    }
    if (bytes_read == 0) {
        return -1;
    }

    if (ch == ESC) {
        char sequence[2];
        if (read(STDIN_FILENO, &sequence[0], 1) != 1) {
            return ESC;
        }
        if (read(STDIN_FILENO, &sequence[1], 1) != 1) {
            return ESC;
        }
        if (sequence[0] == '[') {
            switch (sequence[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
            }
        }
        return ESC;
    }

    return ch;

}

void refresh_screen() {
    if (resize_pending) {
        resize_pending = 0;
        get_window_size();
    }
    clear_screen();
    check_scroll();
    draw_rows();
    draw_status();
    move_cursor_to(cursor_row - viewport_row + 1, cursor_col - viewport_col + 1);
}

int main(int argc, string_list argv) {
    setup_signals();
    enable_raw_mode();
    get_window_size();

    if (argc != 2) {
        printf("Usage: ./pico <file>\n");
        exit(1);
    }
    current_line = first_line = new_line();
    filename = argv[1];
    if (access(filename, F_OK) == 0) {
        read_file(filename);
    }

    refresh_screen(); // Initial render
    int c;
    while (1) {
        if (resize_pending) {
            refresh_screen();
        }
        c = read_key();
        if (c != -1) {
            process_input(c);
            refresh_screen();
        }
        // 10 ms delay to prevent excessive CPU usage
        usleep(10000);
    }

    return 0;
}

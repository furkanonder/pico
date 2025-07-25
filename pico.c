#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>

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

#define ENTER       13      // \r (Carriage Return)
#define BACKSPACE   127     // DEL (Delete)

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

int cursor_row = 0, cursor_col = 0; // Cursor position in FILE coordinates (0-based)

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

void draw_rows() {
    clear_screen();
    line_t* line = first_line;
    while (line != NULL) {
        write(STDOUT_FILENO, line->text, line->len);
        // Add newline after each line (except the last one being edited)
        if (line->next != NULL) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
        line = line->next;
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
        perror("Failed to read from stdin");
        exit(EXIT_FAILURE);
    }
    if (bytes_read == 0) {
        return -1;
    }

    return ch;
}

void refresh_screen() {
    clear_screen();
    draw_rows();
}

int main(int argc, string_list argv) {
    enable_raw_mode();

    if (argc < 2) {
        printf("Usage: %s <file>\n", argv[0]);
        exit(1);
    }
    else {
        filename = argv[1];
        if (access(filename, F_OK) == 0) {
            read_file(filename);
        }
    }

    first_line = new_line();
    current_line = first_line;
    refresh_screen(); // Initial render
    int c;

    while (1) {
        c = read_key();
        if (c != -1) {
            process_input(c);
            refresh_screen();
        }
    }

    return 0;
}

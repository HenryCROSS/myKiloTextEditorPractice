/* includes */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* defines */

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* data */

// editor row
typedef struct erow
{
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

// controlling the cursor, the text, all the property of the application
struct editorConfig
{
    int cx, cy;
    int rx; // indicate the index in the render field
    // row offset, keep track of what row of the file the user is currently scrolled to
    // it decides the edge of the screen, cy would go over the screen,
    // the idea of this variable is to do the calculation with cy to calculate the
    // pos of the cursor within the screen after the cursor go through the
    // screen
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty; // if the file has been modified since opening or saving the file
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/* prototypes */

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/* terminal */

void die(const char *s)
{
    // clear entire screen
    write(STDOUT_FILENO, "\x1b[2J", 4);

    // reposite the cursor <esc>[1;1H to the top left corner
    write(STDOUT_FILENO, "\x1b[H", 3);

    // prints the descriptive error message
    perror(s);

    // indicate failure
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    // the function "disableRawMode" would be execute when the application exits
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // read the current attributes
    tcgetattr(STDIN_FILENO, &raw);

    // disable ctrl-s ctrl-q
    // "ICRNL" fix ctrl-m, CR->carriage return, NL->new line
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // turn off all processing features
    // O means ouput flag
    raw.c_oflag &= ~(OPOST);

    raw.c_cflag |= (CS8);

    // modify the struct
    // c_lflag could be thought as "miscellaneous flags"
    // ICANON flage allow us to turn off "canonical mode", which the enter key won't enter anything
    // and it will be reading input byte-by-byte, instead of line-by-line
    // "ISIG" disable Ctrl-c Ctrl-z Ctrl-y
    // "IEXTEN" disable Ctrl-v
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // c_cc is an array of controlling various terminal settings
    // set the minimum number of bytes of input needed before read() can return
    raw.c_cc[VMIN] = 0;
    // set the max amount of time to wait before read() returns.
    // set it to 1/10 of second, 100ms
    raw.c_cc[VTIME] = 1;

    // pending output to be written to the terminal and discards any input that hasn't been read.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey()
{
    int nread;
    char c;

    // read content byte by byte
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        // errno indicate what erro was
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    // if it is <esc>
    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';

                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    // get the position of cursor
    // the actual response: <esc>[24;80R, or similar
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    // parse the response from the standard input
    // think about echo -e hello > file
    // 0 -> output, 1-> standard input 2->error message
    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;

        if (buf[i] == 'R')
            break;

        i++;
    }

    buf[i] = '\0';

    // error checking
    if (buf[0] != '\x1b' || buf[1] != ']')
        return -1;

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    // buf[0] is <esc>, it needs to be skipped, otherwise, nothing would show
    /* printf("\r\n&buf[1]: '%s'\r\n", &buf[1]); */

    /* editorReadKey(); */

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    // ioctl get the size of terminal
    // TIOCGWINSZ -> Terminal Input/Output Control Get WINdow SiZe
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // C -> move the cursor to the right until reach the edge before 999 move
        // B -> move the cursor to the botton untill reach the edge of the screen before 999 move
        // does not use <esc>[999;999H, because the docs doesn't specify the concequence when
        // the cursor is out of the screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;

        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

/* row operations */

// convert the chars index into a render index, the cursor would jump to the
// beginning of next word if there is a '\t'
int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;

    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
        {
            /**
             * (rx % KILO_TAB_STOP) find out how many columns we are to the right of the last tab stop
             * (KILO_TAB_STOP - 1) find out how many colunmns we are to the left of the next tab stop
             */
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }

        // gets us right on the next tab stop
        rx++;
    }

    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;

    for(cx = 0; cx < row->size; cx++)
    {
        if(row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);

        cur_rx++;

        if(cur_rx > rx)
            return cx;
    }

    return cx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;

    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';

            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;

    editorFreeRow(&E.row[at]);
    // overwrite the current row by shifting the next and the rest of the rows
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    // decrement the total row of the file
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;

    // 1 byte for new character and 1 byte for \0
    row->chars = realloc(row->chars, row->size + 2);
    // safer than memcpy when the source and destination arrays overlap
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppenedString(erow *row, char *s, size_t len)
{
    // including '\0'
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* editor operations */

void editorInsertChar(int c)
{
    // whether the cursor is on the tilde line after the end of the file
    if (E.cy == E.numrows)
    {
        // append a new row to the file before insertinga character there
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numrows)
        return;

    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.row[E.cy];

    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    // if the cursor is in the beginning of the line
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppenedString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/* file i/o */

char *editorRowsToString(int *buflen)
{
    int totlen = 0;
    int j;

    // get the total length of the content
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1; // plus 1 for '\n'
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;

    // copy all the content into buf
    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    free(E.filename);
    // makes a copy of the given string, allocating the required memory
    // and assuming you will free that memory.
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0; // line capacity
    ssize_t linelen;

    // get a line of text from the file and get the length of line it read
    // whether is the EOF
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        // truncate the \r\n
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;

        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave()
{
    /**
     * Bash on Windows required to press ESC 3 times to get one ESC keypress
     * to register in out program, because the read() calls in editorReadKey()
     * that look for an escape sequemce never time out.
     */

    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)");

        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    /**
     * Note: The reason not use O_TRUNC is because it will erase all the
     * data first when it opens the file. If write() fail, the data from
     * the file would be lost completely.
     * The way now is using would only lost some of the data, which much
     * safer.
     *
     * The more advanced editors will write to a new, temporary file, and then
     * rename that file to the actual file the user wants to overwrite, and they
     * will carefully check for errors through the whole process.
     *
     */
    // O_CREAT: create a new file if it doesn't already exist
    // O_RDWR: open file for reading and wrting
    // read(4), write(2), execute(1), 644
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if (fd != -1)
    {
        // set file size to specific length
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);

                return;
            }
        }
    }

    free(buf);
    // strerror returns human readable string for the error code
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* find */

void editorFind()
{
    // return NULL when enter Escape Key
    char *query = editorPrompt("Search: %s (ESC to cancel)");

    if (query == NULL)
        return;

    int i;
    for (i = 0; i < E.numrows; i++) // loop through all rows
    {
        erow *row = &E.row[i];
        // returns NULL if there is no mathch, otherwise
        // it returns a pointer to the matching substring.
        char *match = strstr(row->render, query);

        // move the cursor to the target
        if (match)
        {
            E.cy = i;
            // subtract the row->render pointer from the mathch pointer
            // since match is a pointer into the row->render string
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            break;
        }
    }

    free(query);
}

// it would be a good idea to do one big write other than a bunch of small
// write(), it could make sure whole screen updates at once, to prevent
// the annoying flicker effect
/* append buffer */

struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT \
    {             \
        NULL, 0   \
    }

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/* output */

void editorScroll()
{
    E.rx = 0;

    //if there is a '\t'
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // if the cursor is above the visible window, then scrolls up
    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }

    // if the cursor is past the bottom of the visible window
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    // the same as parallel to the vertical scrolling code
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }

    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;

        // whether is drawing a row that is part of the text buffer, or a row that comes after the end of the text buffer
        if (filerow >= E.numrows)
        {
            // if the app is not opening a file
            // then print welcome in pos (E.screenrows / 3)
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Kilo editor -- version %s", KILO_VERSION);

                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--)
                    abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                // draw tilde
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            // display a line of text in the screen
            int len = E.row[filerow].rsize - E.coloff;

            if (len < 0)
                len = 0;

            if (len > E.screencols)
                len = E.screencols;

            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    // switch to inverted colors
    abAppend(ab, "\x1b[7m", 4);

    // left status, right status
    char status[80], rstatus[80];
    // display up to 20 characters
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows,
                       E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                        E.cy + 1, E.numrows);

    if (len > E.screencols)
        len = E.screencols;

    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    // switch back to normal formatting
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    // clear the message bar
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);

    if (msglen > E.screencols)
        msglen = E.screencols;

    // only the msg is fix to the bar and the file is opened after less than 5 sec
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];

    // the first calculation gets screenrows or a number lower than screenrows
    // which it is still in the screen. Note: cy could be changed.
    // E.cy - E.rowoff <= screenrows
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    // make out own printf() style function
    // store resulting string in E.statusmsg, and set E.statusmsg_time to
    // current time
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);

    // get current time by passing NULL to time()
    E.statusmsg_time = time(NULL);
}

/* input */

char *editorPrompt(char *prompt)
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        // allow BACKSPACE in the input prompt
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        // press ESC to cancel SAVE AS
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }
        // if user pree ENTER key
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            // if buflen has reached the max capacity
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editorMoveCursor(int key)
{
    // if there is a row from the file, the row would be defined
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            E.cx--;
        }
        else if (E.cy > 0) //move the cursor to the end of previous line if the cursor in the beginning of the line
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;

    case ARROW_RIGHT:
        // is the row has something out of the screen
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        else if (row && E.cx == row->size) //move the cursor to the beginning of next line if the cursor in the end of the line
        {
            E.cy++;
            E.cx = 0;
        }
        break;

    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;

    case ARROW_DOWN:
        if (E.cy != E.numrows)
        {
            E.cy++;
        }
        break;
    }

    // check whether the line is shorter or longer than the previous line
    // if so, change the position of the cursor
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;

    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

// convert the input into actions
void editorProcessKeypress()
{
    // only initialize once
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;
    /**
     * ^q 3 times, then quit without save, reset when press the key
     * other than ^q
     */
    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        // clear entire screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        // reposite the cursor <esc>[1;1H to the top left corner
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);

        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        /**
             * Delegating to editorMoveCursor() takes care of all the
             * bounds-checking and cursor-fixing that need to be done when moving
             * the cursor.
             */

        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1;

            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    // insert the character to the row
    default:
        editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/* init */

// initialize all the fields in the E struct
void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");

    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    // read 1 byte from the standard input into c until no more bytes from the buffer
    // read returns the bytes it read, 0 indicate the EOF
    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}

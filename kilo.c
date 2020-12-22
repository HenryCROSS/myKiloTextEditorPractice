/* includes */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/* defines */

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
{
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
    char *chars;
} erow;

// controlling the cursor, the text, all the property of the application
struct editorConfig
{
    int cx, cy;
    // row offset, keep track of what row of the file the user is currently scrolled to
    // it decides the edge of the screen, cy would go over the screen,
    // the idea of this variable is to do the calculation with cy to calculate the
    // pos of the cursor within the screen after the cursor go through the
    // screen
    int rowoff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    struct termios orig_termios;
};

struct editorConfig E;

/* terminal */

void die (const char *s)
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
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
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
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
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

        if(read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if(seq[0] == '[')
        {
            if(seq[1] >= '0' && seq[1] <= '9')
            {
                if(read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';

                if(seq[2] == '~')
                {
                    switch(seq[1])
                    {
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                    }
                }
            }
            else
            {
                switch(seq[1])
                {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        }
        else if(seq[0] == 'O')
        {
            switch(seq[1])
            {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
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
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    // parse the response from the standard input
    // think about echo -e hello > file
    // 0 -> output, 1-> standard input 2->error message
    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;

        if(buf[i] == 'R')
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
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
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

void editorAppendRow(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/* file i/o */

void editorOpen(char *filename)
{
    FILE *fp = fopen(filename, "r");
    if(!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0; // line capacity
    ssize_t linelen;

    // get a line of text from the file and get the length of line it read
    // whether is the EOF
    while((linelen = getline(&line, &linecap, fp)) != -1)
    {
        // truncate the \r\n
        while(linelen > 0 && (line[linelen - 1] == '\n' ||
                              line[linelen - 1] == '\r'))
            linelen--;

        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
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

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL)
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
    // if the cursor is above the visible window, then scrolls up
    if(E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }

    // if the cursor is past the bottom of the visible window
    if(E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for(y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;

        // whether is drawing a row that is part of the text buffer, or a row that comes after the end of the text buffer
        if(filerow >= E.numrows)
        {
            // if the app is not opening a file
            // then print welcome in pos (E.screenrows / 3)
            if(E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Kilo editor -- version %s", KILO_VERSION);

                if(welcomelen > E.screencols)
                    welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
                if(padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while(padding--)
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
            // display a line of text
            int len = E.row[filerow].size;

            if (len > E.screencols)
                len = E.screencols;

            abAppend(ab, E.row[filerow].chars, len);
        }

        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1)
        {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];

    // the first calculation gets screenrows or a number lower than screenrows
    // which it is still in the screen. Note: cy could be changed.
    // E.cy - E.rowoff <= screenrows
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* input */

void editorMoveCursor(int key)
{
    switch(key)
    {
    case ARROW_LEFT:
        if(E.cx != 0)
        {
            E.cx--;
        }
        break;

    case ARROW_RIGHT:
        if(E.cx != E.screencols - 1)
        {
            E.cx++;
        }
        break;

    case ARROW_UP:
        if(E.cy != 0)
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
}

// convert the input into actions
void editorProcessKeypress()
{
    int c = editorReadKey();

    switch (c) 
    {
    case CTRL_KEY('q'):
        // clear entire screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        // reposite the cursor <esc>[1;1H to the top left corner
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        E.cx = E.screencols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            int times = E.screenrows;
            while(times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    }
}

/* init */

// initialize all the fields in the E struct
void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.numrows = 0;
    E.row = NULL;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if(argc >= 2)
    {
        editorOpen(argv[1]);
    }

    // read 1 byte from the standard input into c until no more bytes from the buffer
    // read returns the bytes it read, 0 indicate the EOF
    while(1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}


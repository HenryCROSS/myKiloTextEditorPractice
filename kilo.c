/* includes */
#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */
#define CTRL_KEY(k) ((k) & 0x1f)

/* data */

struct editorConfig
{
    int screenrows;
    int screencols;
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

char editorReadKey()
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

    return c;
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
/* output */

void editorDrawRows()
{
    int y;
    for(y = 0; y < E.screenrows; y++)
    {
        // draw tilde
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void editorRefreshScreen()
{
    // CHECK VT100 User Guide
    // clear entire screen
    write(STDOUT_FILENO, "\x1b[2J", 4);

    // reposite the cursor <esc>[1;1H to the top left corner
    write(STDOUT_FILENO, "\x1b[H", 3);

    // draw tilde, which it is not part of the file and cannot contain any text
    editorDrawRows();

    // reposite the cursor <esc>[1;1H to the top left corner
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/* input */

void editorProcessKeypress()
{
    char c = editorReadKey();

    switch (c) 
    {
    case CTRL_KEY('q'):
        // clear entire screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        // reposite the cursor <esc>[1;1H to the top left corner
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    }
}

/* init */

// initialize all the fields in the E struct
void initEditor()
{
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main()
{
    enableRawMode();
    initEditor();

    // read 1 byte from the standard input into c until no more bytes from the buffer
    // read returns the bytes it read, 0 indicate the EOF
    while(1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}


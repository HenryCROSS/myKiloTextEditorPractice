#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void die (const char *s)
{
    // prints the descriptive error message
    perror(s);
    // indicate failure
    exit(1);
}

void disableRawMode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios))
        die("tcsetattr");
}

void enableRawMode()
{
    if(tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");

    // the function "disableRawMode" would be execute when the application exits
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

int main()
{
    enableRawMode();

    // read 1 byte from the standard input into c until no more bytes from the buffer
    // read returns the bytes it read, 0 indicate the EOF
    while(1)
    {
        char c = '\0';

        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
            die("read");

        // whether is a control character - nonprintable character
        if(iscntrl(c))
        {
            printf("%d\r\n", c);
        }
        else
        {
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q')
            break;
    }

    return 0;
}


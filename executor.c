#include "executor.h"

int main(void) {
    init();
    char input[511];
    while (read_line(input, 511, stdin))
    {
        safety(true);
        char **args = split_string(input);
        if (!strcmp(*args, "quit")) { quit(args); break; }
        if (!strcmp(*args, "run")) { runner(args); }
        else if (!strcmp(*args, "out") || !strcmp(*args, "err")) { printer(args); }
        else if (!strcmp(*args, "sleep")) { nap(args); }
        else if (!strcmp(*args, "kill")) { killer(args); }
        else if (!strcmp(*args, "\n")) { usleep(0); }
        else { fprintf(stderr, "Wrong command\n"); exit(1); }
        free_split_string(args);
        safety(false);
    }
    if (!flag) safety(true);
    kill_tasks();
    destroy();
    return 0;
}
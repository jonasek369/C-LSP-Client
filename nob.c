#define NOB_IMPLEMENTATION
#include "../thirdparty/nob.h"

#define OPTIMISATION 0

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    Nob_Cmd cmd = {0};

    nob_cmd_append(&cmd,
    "gcc",
    "-std=gnu11",
    "-D_POSIX_C_SOURCE=200809L",
    "-g",
    "-o", "main",
    "main.c",
    "../tiny_queue/tiny_queue.c",
    "-Wall",
    "-Wextra",
    "-Wno-sign-compare",
    "-lpthread"
);

    #if (OPTIMISATION == 1)
        nob_cmd_append(&cmd, "-O3", "-march=native");
    #endif

    if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

    nob_cmd_append(&cmd, "./main", "main.c");
    if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

    return 0;
}
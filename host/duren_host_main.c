/*
 * Desktop entry for Duren: chdir into host_data/ then run the SDL game loop.
 * Assets (PNG + Duren.pak) live next to this working directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

int duren_application_main(int argc, char *argv[]);

static int try_chdir(const char *path)
{
    if (!path || !path[0])
        return -1;
    if (chdir(path) == 0) {
        printf("host: assets in %s\n", path);
        return 0;
    }
    return -1;
}

static int chdir_to_host_data(const char *argv0)
{
    const char *env = getenv("DUREN_HOST_DATA");
    char buf[PATH_MAX];
    char exe_copy[PATH_MAX];

    if (env && try_chdir(env) == 0)
        return 0;

    /* Prefer ./host_data from the current working directory. */
    if (try_chdir("host_data") == 0)
        return 0;

    /* Then <dirname(argv0)>/host_data when launched from elsewhere. */
    if (argv0 && argv0[0]) {
        strncpy(exe_copy, argv0, sizeof(exe_copy) - 1);
        exe_copy[sizeof(exe_copy) - 1] = '\0';
        {
            char *dir = dirname(exe_copy);
            snprintf(buf, sizeof(buf), "%s/host_data", dir);
            if (try_chdir(buf) == 0)
                return 0;
            snprintf(buf, sizeof(buf), "%s/../host_data", dir);
            if (try_chdir(buf) == 0)
                return 0;
        }
    }

    fprintf(stderr,
            "host: cannot find host_data/ (PNG + Duren.pak).\n"
            "      Run from the repo root, or set DUREN_HOST_DATA.\n");
    return -1;
}

int main(int argc, char **argv)
{
    if (chdir_to_host_data(argc > 0 ? argv[0] : NULL) != 0)
        return 1;

    printf("host: Esc to quit — arrows move, Z/A confirm, X/B cancel, Enter/P/G pause\n");
    return duren_application_main(argc, argv);
}

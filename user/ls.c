/* =============================================================================
 * ls.c — a minimal coreutils `ls`, linked against REAL musl (§M36).
 *
 * Lists a directory (argv[1], or "/" if none) via musl's opendir/readdir —
 * which bottom out in d-os's Linux-ABI open + getdents64.  Proves the store→
 * exec path for a directory-reading program.  Run from the store by `pkgrun`.
 * ============================================================================= */
#include <stdio.h>
#include <dirent.h>

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/";
    DIR* d = opendir(path);
    if (!d) { fprintf(stderr, "ls: %s: cannot open\n", path); return 1; }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;                       /* skip "." and ".." */
        puts(e->d_name);
    }
    closedir(d);
    return 0;
}

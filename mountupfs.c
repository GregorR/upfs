#define _BSD_SOURCE /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef UPFS_PATH
#define UPFS_PATH "/usr/bin/upfs"
#endif

#define NEED_OPTS "allow_other"

void usage(void)
{
    fprintf(stderr, "Use: mount.upfs <perm root>:<store root> <mount point>\n");
}

void handle_options(char *options, int *mount_p, int *mount_s)
{
    char *cur, *comma;
    int skip;

    /* Look for a mount_* option */
    cur = options;
    while ((comma = strchr(cur, ','))) {
        skip = 0;
        comma = strchr(cur, ',');
        if (comma) *comma = 0;

        if (!strcmp(cur, "mount_p")) {
            *mount_p = 1;
            skip = 1;
        } else if (!strcmp(cur, "mount_s")) {
            *mount_s = 1;
            skip = 1;
        }

        if (skip) {
            memmove(comma + 1, cur, strlen(comma + 1) + 1);
        } else {
            *comma = ',';
            cur = comma + 1;
        }
    }
}

void do_mount(char *path)
{
    char *mount_args[3];
    pid_t pid;

    mount_args[0] = "/bin/mount";
    mount_args[1] = path;
    mount_args[2] = NULL;

    /* Fork off the mount */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid >= 0) {
        waitpid(pid, NULL, 0);
    } else {
        execv(mount_args[0], mount_args);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    char *arg, **fuse_argv;
    char *opt_arg, *perm_root, *store_root;
    int ai, fai, got_root, got_opts;
    int mount_p = 0, mount_s = 0;

    fuse_argv = calloc(argc + 4, sizeof(char *));
    if (!fuse_argv) {
        perror("calloc");
        return 1;
    }

    /* Translate the arguments */
    fuse_argv[0] = UPFS_PATH;
    got_root = 0;
    got_opts = 0;
    for (ai = fai = 1; ai < argc; ai++) {
        arg = argv[ai];
        if (arg[0] == '-') {
            fuse_argv[fai++] = arg;
            if (arg[1] == 'o' && !arg[2]) {
                /* Make sure the options have what we need */
                arg = argv[++ai];
                opt_arg = malloc(strlen(arg) + sizeof(NEED_OPTS) + 2);
                if (!opt_arg) {
                    perror("malloc");
                    return 1;
                }
                sprintf(opt_arg, "%s,%s", arg, NEED_OPTS);
                handle_options(opt_arg, &mount_p, &mount_s);
                fuse_argv[fai++] = opt_arg;
                got_opts = 1;
            }

        } else if (!got_root) {
            perm_root = strdup(arg);
            if (!perm_root) {
                perror("strdup");
                return 1;
            }
            store_root = strchr(perm_root, ':');
            if (!store_root) {
                usage();
                return 1;
            }
            *store_root++ = 0;
            fuse_argv[fai++] = perm_root;
            fuse_argv[fai++] = store_root;
            got_root = 1;

        } else {
            fuse_argv[fai++] = arg;

        }
    }

    if (!got_opts) {
        fuse_argv[fai++] = "-o";
        fuse_argv[fai++] = NEED_OPTS;
    }

    if (!got_root) {
        usage();
        return 1;
    }

    /* If we were requested to mount either, do so */
    if (mount_p)
        do_mount(perm_root);
    if (mount_s)
        do_mount(store_root);

    execv(fuse_argv[0], fuse_argv);
    perror(fuse_argv[0]);
    return 1;
}

/* FIXME: This is mostly duplicated from mountupfs */

#define _BSD_SOURCE /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef UPFS_PATH
#define UPFS_PATH "/usr/bin/upfs-ps"
#endif

#define NEED_OPTS "nonempty,allow_other,default_permissions"

void usage(void)
{
    fprintf(stderr, "Use: mount.upfsps <root> <mount point>\n");
}

void handle_options(char *options, int *mount_r)
{
    char *cur, *comma;
    int skip;

    /* Look for a mount_* option */
    cur = options;
    while ((comma = strchr(cur, ','))) {
        skip = 0;
        comma = strchr(cur, ',');
        if (comma) *comma = 0;

        if (!strcmp(cur, "mount_r")) {
            *mount_r = 1;
            skip = 1;
        }

        if (skip) {
            memmove(cur, comma + 1, strlen(comma + 1) + 1);
        } else {
            *comma = ',';
            cur = comma + 1;
        }
    }
}

void do_mount(char **root, char *target)
{
    char *mount_args[4];
    pid_t pid;
    struct stat sbuf;
    int check_code = 0, code;

    /* Choose what kind of mount to perform based on the type of root */
    if (stat(*root, &sbuf) < 0)
        return;

    if (S_ISDIR(sbuf.st_mode)) {
        /* Normal directory mount out of fstab */
        mount_args[0] = "/bin/mount";
        mount_args[1] = *root;
        mount_args[2] = NULL;

    } else {
        /* Device mount. FIXME: loop mounting, sub-mount options/type, etc */
        mount_args[0] = "/bin/mount";
        mount_args[1] = *root;
        mount_args[2] = target;
        mount_args[3] = NULL;
        check_code = 1;
        *root = target;

    }

    /* Fork off the mount */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid >= 0) {
        waitpid(pid, &code, 0);
    } else {
        execv(mount_args[0], mount_args);
        exit(1);
    }

    /* And check that it worked */
    if (check_code) {
        if (!WIFEXITED(code) || WEXITSTATUS(code)) {
            exit(1);
        }
    }

}

int main(int argc, char **argv)
{
    char *arg, **fuse_argv;
    char *opt_arg, **root, *target;
    int ai, fai, got_root = 0, got_target = 0, got_opts = 0;
    int mount_r = 0;

    fuse_argv = calloc(argc + 3, sizeof(char *));
    if (!fuse_argv) {
        perror("calloc");
        return 1;
    }

    /* Translate the arguments */
    fuse_argv[0] = UPFS_PATH;
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
                handle_options(opt_arg, &mount_r);
                fuse_argv[fai++] = opt_arg;
                got_opts = 1;
            }

        } else if (!got_root) {
            root = &fuse_argv[fai++];
            *root = arg;
            got_root = 1;

        } else if (!got_target) {
            target = arg;
            fuse_argv[fai++] = arg;
            got_target = 1;

        } else {
            fuse_argv[fai++] = arg;

        }
    }

    if (!got_opts) {
        fuse_argv[fai++] = "-o";
        fuse_argv[fai++] = NEED_OPTS;
    }

    if (!got_root || !got_target) {
        usage();
        return 1;
    }

    /* If we were requested to mount either, do so */
    if (mount_r)
        do_mount(root, target);

    execv(fuse_argv[0], fuse_argv);
    perror(fuse_argv[0]);
    return 1;
}

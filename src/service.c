/*
 * xlrbridge — launchd service integration. See service.h.
 *
 * We shell out to `launchctl` (load -w / unload / list) via fork+exec rather
 * than the deprecated/header-only launchd C API; it's the documented, stable
 * interface and matches the prototype's approach. The plist is written with a
 * minimal hand-rolled XML emitter (no plist library dependency).
 */

#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#include <limits.h>

/* Run argv[] via fork/exec, with stdout/stderr captured into out (size outsz,
 * NUL-terminated, may be NULL to discard). Returns the child's exit status, or
 * -1 if the child could not be spawned. */
static int run_capture(char *const argv[], char *out, size_t outsz) {
    int pipefd[2];
    int have_pipe = (out != NULL && outsz > 0);
    if (have_pipe) {
        if (pipe(pipefd) != 0) {
            have_pipe = 0;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (have_pipe) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }
    if (pid == 0) {
        /* child */
        if (have_pipe) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent */
    if (have_pipe) {
        close(pipefd[1]);
        size_t total = 0;
        ssize_t r;
        while (total + 1 < outsz &&
               (r = read(pipefd[0], out + total, outsz - 1 - total)) > 0) {
            total += (size_t)r;
        }
        out[total] = '\0';
        close(pipefd[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

int xb_service_plist_path(char *buf, size_t bufsz) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    int wrote = snprintf(buf, bufsz,
                         "%s/Library/LaunchAgents/" XB_AGENT_LABEL ".plist",
                         home);
    if (wrote < 0 || (size_t)wrote >= bufsz) {
        return -1;
    }
    return 0;
}

int xb_service_self_path(char *buf, size_t bufsz) {
    char raw[PATH_MAX];
    uint32_t sz = sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0) {
        return -1;
    }
    /* Canonicalize (resolve .., symlinks) so the plist holds a stable path. */
    char resolved[PATH_MAX];
    if (realpath(raw, resolved) != NULL) {
        int wrote = snprintf(buf, bufsz, "%s", resolved);
        return (wrote > 0 && (size_t)wrote < bufsz) ? 0 : -1;
    }
    int wrote = snprintf(buf, bufsz, "%s", raw);
    return (wrote > 0 && (size_t)wrote < bufsz) ? 0 : -1;
}

/* Ensure ~/Library/LaunchAgents exists. Returns 0 on success. */
static int ensure_launchagents_dir(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home) >=
        (int)sizeof(dir)) {
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int xb_service_write_plist(const char *binary_path) {
    char self[PATH_MAX];
    if (binary_path == NULL || binary_path[0] == '\0') {
        if (xb_service_self_path(self, sizeof(self)) != 0) {
            fprintf(stderr, "xlrbridge: service: cannot resolve binary path\n");
            return -1;
        }
        binary_path = self;
    }

    if (ensure_launchagents_dir() != 0) {
        fprintf(stderr, "xlrbridge: service: cannot create ~/Library/LaunchAgents\n");
        return -1;
    }

    char path[PATH_MAX];
    if (xb_service_plist_path(path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "xlrbridge: service: cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>" XB_AGENT_LABEL "</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>%s</string>\n"
        "    <string>run</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "  <key>ThrottleInterval</key><integer>10</integer>\n"
        "  <key>StandardErrorPath</key><string>" XB_AGENT_LOG_PATH "</string>\n"
        "  <key>StandardOutPath</key><string>" XB_AGENT_LOG_PATH "</string>\n"
        "</dict>\n"
        "</plist>\n",
        binary_path);

    if (fclose(f) != 0) {
        fprintf(stderr, "xlrbridge: service: error closing %s\n", path);
        return -1;
    }
    return 0;
}

int xb_service_load(void) {
    char path[PATH_MAX];
    if (xb_service_plist_path(path, sizeof(path)) != 0) {
        return -1;
    }
    char *argv[] = { "launchctl", "load", "-w", path, NULL };
    int rc = run_capture(argv, NULL, 0);
    /* launchctl load returns non-zero if already loaded; treat loaded state as
     * success by re-checking. */
    if (rc != 0 && !xb_service_is_loaded(XB_AGENT_LABEL)) {
        return -1;
    }
    return 0;
}

int xb_service_unload(void) {
    char path[PATH_MAX];
    if (xb_service_plist_path(path, sizeof(path)) != 0) {
        return -1;
    }
    char *argv[] = { "launchctl", "unload", path, NULL };
    run_capture(argv, NULL, 0);
    /* Success == not loaded afterwards (tolerate "not loaded" errors). */
    if (xb_service_is_loaded(XB_AGENT_LABEL)) {
        return -1;
    }
    return 0;
}

int xb_service_is_loaded(const char *label) {
    long pid;
    /* loaded == listed by `launchctl list <label>` exiting 0. */
    char out[256];
    char *argv[] = { "launchctl", "list", (char *)label, NULL };
    int rc = run_capture(argv, out, sizeof(out));
    if (rc == 0) {
        return 1;
    }
    /* Fall back: also try pid lookup (covers older list formats). */
    return (xb_service_agent_pid(label, &pid) == 0) ? 1 : 0;
}

int xb_service_agent_pid(const char *label, long *out_pid) {
    /* `launchctl list <label>` prints a plist-ish dict with "PID" = N; if the
     * agent is loaded but not running it has no PID key. */
    char out[8192];
    char *argv[] = { "launchctl", "list", (char *)label, NULL };
    int rc = run_capture(argv, out, sizeof(out));
    if (rc != 0) {
        return -1;
    }
    const char *p = strstr(out, "\"PID\"");
    if (p == NULL) {
        p = strstr(out, "PID");
    }
    if (p == NULL) {
        return -1;
    }
    const char *eq = strchr(p, '=');
    if (eq == NULL) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(eq + 1, &end, 10);
    if (end == eq + 1) {
        return -1;
    }
    if (out_pid != NULL) {
        *out_pid = v;
    }
    return 0;
}

int xb_service_remove_plist(void) {
    char path[PATH_MAX];
    if (xb_service_plist_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "xlrbridge: service: cannot remove %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

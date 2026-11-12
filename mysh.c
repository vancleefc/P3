#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "arraylist.h"

#define BUFSIZE 4096


/* One program in a job. args is NULL-terminated before execv(). */
typedef struct {
    arraylist_t args;
} cmd_t;

/* One line of input: 1 command, or n+1 commands joined by n pipes. */
typedef struct {
    cmd_t *cmds;
    int    ncmds;
    char  *infile;    /* "< file"            or NULL */
    char  *outfile;   /* "> file"/">> file"  or NULL */
    int    append;    /* 1 if >>, 0 if >     */
} job_t;

/* Buffered reader on top of read(). */
typedef struct {
    int  fd;
    char buf[BUFSIZE];
    int  pos;         /* next unread byte in buf */
    int  len;         /* number of valid bytes in buf */
} input_t;

static int interactive = 0;      /* set once in main() */
static int exit_requested = 0;   /* set when a job contains `exit` */

static const char *search_dirs[] = { "/usr/local/bin", "/usr/bin", "/bin" };
#define NUM_SEARCH_DIRS 3


/* Return the next line (without '\n') as a malloc'd string, or NULL at EOF.
 *
 * read() is only called when the buffer holds no unread bytes, so a
 * complete line that is already buffered is returned without blocking.
 * A final line with no trailing newline is still returned.
 */
static char *next_line(input_t *in)
{
    char  *line = NULL;
    size_t linelen = 0;

    for (;;) {
        if (in->pos == in->len) {             /* buffer empty: refill */
            ssize_t n = read(in->fd, in->buf, BUFSIZE);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) perror("read");
            if (n <= 0) return line;          /* EOF: partial line or NULL */
            in->pos = 0;
            in->len = (int)n;
        }

        char *start = in->buf + in->pos;
        int   avail = in->len - in->pos;
        char *nl    = memchr(start, '\n', avail);
        int   chunk = nl ? (int)(nl - start) : avail;

        char *p = realloc(line, linelen + chunk + 1);
        if (!p) { perror("realloc"); exit(EXIT_FAILURE); }
        line = p;
        memcpy(line + linelen, start, chunk);
        linelen += chunk;
        line[linelen] = '\0';

        in->pos += chunk + (nl ? 1 : 0);      /* consume the newline too */
        if (nl) return line;
    }
}

/* Return the working directory as a malloc'd string, or NULL on error. */
static char *get_cwd(void)
{
    size_t size = 256;
    for (;;) {
        char *buf = malloc(size);
        if (!buf) return NULL;
        if (getcwd(buf, size)) return buf;
        free(buf);
        if (errno != ERANGE) return NULL;     /* real error, not "too small" */
        size *= 2;
    }
}

/* Print "<cwd>$ ", replacing a leading $HOME with ~. */
static void print_prompt(void)
{
    char *cwd = get_cwd();
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;

    if (!cwd)
        printf("$ ");
    else if (hl > 0 && strncmp(cwd, home, hl) == 0 &&
             (cwd[hl] == '/' || cwd[hl] == '\0'))
        printf("~%s$ ", cwd + hl);            /* /home/user/a -> ~/a */
    else
        printf("%s$ ", cwd);

    fflush(stdout);
    free(cwd);
}


/* Split line into tokens, pushing malloc'd copies onto tokens.
 *
 *  - '#' ends the line (comment), wherever it appears.
 *  - Whitespace separates tokens.
 *  - <, >, >> and | are always their own token, even without spaces
 *    around them ("a|b" -> "a" "|" "b").
 */
static void tokenize(const char *line, arraylist_t *tokens)
{
    const char *p = line;

    while (*p && *p != '#') {
        if (isspace((unsigned char)*p)) { p++; continue; }

        const char *start = p;
        if (*p == '<' || *p == '|') {
            p++;
        } else if (*p == '>') {
            p += (p[1] == '>') ? 2 : 1;
        } else {
            while (*p && *p != '#' && !isspace((unsigned char)*p) &&
                   *p != '<' && *p != '>' && *p != '|')
                p++;
        }

        char *tok = strndup(start, (size_t)(p - start));
        if (!tok) { perror("strndup"); exit(EXIT_FAILURE); }
        al_push(tokens, tok);
    }
}

/* qsort comparator for an array of char* (keeps glob output in order). */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
 
/* Expand one token that contains '*', pushing each match onto args.
 * If nothing matches, push a copy of the token unchanged.
 *
 *  - Split at the last '/': directory part (or ".") and name pattern.
 *  - Split the pattern at '*' into prefix and suffix.
 *  - opendir()/readdir(): a name matches if
 *        strlen(name) >= strlen(prefix) + strlen(suffix)
 *        && it starts with prefix && it ends with suffix.
 *  - If prefix is empty, skip names beginning with '.'.
 *  - Matches must include the directory part ("baz/foo1bar", not "foo1bar").
 */
static void expand_wildcard(const char *token, arraylist_t *args)
{
    const char *slash = strrchr(token, '/');
    const char *pattern = slash ? slash + 1 : token;
    const char *star = strchr(pattern, '*');
 
    /* Only one '*' is allowed, and only in the last path component.
     * Anything else is passed through literally. */
    if (!star || strchr(star + 1, '*') ||
        (slash && memchr(token, '*', (size_t)(slash - token)))) {
        al_push(args, strdup(token));
        return;
    }
 
    /* dirpart is what gets prepended to each match ("" or "baz/"). */
    size_t dirlen = slash ? (size_t)(slash - token) + 1 : 0;
    char *dirpart = strndup(token, dirlen);
    char *prefix  = strndup(pattern, (size_t)(star - pattern));
    const char *suffix = star + 1;
    size_t plen = strlen(prefix), slen = strlen(suffix);
    if (!dirpart || !prefix) { perror("strndup"); exit(EXIT_FAILURE); }
 
    arraylist_t matches;
    al_init(&matches, 8);
 
    DIR *dir = opendir(dirlen ? dirpart : ".");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *name = de->d_name;
            size_t nlen = strlen(name);
 
            if (plen == 0 && name[0] == '.') continue;   /* hidden files */
            if (nlen < plen + slen) continue;
            if (strncmp(name, prefix, plen) != 0) continue;
            if (strcmp(name + nlen - slen, suffix) != 0) continue;
 
            char *m = malloc(dirlen + nlen + 1);
            if (!m) { perror("malloc"); exit(EXIT_FAILURE); }
            memcpy(m, dirpart, dirlen);
            memcpy(m + dirlen, name, nlen + 1);
            al_push(&matches, m);
        }
        closedir(dir);
    }
 
    if (matches.length == 0) {
        al_push(args, strdup(token));
    } else {
        qsort(matches.data, matches.length, sizeof(char *), cmp_str);
        for (unsigned i = 0; i < matches.length; i++) {
            al_push(args, matches.data[i]);   /* ownership moves to args */
            matches.data[i] = NULL;
        }
    }
 
    al_destroy(&matches);                     /* frees only NULLs now */
    free(dirpart);
    free(prefix);
}
 
static int is_special(const char *tok)
{
    return !strcmp(tok, "<") || !strcmp(tok, ">") ||
           !strcmp(tok, ">>") || !strcmp(tok, "|");
}
 
/* Build job from tokens. Return 0 on success, -1 on syntax error
 * (after printing a message to stderr).
 *
 * Walk the tokens:
 *  - "<", ">", ">>": the next token is the file name. Syntax error if it
 *    is missing or is_special(). Record in job->infile / outfile / append.
 *  - "|": syntax error if the current command has no args; otherwise
 *    start a new cmd_t (realloc job->cmds, al_init its args).
 *  - anything else: if it contains '*' call expand_wildcard(), else push
 *    a strdup onto the current command's args.
 *  - At the end: syntax error if the last command has no args
 *    (e.g. "foo |").
 *  - Set exit_requested if any command's args[0] is "exit".
 *
 * The job is always left in a state free_job() can clean up, even after
 * a syntax error.
 */
static int parse_job(arraylist_t *tokens, job_t *job)
{
    job->infile = job->outfile = NULL;
    job->append = 0;
    job->ncmds = 1;
    job->cmds = malloc(sizeof(cmd_t));
    if (!job->cmds) { perror("malloc"); exit(EXIT_FAILURE); }
    al_init(&job->cmds[0].args, 8);
 
    for (unsigned i = 0; i < tokens->length; i++) {
        const char *tok = tokens->data[i];
        cmd_t *cur = &job->cmds[job->ncmds - 1];
 
        if (!strcmp(tok, "<") || !strcmp(tok, ">") || !strcmp(tok, ">>")) {
            if (i + 1 >= tokens->length) {
                fprintf(stderr, "mysh: syntax error: expected file name after '%s'\n", tok);
                return -1;
            }
            const char *file = tokens->data[++i];
            if (is_special(file)) {
                fprintf(stderr, "mysh: syntax error near unexpected token '%s'\n", file);
                return -1;
            }
            char *copy = strdup(file);
            if (!copy) { perror("strdup"); exit(EXIT_FAILURE); }
 
            if (tok[0] == '<') {
                free(job->infile);            /* last one wins */
                job->infile = copy;
            } else {
                free(job->outfile);
                job->outfile = copy;
                job->append = (tok[1] == '>');
            }
 
        } else if (!strcmp(tok, "|")) {
            if (cur->args.length == 0) {
                fprintf(stderr, "mysh: syntax error near unexpected token '|'\n");
                return -1;
            }
            cmd_t *p = realloc(job->cmds, (job->ncmds + 1) * sizeof(cmd_t));
            if (!p) { perror("realloc"); exit(EXIT_FAILURE); }
            job->cmds = p;
            al_init(&job->cmds[job->ncmds].args, 8);
            job->ncmds++;
 
        } else if (strchr(tok, '*')) {
            expand_wildcard(tok, &cur->args);
 
        } else {
            char *copy = strdup(tok);
            if (!copy) { perror("strdup"); exit(EXIT_FAILURE); }
            al_push(&cur->args, copy);
        }
    }
 
    if (job->cmds[job->ncmds - 1].args.length == 0) {
        fprintf(stderr, job->ncmds > 1
                ? "mysh: syntax error: missing command after '|'\n"
                : "mysh: syntax error: missing command\n");
        return -1;
    }
 
    for (int i = 0; i < job->ncmds; i++)
        if (!strcmp(job->cmds[i].args.data[0], "exit"))
            exit_requested = 1;
 
    return 0;
}
 
static void free_job(job_t *job)
{
    for (int i = 0; i < job->ncmds; i++) al_destroy(&job->cmds[i].args);
    free(job->cmds);
    free(job->infile);
    free(job->outfile);
}
 
/* ------------------------------------------------------------------ */
/* Program lookup and built-ins                                        */
/* ------------------------------------------------------------------ */
 
/* Return a malloc'd path to the executable for name, or NULL if there
 * is none. The caller frees the result.
 *
 *  - name contains '/': it is a path; usable if we may execute it.
 *  - otherwise: first hit in /usr/local/bin, /usr/bin, /bin.
 */
static char *find_program(const char *name)
{
    if (strchr(name, '/'))
        return access(name, X_OK) == 0 ? strdup(name) : NULL;
 
    for (int i = 0; i < NUM_SEARCH_DIRS; i++) {
        size_t n = strlen(search_dirs[i]) + strlen(name) + 2;   /* '/' + '\0' */
        char *path = malloc(n);
        if (!path) { perror("malloc"); exit(EXIT_FAILURE); }
        snprintf(path, n, "%s/%s", search_dirs[i], name);
        if (access(path, X_OK) == 0) return path;
        free(path);
    }
    return NULL;
}
 
static int is_builtin(const char *name)
{
    return !strcmp(name, "cd") || !strcmp(name, "pwd") ||
           !strcmp(name, "which") || !strcmp(name, "exit");
}
 
/* Run a built-in using the current stdin/stdout. argv[0] is the built-in's
 * name and argv[argc] == NULL. Return 0 on success, 1 on failure.
 * Output is flushed before returning, so the caller can restore fd 1
 * immediately afterwards.
 */
static int run_builtin(int argc, char **argv)
{
    int failed = 0;
 
    if (!strcmp(argv[0], "cd")) {
        const char *dir;
        if (argc > 2) {
            fprintf(stderr, "cd: too many arguments\n");
            return 1;
        }
        if (argc == 2) {
            dir = argv[1];
        } else if (!(dir = getenv("HOME"))) {
            fprintf(stderr, "cd: HOME not set\n");
            return 1;
        }
        if (chdir(dir) != 0) {
            perror("cd");
            return 1;
        }
 
    } else if (!strcmp(argv[0], "pwd")) {
        char *cwd = get_cwd();
        if (!cwd) {
            perror("pwd");
            return 1;
        }
        printf("%s\n", cwd);
        free(cwd);
 
    } else if (!strcmp(argv[0], "which")) {
        if (argc < 2) {
            fprintf(stderr, "which: missing argument\n");
            return 1;
        }
        for (int i = 1; i < argc; i++) {
            char *path = find_program(argv[i]);   /* built-ins not special */
            if (path) {
                printf("%s\n", path);
                free(path);
            } else {
                fprintf(stderr, "which: %s: not found\n", argv[i]);
                failed = 1;
            }
        }
    }
    /* exit: nothing to do here; parse_job sets exit_requested. */
 
    if (fflush(stdout) != 0) failed = 1;
    return failed;
}
 
/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */
 
/* Open the job's redirection files.
 * On success set *in_fd / *out_fd (-1 = no redirection) and return 0.
 * On failure perror, close anything opened, and return -1.
 *
 *  - infile:  open(O_RDONLY)
 *  - outfile: open(O_WRONLY|O_CREAT|(append ? O_APPEND : O_TRUNC), 0640)
 *  - In batch mode with no infile, open /dev/null as the input.
 */
static int open_redirects(job_t *job, int *in_fd, int *out_fd)
{
    *in_fd = *out_fd = -1;
 
    if (job->infile) {
        *in_fd = open(job->infile, O_RDONLY);
        if (*in_fd < 0) {
            perror(job->infile);
            return -1;
        }
    } else if (!interactive) {
        *in_fd = open("/dev/null", O_RDONLY);
        if (*in_fd < 0) {
            perror("/dev/null");
            return -1;
        }
    }
 
    if (job->outfile) {
        int flags = O_WRONLY | O_CREAT | (job->append ? O_APPEND : O_TRUNC);
        *out_fd = open(job->outfile, flags, S_IRUSR | S_IWUSR | S_IRGRP);
        if (*out_fd < 0) {
            perror(job->outfile);
            if (*in_fd != -1) close(*in_fd);
            *in_fd = -1;
            return -1;
        }
    }
 
    return 0;
}
 
/* Code that runs in a forked child: wire up stdin/stdout, then become
 * the program (or run the built-in and exit). Never returns.
 *
 *  - dup2(in_fd, 0) / dup2(out_fd, 1) when they are not -1, then close
 *    the originals. (execute_job closes every other pipe fd the child
 *    inherited before calling this.)
 *  - Built-in: exit(run_builtin(argc, argv)).
 *  - Otherwise find_program(); if NULL print "name: command not found"
 *    and exit(1). Then execv(path, argv); if it returns, perror and exit(1).
 */
static void child_exec(cmd_t *cmd, int in_fd, int out_fd)
{
    if (in_fd != -1 && in_fd != STDIN_FILENO) {
        if (dup2(in_fd, STDIN_FILENO) < 0) { perror("dup2"); exit(EXIT_FAILURE); }
        close(in_fd);
    }
    if (out_fd != -1 && out_fd != STDOUT_FILENO) {
        if (dup2(out_fd, STDOUT_FILENO) < 0) { perror("dup2"); exit(EXIT_FAILURE); }
        close(out_fd);
    }
 
    int    argc = (int)cmd->args.length - 1;
    char **argv = cmd->args.data;
 
    if (is_builtin(argv[0]))
        exit(run_builtin(argc, argv));
 
    char *path = find_program(argv[0]);
    if (!path) {
        fprintf(stderr, "%s: command not found\n", argv[0]);
        exit(EXIT_FAILURE);
    }
 
    execv(path, argv);
    perror(argv[0]);
    free(path);
    exit(EXIT_FAILURE);
}
 
/* Run a parsed job. Return 1 if it succeeded, 0 if it failed.
 *
 *  1. NULL-terminate each command's args: al_push(&args, NULL).
 *     (argc is then args.length - 1, argv is args.data.)
 *  2. open_redirects(); fail the job if it returns -1.
 *  3. Single built-in (ncmds == 1): run it in THIS process so cd works.
 *     If redirected, save fd 0/1 with dup(), dup2() the new ones in,
 *     run_builtin(), then dup2() the saved ones back and close them.
 *  4. Otherwise, for i in 0..ncmds-1:
 *       - if not the last command, pipe(); on failure perror and fail.
 *       - fflush(stdout), then fork(); the child calls child_exec() with
 *             stdin  = previous pipe's read end (or in_fd for i == 0)
 *             stdout = this pipe's write end   (or out_fd for the last)
 *       - the parent closes the fds it no longer needs right away.
 *  5. waitpid() for EVERY child; keep the status of the LAST one.
 *  6. Success iff WIFEXITED && WEXITSTATUS == 0. In interactive mode print
 *     "Exited with status N" (non-zero exit) or
 *     "Terminated by signal: <strsignal(WTERMSIG)>".
 */
static int execute_job(job_t *job)
{
    int in_fd, out_fd;
 
    /* 1. argv arrays must end in NULL for execv(). */
    for (int i = 0; i < job->ncmds; i++)
        al_push(&job->cmds[i].args, NULL);
 
    /* 2. Redirection files. */
    if (open_redirects(job, &in_fd, &out_fd) < 0)
        return 0;
 
    /* 3. A lone built-in runs in the shell itself. */
    if (job->ncmds == 1 && is_builtin(job->cmds[0].args.data[0])) {
        cmd_t *cmd = &job->cmds[0];
        int saved_in = -1, saved_out = -1, result;
 
        fflush(stdout);
        if (in_fd != -1) {
            saved_in = dup(STDIN_FILENO);
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (out_fd != -1) {
            saved_out = dup(STDOUT_FILENO);
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }
 
        result = run_builtin((int)cmd->args.length - 1, cmd->args.data);
 
        if (saved_in != -1) {
            dup2(saved_in, STDIN_FILENO);
            close(saved_in);
        }
        if (saved_out != -1) {
            dup2(saved_out, STDOUT_FILENO);
            close(saved_out);
        }
        return result == 0;
    }
 
    /* 4. Start every process in the pipeline. */
    pid_t *pids = malloc(job->ncmds * sizeof(pid_t));
    if (!pids) { perror("malloc"); exit(EXIT_FAILURE); }
    int started = 0;
    int failed = 0;
    int prev_read = in_fd;          /* stdin for the next child */
 
    for (int i = 0; i < job->ncmds; i++) {
        int last = (i == job->ncmds - 1);
        int pfd[2] = { -1, -1 };
 
        if (!last && pipe(pfd) < 0) {
            perror("pipe");
            failed = 1;
            break;
        }
 
        int child_in  = prev_read;
        int child_out = last ? out_fd : pfd[1];
 
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (!last) { close(pfd[0]); close(pfd[1]); }
            failed = 1;
            break;
        }
 
        if (pid == 0) {
            /* Close fds this child does not use, so readers see EOF. */
            if (!last) close(pfd[0]);
            if (!last && out_fd != -1) close(out_fd);
            child_exec(&job->cmds[i], child_in, child_out);
        }
 
        pids[started++] = pid;
 
        /* Parent: done with this child's stdin and the pipe's write end. */
        if (prev_read != -1) close(prev_read);
        if (!last) close(pfd[1]);
        prev_read = last ? -1 : pfd[0];
    }
 
    /* Anything still open after an early break, plus the output file. */
    if (prev_read != -1) close(prev_read);
    if (out_fd != -1) close(out_fd);
 
    /* 5. Reap every child; remember how the last one finished. */
    int status = 0, last_status = 0;
    for (int i = 0; i < started; i++) {
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno != EINTR) { perror("waitpid"); break; }
        }
        if (i == job->ncmds - 1) last_status = status;
    }
    free(pids);
 
    /* The last command never started, so the job failed. */
    if (failed || started < job->ncmds)
        return 0;
 
    /* 6. Report the result. */
    if (WIFEXITED(last_status)) {
        int code = WEXITSTATUS(last_status);
        if (code != 0 && interactive)
            printf("Exited with status %d\n", code);
        return code == 0;
    }
    if (WIFSIGNALED(last_status) && interactive)
        printf("Terminated by signal: %s\n", strsignal(WTERMSIG(last_status)));
    return 0;
}
 
 
int main(int argc, char **argv)
{
    input_t in = { .fd = STDIN_FILENO, .pos = 0, .len = 0 };
 
    if (argc > 2) {
        fprintf(stderr, "usage: %s [script]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        in.fd = open(argv[1], O_RDONLY);
        if (in.fd < 0) { perror(argv[1]); return EXIT_FAILURE; }
    } else {
        interactive = isatty(STDIN_FILENO);
    }
 
    if (interactive) printf("Welcome to my shell!\n");
 
    while (!exit_requested) {
        if (interactive) print_prompt();
 
        char *line = next_line(&in);
        if (!line) break;                     /* end of input */
 
        arraylist_t tokens;
        al_init(&tokens, 8);
        tokenize(line, &tokens);
 
        if (tokens.length > 0) {              /* skip empty/comment lines */
            job_t job;
            if (parse_job(&tokens, &job) == 0)
                execute_job(&job);
            free_job(&job);
        }
 
        al_destroy(&tokens);
        free(line);
    }
 
    if (interactive) printf("Exiting my shell.\n");
    if (in.fd != STDIN_FILENO) close(in.fd);
    return EXIT_SUCCESS;
}
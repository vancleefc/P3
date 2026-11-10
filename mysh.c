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

/* Expand one token that contains '*', pushing each match onto args.
 * If nothing matches, push a copy of the token unchanged.
 *
 * TODO:
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
    al_push(args, strdup(token));
}

static int is_special(const char *tok)
{
    return !strcmp(tok, "<") || !strcmp(tok, ">") ||
           !strcmp(tok, ">>") || !strcmp(tok, "|");
}

/* Build job from tokens. Return 0 on success, -1 on syntax error
 * (after printing a message to stderr).
 *
 * TODO: walk the tokens:
 *  - "<", ">", ">>": the next token is the file name. Syntax error if it
 *    is missing or is_special(). Record in job->infile / outfile / append.
 *  - "|": syntax error if the current command has no args; otherwise
 *    start a new cmd_t (realloc job->cmds, al_init its args).
 *  - anything else: if it contains '*' call expand_wildcard(), else push
 *    a strdup onto the current command's args.
 *  - At the end: syntax error if the last command has no args
 *    (e.g. "foo |").
 *  - Set exit_requested if any command's args[0] is "exit".
 */
static int parse_job(arraylist_t *tokens, job_t *job)
{
    (void)tokens; (void)is_special; (void)expand_wildcard;
    job->cmds = NULL;
    job->ncmds = 0;
    job->infile = job->outfile = NULL;
    job->append = 0;
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
 * TODO:
 *  - infile:  open(O_RDONLY)
 *  - outfile: open(O_WRONLY|O_CREAT|(append ? O_APPEND : O_TRUNC), 0640)
 *  - In batch mode with no infile, open /dev/null as the input.
 */
static int open_redirects(job_t *job, int *in_fd, int *out_fd)
{
    (void)job;
    *in_fd = *out_fd = -1;
    return 0;
}

/* Code that runs in a forked child: wire up stdin/stdout, then become
 * the program (or run the built-in and exit). Never returns.
 *
 * TODO:
 *  - dup2(in_fd, 0) / dup2(out_fd, 1) when they are not -1, then close
 *    the originals (and every other pipe fd the child inherited!).
 *  - Built-in: exit(run_builtin(argc, argv)).
 *  - Otherwise find_program(); if NULL print "name: command not found"
 *    and exit(1). Then execv(path, argv); if it returns, perror and exit(1).
 */
static void child_exec(cmd_t *cmd, int in_fd, int out_fd)
{
    (void)cmd; (void)in_fd; (void)out_fd; (void)is_builtin;
    exit(EXIT_FAILURE);
}

/* Run a parsed job. Return 1 if it succeeded, 0 if it failed.
 *
 * TODO:
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
    (void)job; (void)open_redirects; (void)child_exec; (void)run_builtin;
    return 1;
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

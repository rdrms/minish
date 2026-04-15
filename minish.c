#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_TOKENS 256
#define MAX_CMDS 16

int last_status = 0;

/* --- 1. The "Messy Cat" Fix (Invisible) --- */
void ensure_fresh_line() {
    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) return;
    struct termios term, raw;
    tcgetattr(STDIN_FILENO, &term);
    raw = term;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[6n");
    fflush(stdout);
    char buf[32]; int i = 0;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    int r, c;
    // If the cursor is not at column 1, just print a newline silently
    if (sscanf(buf, "\033[%d;%dR", &r, &c) == 2 && c != 1) {
        printf("\n");
    }
}

/* --- 2. Tokenizer & Expansion --- */
char* expand_vars(char* t) {
    if (t[0] == '~' && (t[1] == '\0' || t[1] == '/')) {
        char* home = getenv("HOME");
        if (!home) home = ""; 
        char* expanded = malloc(strlen(home) + strlen(t)); 
        sprintf(expanded, "%s%s", home, t + 1);
        return expanded;
    }
    if (t[0] == '$') {
        if (strcmp(t, "$?") == 0) { 
            char* s = malloc(16); 
            sprintf(s, "%d", last_status); 
            return s; 
        }
        char* e = getenv(t + 1); 
        return e ? strdup(e) : strdup("");
    }
    return strdup(t);
}

int tokenize(char *line, char *tokens[]) {
    int count = 0; char *p = line; int s=0, d=0; char *start = NULL;
    while (*p) {
        if (!s && !d && strchr(" \t\n", *p)) {
            if (start) { *p = '\0'; tokens[count++] = expand_vars(start); start = NULL; }
        } else {
            if (*p == '\'' && !d) s = !s; else if (*p == '\"' && !s) d = !d;
            if (!start) start = p;
        }
        p++;
    }
    if (start) tokens[count++] = expand_vars(start);
    tokens[count] = NULL;
    return count;
}

/* --- 3. The Pipeline Executor --- */
int execute_pipeline(char **tokens) {
    int num_cmds = 0;
    char **cmds[MAX_CMDS];
    cmds[num_cmds++] = tokens;

    for (int i = 0; tokens[i] != NULL; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            tokens[i] = NULL;
            cmds[num_cmds++] = &tokens[i + 1];
        }
    }

    if (num_cmds == 1) {
        if (strcmp(cmds[0][0], "exit") == 0) exit(last_status);
        if (strcmp(cmds[0][0], "cd") == 0) {
            if (chdir(cmds[0][1] ? cmds[0][1] : getenv("HOME")) != 0) perror("cd");
            return 0;
        }
    }

    int pipefds[2 * (num_cmds - 1)];
    for (int i = 0; i < num_cmds - 1; i++) pipe(pipefds + i * 2);

    for (int i = 0; i < num_cmds; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            if (i < num_cmds - 1) dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            for (int j = 0; j < 2 * (num_cmds - 1); j++) close(pipefds[j]);

            char *args[MAX_TOKENS]; int arg_c = 0;
            for (int k = 0; cmds[i][k] != NULL; k++) {
                if (strcmp(cmds[i][k], ">") == 0) {
                    int fd = open(cmds[i][++k], O_WRONLY|O_CREAT|O_TRUNC, 0644);
                    dup2(fd, STDOUT_FILENO); close(fd);
                } else if (strcmp(cmds[i][k], ">>") == 0) {
                    int fd = open(cmds[i][++k], O_WRONLY|O_CREAT|O_APPEND, 0644);
                    dup2(fd, STDOUT_FILENO); close(fd);
                } else if (strcmp(cmds[i][k], "<") == 0) {
                    int fd = open(cmds[i][++k], O_RDONLY);
                    dup2(fd, STDIN_FILENO); close(fd);
                } else args[arg_c++] = cmds[i][k];
            }
            args[arg_c] = NULL;
            execvp(args[0], args);
            perror(args[0]); exit(127);
        }
    }

    for (int i = 0; i < 2 * (num_cmds - 1); i++) close(pipefds[i]);
    int status;
    for (int i = 0; i < num_cmds; i++) wait(&status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* --- 4. Logic Wrapper (&&, ||) --- */
void run_line(char *line) {
    char *tokens[MAX_TOKENS];
    int n = tokenize(line, tokens);
    if (n == 0) return;

    char *current_seg[MAX_TOKENS];
    int seg_idx = 0, skip = 0;

    for (int i = 0; i <= n; i++) {
        if (i == n || strcmp(tokens[i], "&&") == 0 || strcmp(tokens[i], "||") == 0) {
            current_seg[seg_idx] = NULL;
            if (!skip) last_status = execute_pipeline(current_seg);
            if (i < n) {
                if (strcmp(tokens[i], "&&") == 0) skip = (last_status != 0);
                else if (strcmp(tokens[i], "||") == 0) skip = (last_status == 0);
            }
            seg_idx = 0;
        } else current_seg[seg_idx++] = tokens[i];
    }
    for (int i = 0; i < n; i++) free(tokens[i]);
}

int main() {
    rl_variable_bind("editing-mode", "vi");
    while (1) {
        ensure_fresh_line();
        char cwd[1024], display[1024], prompt[1200], *home = getenv("HOME");
        getcwd(cwd, sizeof(cwd));
        if (home && strncmp(cwd, home, strlen(home)) == 0) snprintf(display, 1024, "~%s", cwd + strlen(home));
        else strcpy(display, cwd);
        snprintf(prompt, 1200, "\001\033[1;36m\002%s\001\033[0m\002 \001\033[1;32m\002$\001\033[0m\002 ", display);

        char *line = readline(prompt);
        if (!line) break;
        if (*line) { add_history(line); char *c = strdup(line); run_line(c); free(c); }
        free(line);
    }
    return 0;
}
// シェル。

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// 解析済みコマンドの表現
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct command {
    int type;
};

struct exec_command {
    int type;
    char *argv[MAXARGS];
    char *argument_ends[MAXARGS];
};

struct redirection_command {
    int type;
    struct command *child_command;
    char *file;
    char *file_end;
    int mode;
    int fd;
};

struct pipe_command {
    int type;
    struct command *left;
    struct command *right;
};

struct list_command {
    int type;
    struct command *left;
    struct command *right;
};

struct background_command {
    int type;
    struct command *child_command;
};

int fork_or_panic(void); // forkする。失敗時はpanicする。
void panic(char *message);
struct command *parse_command(char *input);
void run_command(struct command *command) __attribute__((noreturn));

// commandを実行する。戻らない。
void run_command(struct command *command)
{
    int pipe_fds[2];
    struct background_command *background;
    struct exec_command *executable;
    struct list_command *list;
    struct pipe_command *pipeline;
    struct redirection_command *redirection;

    if (command == 0)
        exit(1);

    switch (command->type) {
    default:
        panic("run_command");

    case EXEC:
        executable = (struct exec_command *)command;
        if (executable->argv[0] == 0)
            exit(1);
        exec(executable->argv[0], executable->argv);
        fprintf(2, "exec %s failed\n", executable->argv[0]);
        break;

    case REDIR:
        redirection = (struct redirection_command *)command;
        close(redirection->fd);
        if (open(redirection->file, redirection->mode) < 0) {
            fprintf(2, "open %s failed\n", redirection->file);
            exit(1);
        }
        run_command(redirection->child_command);
        break;

    case LIST:
        list = (struct list_command *)command;
        if (fork_or_panic() == 0)
            run_command(list->left);
        wait(0);
        run_command(list->right);
        break;

    case PIPE:
        pipeline = (struct pipe_command *)command;
        if (pipe(pipe_fds) < 0)
            panic("pipe");
        if (fork_or_panic() == 0) {
            close(1);
            dup(pipe_fds[1]);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            run_command(pipeline->left);
        }
        if (fork_or_panic() == 0) {
            close(0);
            dup(pipe_fds[0]);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            run_command(pipeline->right);
        }
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        wait(0);
        wait(0);
        break;

    case BACK:
        background = (struct background_command *)command;
        if (fork_or_panic() == 0)
            run_command(background->child_command);
        break;
    }
    exit(0);
}

int read_command(char *buffer, int buffer_size)
{
    write(2, "$ ", 2);
    memset(buffer, 0, buffer_size);
    gets(buffer, buffer_size);
    if (buffer[0] == 0) // ファイル終端
        return -1;
    return 0;
}

int main(void)
{
    static char command_buffer[100];
    int fd;

    // 3つのファイルディスクリプタが開いていることを保証する。
    while ((fd = open("console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    // 入力コマンドを読み取って実行する。
    while (read_command(command_buffer, sizeof(command_buffer)) >= 0) {
        char *command = command_buffer;
        while (*command == ' ' || *command == '\t')
            command++;
        if (*command == '\n') // 空のコマンドか
            continue;
        if (command[0] == 'c' && command[1] == 'd' && command[2] == ' ') {
            // Chdirは子ではなく親が呼ばなければならない。
            command[strlen(command) - 1] = 0; // \nを削る
            if (chdir(command + 3) < 0)
                fprintf(2, "cannot cd %s\n", command + 3);
        } else {
            if (fork_or_panic() == 0)
                run_command(parse_command(command));
            wait(0);
        }
    }
    exit(0);
}

void panic(char *message)
{
    fprintf(2, "%s\n", message);
    exit(1);
}

int fork_or_panic(void)
{
    int pid;

    pid = fork();
    if (pid == -1)
        panic("fork");
    return pid;
}

//PAGEBREAK!
// コンストラクタ

struct command *create_exec_command(void)
{
    struct exec_command *command;

    command = malloc(sizeof(*command));
    memset(command, 0, sizeof(*command));
    command->type = EXEC;
    return (struct command *)command;
}

struct command *create_redirection_command(struct command *child_command,
                                           char *file, char *file_end,
                                           int mode, int fd)
{
    struct redirection_command *command;

    command = malloc(sizeof(*command));
    memset(command, 0, sizeof(*command));
    command->type = REDIR;
    command->child_command = child_command;
    command->file = file;
    command->file_end = file_end;
    command->mode = mode;
    command->fd = fd;
    return (struct command *)command;
}

struct command *create_pipe_command(struct command *left, struct command *right)
{
    struct pipe_command *command;

    command = malloc(sizeof(*command));
    memset(command, 0, sizeof(*command));
    command->type = PIPE;
    command->left = left;
    command->right = right;
    return (struct command *)command;
}

struct command *create_list_command(struct command *left, struct command *right)
{
    struct list_command *command;

    command = malloc(sizeof(*command));
    memset(command, 0, sizeof(*command));
    command->type = LIST;
    command->left = left;
    command->right = right;
    return (struct command *)command;
}

struct command *create_background_command(struct command *child_command)
{
    struct background_command *command;

    command = malloc(sizeof(*command));
    memset(command, 0, sizeof(*command));
    command->type = BACK;
    command->child_command = child_command;
    return (struct command *)command;
}
//PAGEBREAK!
// 解析

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int read_token(char **position, char *input_end,
               char **token_start_out, char **token_end_out)
{
    char *cursor;
    int token_type;

    cursor = *position;
    while (cursor < input_end && strchr(whitespace, *cursor))
        cursor++;
    if (token_start_out)
        *token_start_out = cursor;
    token_type = *cursor;
    switch (*cursor) {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        cursor++;
        break;
    case '>':
        cursor++;
        if (*cursor == '>') {
            token_type = '+';
            cursor++;
        }
        break;
    default:
        token_type = 'a';
        while (cursor < input_end && !strchr(whitespace, *cursor) &&
               !strchr(symbols, *cursor))
            cursor++;
        break;
    }
    if (token_end_out)
        *token_end_out = cursor;

    while (cursor < input_end && strchr(whitespace, *cursor))
        cursor++;
    *position = cursor;
    return token_type;
}

int next_token_is_one_of(char **position, char *input_end,
                         char *token_characters)
{
    char *cursor;

    cursor = *position;
    while (cursor < input_end && strchr(whitespace, *cursor))
        cursor++;
    *position = cursor;
    return *cursor && strchr(token_characters, *cursor);
}

struct command *parse_line(char **position, char *input_end);
struct command *parse_pipe(char **position, char *input_end);
struct command *parse_exec(char **position, char *input_end);
struct command *terminate_strings(struct command *command);

struct command *parse_command(char *input)
{
    char *input_end;
    char *position;
    struct command *command;

    position = input;
    input_end = input + strlen(input);
    command = parse_line(&position, input_end);
    next_token_is_one_of(&position, input_end, "");
    if (position != input_end) {
        fprintf(2, "leftovers: %s\n", position);
        panic("syntax");
    }
    terminate_strings(command);
    return command;
}

struct command *parse_line(char **position, char *input_end)
{
    struct command *command;

    command = parse_pipe(position, input_end);
    while (next_token_is_one_of(position, input_end, "&")) {
        read_token(position, input_end, 0, 0);
        command = create_background_command(command);
    }
    if (next_token_is_one_of(position, input_end, ";")) {
        read_token(position, input_end, 0, 0);
        command = create_list_command(command,
                                      parse_line(position, input_end));
    }
    return command;
}

struct command *parse_pipe(char **position, char *input_end)
{
    struct command *command;

    command = parse_exec(position, input_end);
    if (next_token_is_one_of(position, input_end, "|")) {
        read_token(position, input_end, 0, 0);
        command = create_pipe_command(command,
                                      parse_pipe(position, input_end));
    }
    return command;
}

struct command *parse_redirections(struct command *command, char **position,
                                   char *input_end)
{
    int token_type;
    char *file_start, *file_end;

    while (next_token_is_one_of(position, input_end, "<>")) {
        token_type = read_token(position, input_end, 0, 0);
        if (read_token(position, input_end, &file_start, &file_end) != 'a')
            panic("missing file for redirection");
        switch (token_type) {
        case '<':
            command = create_redirection_command(
                command, file_start, file_end, O_RDONLY, 0);
            break;
        case '>':
            command = create_redirection_command(
                command, file_start, file_end,
                O_WRONLY | O_CREATE | O_TRUNC, 1);
            break;
        case '+': // >>
            command = create_redirection_command(
                command, file_start, file_end, O_WRONLY | O_CREATE, 1);
            break;
        }
    }
    return command;
}

struct command *parse_block(char **position, char *input_end)
{
    struct command *command;

    if (!next_token_is_one_of(position, input_end, "("))
        panic("parse_block");
    read_token(position, input_end, 0, 0);
    command = parse_line(position, input_end);
    if (!next_token_is_one_of(position, input_end, ")"))
        panic("syntax - missing )");
    read_token(position, input_end, 0, 0);
    command = parse_redirections(command, position, input_end);
    return command;
}

struct command *parse_exec(char **position, char *input_end)
{
    char *argument_start, *argument_end;
    int token_type, argc;
    struct exec_command *command;
    struct command *parsed_command;

    if (next_token_is_one_of(position, input_end, "("))
        return parse_block(position, input_end);

    parsed_command = create_exec_command();
    command = (struct exec_command *)parsed_command;

    argc = 0;
    parsed_command = parse_redirections(parsed_command, position, input_end);
    while (!next_token_is_one_of(position, input_end, "|)&;")) {
        if ((token_type = read_token(position, input_end,
                                     &argument_start, &argument_end)) == 0)
            break;
        if (token_type != 'a')
            panic("syntax");
        command->argv[argc] = argument_start;
        command->argument_ends[argc] = argument_end;
        argc++;
        if (argc >= MAXARGS)
            panic("too many args");
        parsed_command = parse_redirections(parsed_command, position,
                                            input_end);
    }
    command->argv[argc] = 0;
    command->argument_ends[argc] = 0;
    return parsed_command;
}

// 長さ付き文字列をすべてNUL終端する。
struct command *terminate_strings(struct command *command)
{
    int i;
    struct background_command *background;
    struct exec_command *executable;
    struct list_command *list;
    struct pipe_command *pipeline;
    struct redirection_command *redirection;

    if (command == 0)
        return 0;

    switch (command->type) {
    case EXEC:
        executable = (struct exec_command *)command;
        for (i = 0; executable->argv[i]; i++)
            *executable->argument_ends[i] = 0;
        break;

    case REDIR:
        redirection = (struct redirection_command *)command;
        terminate_strings(redirection->child_command);
        *redirection->file_end = 0;
        break;

    case PIPE:
        pipeline = (struct pipe_command *)command;
        terminate_strings(pipeline->left);
        terminate_strings(pipeline->right);
        break;

    case LIST:
        list = (struct list_command *)command;
        terminate_strings(list->left);
        terminate_strings(list->right);
        break;

    case BACK:
        background = (struct background_command *)command;
        terminate_strings(background->child_command);
        break;
    }
    return command;
}

#ifndef CMD_H
#define CMD_H

#define MAX_ARGS 64 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

typedef struct {
    char *args[MAX_ARGS];
    char *input_file;
    char *output_file;
    char *error_file;
} Command;

int parse_command(Command *cmd, char *cmd_str);

#endif //CMD_H

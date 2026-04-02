#ifndef HELPER_CMD_C_H
#define HELPER_CMD_C_H

/*
 * helper_cmd_c.h - Public interface for the "c-func" CLI command
 *
 * Provides direct interactive access to SOEM wrapper functions from the CLI.
 *
 * Syntax:
 *   c-func <function_name> [arg1] [arg2] ...
 *
 * See cmd_c_funcs() for the full list of supported functions.
 */

/**
 * Handle the "c-func" CLI command.
 *
 * Dispatches argv[1] to the matching SOEM function, forwarding any remaining
 * arguments as parameters.  Prints usage information when called with no
 * arguments (argc < 2).
 *
 * @param argc  Total argument count (argv[0] is the command name "c-func").
 * @param argv  Argument vector; argv[1] is the SOEM function name.
 */
void cmd_c_funcs(int argc, char **argv);

#endif /* HELPER_CMD_C_H */
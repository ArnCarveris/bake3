#ifndef BAKE3_COMMANDS_H
#define BAKE3_COMMANDS_H

#include "bake/build.h"

void bake_print_help(void);
bool bake_is_command(const char *arg);
bool bake_command_builds(const char *command);
int bake_execute(bake_context_t *ctx, const char *argv0);
int bake_repeat_build(const bake_options_t *opts, int argc, char *argv[]);

#endif

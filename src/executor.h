/*
 * executor.h — command execution engine for xvsh
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "xvsh.h"
#include "parser.h"

int exec_cmdlist(ShellState *sh, CmdList *cl);
int exec_pipeline(ShellState *sh, Pipeline *pl);
int exec_simple(ShellState *sh, SimpleCmd *sc);

#endif /* EXECUTOR_H */

/*
 * completion.h — fish-style ghost text + command db for xvsh
 */
#ifndef COMPLETION_H
#define COMPLETION_H

/*
 * Create ~/xvsh/ and set up the db path.
 * Call once before rl_initialize().
 */
void completion_init(void);

/*
 * Hook readline's redisplay function and bind Right-arrow / End
 * to accept the ghost suggestion.
 * Call once after rl_initialize().
 */
void completion_setup(void);

/*
 * Append `cmd` to ~/xvsh/command.db.
 * Call from the REPL after the user submits a non-empty line.
 */
void db_record(const char *cmd);

#endif /* COMPLETION_H */

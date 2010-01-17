#ifndef ADVICE_H
#define ADVICE_H

#include "git-compat-util.h"

extern int advice_push_nonfastforward;
extern int advice_status_hints;
extern int advice_commit_before_merge;
extern int advice_resolve_conflict;

int git_default_advice_config(const char *var, const char *value);

extern void NORETURN die_resolve_conflict(const char *me);

#endif /* ADVICE_H */

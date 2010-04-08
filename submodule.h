#ifndef SUBMODULE_H
#define SUBMODULE_H

void show_submodule_summary(FILE *f, const char *path,
		unsigned char one[20], unsigned char two[20],
		unsigned dirty_submodule,
		const char *del, const char *add, const char *reset);
unsigned is_submodule_modified(const char *path, int ignore_untracked);
int checkout_submodule(const char *path, const unsigned char sha1[20], int force);

#endif

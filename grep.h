#ifndef GREP_H
#define GREP_H
#include "color.h"

enum grep_pat_token {
	GREP_PATTERN,
	GREP_PATTERN_HEAD,
	GREP_PATTERN_BODY,
	GREP_AND,
	GREP_OPEN_PAREN,
	GREP_CLOSE_PAREN,
	GREP_NOT,
	GREP_OR,
};

enum grep_context {
	GREP_CONTEXT_HEAD,
	GREP_CONTEXT_BODY,
};

enum grep_header_field {
	GREP_HEADER_AUTHOR = 0,
	GREP_HEADER_COMMITTER,
};

struct grep_pat {
	struct grep_pat *next;
	const char *origin;
	int no;
	enum grep_pat_token token;
	const char *pattern;
	enum grep_header_field field;
	regex_t regexp;
	unsigned fixed:1;
	unsigned ignore_case:1;
	unsigned word_regexp:1;
};

enum grep_expr_node {
	GREP_NODE_ATOM,
	GREP_NODE_NOT,
	GREP_NODE_AND,
	GREP_NODE_OR,
};

struct grep_expr {
	enum grep_expr_node node;
	unsigned hit;
	union {
		struct grep_pat *atom;
		struct grep_expr *unary;
		struct {
			struct grep_expr *left;
			struct grep_expr *right;
		} binary;
	} u;
};

struct grep_opt {
	struct grep_pat *pattern_list;
	struct grep_pat **pattern_tail;
	struct grep_expr *pattern_expression;
	const char *prefix;
	int prefix_length;
	regex_t regexp;
	int linenum;
	int invert;
	int ignore_case;
	int status_only;
	int name_only;
	int unmatch_name_only;
	int count;
	int word_regexp;
	int fixed;
	int all_match;
#define GREP_BINARY_DEFAULT	0
#define GREP_BINARY_NOMATCH	1
#define GREP_BINARY_TEXT	2
	int binary;
	int extended;
	int relative;
	int pathname;
	int null_following_name;
	int color;
	int max_depth;
	int funcname;
	char color_match[COLOR_MAXLEN];
	int regflags;
	unsigned pre_context;
	unsigned post_context;
	unsigned last_shown;
	int show_hunk_mark;
	void *priv;
};

extern void append_grep_pattern(struct grep_opt *opt, const char *pat, const char *origin, int no, enum grep_pat_token t);
extern void append_header_grep_pattern(struct grep_opt *, enum grep_header_field, const char *);
extern void compile_grep_patterns(struct grep_opt *opt);
extern void free_grep_patterns(struct grep_opt *opt);
extern int grep_buffer(struct grep_opt *opt, const char *name, char *buf, unsigned long size);

#endif

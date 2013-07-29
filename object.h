#ifndef OBJECT_H
#define OBJECT_H

struct object_list {
	struct object *item;
	struct object_list *next;
};

struct object_array {
	unsigned int nr;
	unsigned int alloc;
	struct object_array_entry {
		struct object *item;
		/*
		 * name or NULL.  If non-NULL, the memory pointed to
		 * is owned by this object *except* if it points at
		 * object_array_slopbuf, which is a static copy of the
		 * empty string.
		 */
		char *name;
		unsigned mode;
		struct object_context *context;
	} *objects;
};

#define OBJECT_ARRAY_INIT { 0, 0, NULL }

#define TYPE_BITS   3
#define FLAG_BITS  27

/*
 * The object type is stored in 3 bits.
 */
struct object {
	unsigned parsed : 1;
	unsigned used : 1;
	unsigned type : TYPE_BITS;
	unsigned flags : FLAG_BITS;
	unsigned char sha1[20];
};

extern const char *typename(unsigned int type);
extern int type_from_string(const char *str);

extern unsigned int get_max_object_index(void);
extern struct object *get_indexed_object(unsigned int);

/*
 * This can be used to see if we have heard of the object before, but
 * it can return "yes we have, and here is a half-initialised object"
 * for an object that we haven't loaded/parsed yet.
 *
 * When parsing a commit to create an in-core commit object, its
 * parents list holds commit objects that represent its parents, but
 * they are expected to be lazily initialized and do not know what
 * their trees or parents are yet.  When this function returns such a
 * half-initialised objects, the caller is expected to initialize them
 * by calling parse_object() on them.
 */
struct object *lookup_object(const unsigned char *sha1);

extern void *create_object(const unsigned char *sha1, int type, void *obj);

/*
 * Returns the object, having parsed it to find out what it is.
 *
 * Returns NULL if the object is missing or corrupt.
 */
struct object *parse_object(const unsigned char *sha1);

/*
 * Like parse_object, but will die() instead of returning NULL. If the
 * "name" parameter is not NULL, it is included in the error message
 * (otherwise, the sha1 hex is given).
 */
struct object *parse_object_or_die(const unsigned char *sha1, const char *name);

/* Given the result of read_sha1_file(), returns the object after
 * parsing it.  eaten_p indicates if the object has a borrowed copy
 * of buffer and the caller should not free() it.
 */
struct object *parse_object_buffer(const unsigned char *sha1, enum object_type type, unsigned long size, void *buffer, int *eaten_p);

/** Returns the object, with potentially excess memory allocated. **/
struct object *lookup_unknown_object(const unsigned  char *sha1);

struct object_list *object_list_insert(struct object *item,
				       struct object_list **list_p);

int object_list_contains(struct object_list *list, struct object *obj);

/* Object array handling .. */
void add_object_array(struct object *obj, const char *name, struct object_array *array);
void add_object_array_with_mode(struct object *obj, const char *name, struct object_array *array, unsigned mode);
void add_object_array_with_context(struct object *obj, const char *name, struct object_array *array, struct object_context *context);

typedef int (*object_array_each_func_t)(struct object_array_entry *, void *);

/*
 * Apply want to each entry in array, retaining only the entries for
 * which the function returns true.  Preserve the order of the entries
 * that are retained.
 */
void object_array_filter(struct object_array *array,
			 object_array_each_func_t want, void *cb_data);

/*
 * Remove from array all but the first entry with a given name.
 * Warning: this function uses an O(N^2) algorithm.
 */
void object_array_remove_duplicates(struct object_array *array);

void clear_object_flags(unsigned flags);

#endif /* OBJECT_H */

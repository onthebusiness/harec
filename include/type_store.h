#ifndef HARE_TYPESTORE_H
#define HARE_TYPESTORE_H
#include "ast.h"
#include "types.h"

#define TYPE_STORE_BUCKETS 65536

struct type_bucket {
	struct type type;
	struct type_bucket *next;
};

struct context;

struct type_store {
	struct type_bucket *buckets[TYPE_STORE_BUCKETS];
	struct context *check_context;
};

struct ast_type;

const struct type *type_store_lookup_atype(
	struct type_store *store, const struct ast_type *atype);

const struct type *builtin_type_for_storage(
	enum type_storage storage, bool is_const);

const struct type *type_store_lookup_with_flags(struct type_store *store,
	const struct type *type, unsigned int flags);

const struct type *type_store_lookup_pointer(struct type_store *store,
	const struct type *referent, unsigned int ptrflags);

const struct type *type_store_lookup_array(struct type_store *store,
	const struct type *members, size_t len);

const struct type *type_store_lookup_slice(struct type_store *store,
	const struct type *members);

const struct type *type_store_lookup_alias(struct type_store *store,
	const struct identifier *ident, const struct type *secondary);

const struct type *type_store_lookup_tagged(struct type_store *store,
	struct type_tagged_union *tags);

const struct type *type_store_lookup_tuple(struct type_store *store,
	struct type_tuple *values);

#endif

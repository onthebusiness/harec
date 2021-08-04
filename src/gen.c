#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "expr.h"
#include "gen.h"
#include "scope.h"
#include "types.h"
#include "util.h"

static const struct gen_value gv_void = {
	.kind = GV_CONST,
	.type = &builtin_type_void,
};

static void
gen_copy_memcpy(struct gen_context *ctx,
	struct gen_value dest, struct gen_value src)
{
	struct qbe_value rtfunc = mkrtfunc(ctx, "rt.memcpy");
	struct qbe_value dtemp = mklval(ctx, &dest);
	struct qbe_value stemp = mklval(ctx, &src);
	struct qbe_value sz = constl(dest.type->size);
	pushi(ctx->current, NULL, Q_CALL, &rtfunc, &dtemp, &stemp, &sz, NULL);
}

static void
gen_copy_aligned(struct gen_context *ctx,
	struct gen_value dest, struct gen_value src)
{
	if (dest.type->size > 128) {
		gen_copy_memcpy(ctx, dest, src);
		return;
	}
	enum qbe_instr load, store;
	assert(dest.type->align && (dest.type->align & (dest.type->align - 1)) == 0);
	switch (dest.type->align) {
	case 1: load = Q_LOADUB, store = Q_STOREB; break;
	case 2: load = Q_LOADUH, store = Q_STOREH; break;
	case 4: load = Q_LOADUW, store = Q_STOREW; break;
	default:
		assert(dest.type->align == 8);
		load = Q_LOADL, store = Q_STOREL;
		break;
	}
	struct qbe_value temp = {
		.kind = QV_TEMPORARY,
		.type = ctx->arch.ptr,
		.name = gen_name(ctx, ".%d"),
	};
	struct qbe_value destp = mkcopy(ctx, &dest, ".%d");
	struct qbe_value srcp = mkcopy(ctx, &src, ".%d");
	struct qbe_value align = constl(dest.type->align);
	for (size_t offset = 0; offset < dest.type->size;
			offset += dest.type->align) {
		pushi(ctx->current, &temp, load, &srcp, NULL);
		pushi(ctx->current, NULL, store, &temp, &destp, NULL);
		if (offset + dest.type->align < dest.type->size) {
			pushi(ctx->current, &srcp, Q_ADD, &srcp, &align, NULL);
			pushi(ctx->current, &destp, Q_ADD, &destp, &align, NULL);
		}
	}
}

static void
gen_store(struct gen_context *ctx,
	struct gen_value object,
	struct gen_value value)
{
	switch (type_dealias(object.type)->storage) {
	case STORAGE_ARRAY:
	case STORAGE_SLICE:
	case STORAGE_STRING:
	case STORAGE_STRUCT:
	case STORAGE_TUPLE:
		gen_copy_aligned(ctx, object, value);
		return;
	case STORAGE_TAGGED:
		assert(0); // TODO
	case STORAGE_UNION:
		gen_copy_memcpy(ctx, object, value);
		return;
	case STORAGE_ENUM:
		assert(0); // TODO
	default:
		break; // no-op
	}

	struct qbe_value qobj = mkqval(ctx, &object),
		qval = mkqval(ctx, &value);
	enum qbe_instr qi = store_for_type(ctx, object.type);
	pushi(ctx->current, NULL, qi, &qval, &qobj, NULL);
}

static struct gen_value
gen_load(struct gen_context *ctx, struct gen_value object)
{
	switch (type_dealias(object.type)->storage) {
	case STORAGE_ARRAY:
	case STORAGE_FUNCTION:
	case STORAGE_SLICE:
	case STORAGE_STRING:
	case STORAGE_STRUCT:
	case STORAGE_TAGGED:
	case STORAGE_TUPLE:
	case STORAGE_UNION:
		return object;
	case STORAGE_ENUM:
		assert(0); // TODO
	default:
		break; // no-op
	}

	struct gen_value value = {
		.kind = GV_TEMP,
		.type = object.type,
		.name = gen_name(ctx, "load.%d"),
	};
	struct qbe_value qobj = mkqval(ctx, &object),
		qval = mkqval(ctx, &value);
	enum qbe_instr qi = load_for_type(ctx, object.type);
	pushi(ctx->current, &qval, qi, &qobj, NULL);
	return value;
}

static struct gen_value
gen_autoderef(struct gen_context *ctx, struct gen_value val)
{
	while (type_dealias(val.type)->storage == STORAGE_POINTER) {
		val.type = type_dealias(val.type)->pointer.referent;
		val = gen_load(ctx, val);
	}
	return val;
}

static struct gen_value gen_expr(struct gen_context *ctx,
	const struct expression *expr);
static void gen_expr_at(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value out);
static struct gen_value gen_expr_with(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value *out);

static struct gen_value
gen_access_ident(struct gen_context *ctx, const struct expression *expr)
{
	const struct scope_object *obj = expr->access.object;
	switch (obj->otype) {
	case O_BIND:
		for (const struct gen_binding *gb = ctx->bindings;
				gb; gb = gb->next) {
			if (gb->object == obj) {
				return gb->value;
			}
		}
		break;
	case O_DECL:
		return (struct gen_value){
			.kind = GV_GLOBAL,
			.type = obj->type,
			.name = ident_to_sym(&obj->ident),
		};
	case O_CONST:
	case O_TYPE:
		abort(); // Invariant
	}
	abort(); // Invariant
}

static struct gen_value
gen_access_index(struct gen_context *ctx, const struct expression *expr)
{
	struct gen_value glval = gen_expr(ctx, expr->access.array);
	glval = gen_autoderef(ctx, glval);
	struct qbe_value qlval = mkqval(ctx, &glval);
	struct qbe_value qival = mkqtmp(ctx, ctx->arch.ptr, ".%d");

	struct gen_value index = gen_expr(ctx, expr->access.index);
	struct qbe_value qindex = mkqval(ctx, &index);
	struct qbe_value itemsz = constl(expr->result->size);
	pushi(ctx->current, &qival, Q_MUL, &qindex, &itemsz, NULL);
	pushi(ctx->current, &qival, Q_ADD, &qlval, &qival, NULL);

	// TODO: Check bounds

	return (struct gen_value){
		.kind = GV_TEMP,
		.type = expr->result,
		.name = qival.name,
	};
}

static struct gen_value
gen_access_field(struct gen_context *ctx, const struct expression *expr)
{
	const struct struct_field *field = expr->access.field;
	struct gen_value glval = gen_expr(ctx, expr->access._struct);
	glval = gen_autoderef(ctx, glval);
	struct qbe_value qlval = mkqval(ctx, &glval);
	struct qbe_value qfval = mkqtmp(ctx, ctx->arch.ptr, "field.%d");
	struct qbe_value offs = constl(field->offset);
	pushi(ctx->current, &qfval, Q_ADD, &qlval, &offs, NULL);
	return (struct gen_value){
		.kind = GV_TEMP,
		.type = field->type,
		.name = qfval.name,
	};
}

static struct gen_value
gen_access_value(struct gen_context *ctx, const struct expression *expr)
{
	const struct type_tuple *tuple = expr->access.tvalue;
	struct gen_value glval = gen_expr(ctx, expr->access.tuple);
	glval = gen_autoderef(ctx, glval);
	struct qbe_value qlval = mkqval(ctx, &glval);
	struct qbe_value qfval = mkqtmp(ctx, ctx->arch.ptr, "value.%d");
	struct qbe_value offs = constl(tuple->offset);
	pushi(ctx->current, &qfval, Q_ADD, &qlval, &offs, NULL);
	return (struct gen_value){
		.kind = GV_TEMP,
		.type = tuple->type,
		.name = qfval.name,
	};
}

static struct gen_value
gen_expr_access_addr(struct gen_context *ctx, const struct expression *expr)
{
	struct gen_value addr;
	switch (expr->access.type) {
	case ACCESS_IDENTIFIER:
		addr = gen_access_ident(ctx, expr);
		break;
	case ACCESS_INDEX:
		addr = gen_access_index(ctx, expr);
		break;
	case ACCESS_FIELD:
		addr = gen_access_field(ctx, expr);
		break;
	case ACCESS_TUPLE:
		addr = gen_access_value(ctx, expr);
		break;
	}
	return addr;
}

static struct gen_value
gen_expr_access(struct gen_context *ctx, const struct expression *expr)
{
	struct gen_value addr = gen_expr_access_addr(ctx, expr);
	return gen_load(ctx, addr);
}

static struct gen_value
gen_expr_assert(struct gen_context *ctx, const struct expression *expr)
{
	assert(expr->assert.message); // Invariant
	if (expr->assert.is_static) {
		return gv_void;
	}

	struct qbe_statement failedl = {0}, passedl = {0};
	struct qbe_value bfailed = {0}, bpassed = {0};
	struct qbe_value rtfunc = mkrtfunc(ctx, "rt.abort");
	struct gen_value msg;

	if (expr->assert.cond) {
		bfailed.kind = QV_LABEL;
		bfailed.name = strdup(genl(&failedl, &ctx->id, "failed.%d"));
		bpassed.kind = QV_LABEL;
		bpassed.name = strdup(genl(&passedl, &ctx->id, "passed.%d"));

		struct gen_value cond = gen_expr(ctx, expr->assert.cond);
		struct qbe_value qcond = mkqval(ctx, &cond);
		pushi(ctx->current, NULL, Q_JNZ, &qcond, &bpassed, &bfailed, NULL);
		push(&ctx->current->body, &failedl);
		msg = gen_expr(ctx, expr->assert.message);
	} else {
		msg = gen_expr(ctx, expr->assert.message);
	}

	struct qbe_value qmsg = mkqval(ctx, &msg);
	pushi(ctx->current, NULL, Q_CALL, &rtfunc, &qmsg, NULL);

	if (expr->assert.cond) {
		push(&ctx->current->body, &passedl);
	}

	return gv_void;
}

static struct gen_value
gen_expr_assign(struct gen_context *ctx, const struct expression *expr)
{
	struct expression *object = expr->assign.object;
	struct expression *value = expr->assign.value;
	assert(object->type == EXPR_ACCESS || expr->assign.indirect); // Invariant
	assert(object->type != EXPR_SLICE); // TODO

	struct gen_value obj;
	if (expr->assign.indirect) {
		obj = gen_expr(ctx, object);
		obj.type = type_dealias(object->result)->pointer.referent;
	} else {
		obj = gen_expr_access_addr(ctx, object);
	}
	if (expr->assign.op == BIN_LEQUAL) {
		gen_store(ctx, obj, gen_expr(ctx, value));
	} else if (expr->assign.op == BIN_LAND || expr->assign.op == BIN_LOR) {
		assert(0); // TODO
	} else {
		struct gen_value lvalue = gen_load(ctx, obj);
		struct gen_value rvalue = gen_expr(ctx, value);
		struct qbe_value qlval = mkqval(ctx, &lvalue);
		struct qbe_value qrval = mkqval(ctx, &rvalue);
		enum qbe_instr instr = binarithm_for_op(ctx, expr->assign.op,
			lvalue.type);
		pushi(ctx->current, &qlval, instr, &qlval, &qrval, NULL);
		gen_store(ctx, obj, lvalue);
	}

	return gv_void;
}

static struct gen_value
gen_expr_binarithm(struct gen_context *ctx, const struct expression *expr)
{
	struct gen_value lvalue = gen_expr(ctx, expr->binarithm.lvalue);
	struct gen_value rvalue = gen_expr(ctx, expr->binarithm.rvalue);
	struct gen_value result = mktemp(ctx, expr->result, ".%d");
	struct qbe_value qlval = mkqval(ctx, &lvalue);
	struct qbe_value qrval = mkqval(ctx, &rvalue);
	struct qbe_value qresult = mkqval(ctx, &result);
	enum qbe_instr instr = binarithm_for_op(ctx, expr->binarithm.op,
		expr->binarithm.lvalue->result);
	pushi(ctx->current, &qresult, instr, &qlval, &qrval, NULL);
	return result;
}

static struct gen_value
gen_expr_binding(struct gen_context *ctx, const struct expression *expr)
{
	for (const struct expression_binding *binding = &expr->binding;
			binding; binding = binding->next) {
		const struct type *type = binding->initializer->result;
		struct gen_binding *gb = xcalloc(1, sizeof(struct gen_binding));
		gb->value.kind = GV_TEMP;
		gb->value.type = type;
		gb->value.name = gen_name(ctx, "binding.%d");
		gb->object = binding->object;
		gb->next = ctx->bindings;
		ctx->bindings = gb;

		struct qbe_value qv = mklval(ctx, &gb->value);
		struct qbe_value sz = constl(type->size);
		enum qbe_instr alloc = alloc_for_align(type->align);
		pushprei(ctx->current, &qv, alloc, &sz, NULL);
		gen_expr_at(ctx, binding->initializer, gb->value);
	}
	return gv_void;
}

static struct gen_value
gen_expr_call(struct gen_context *ctx, const struct expression *expr)
{
	struct gen_value lvalue = gen_expr(ctx, expr->call.lvalue);
	lvalue = gen_autoderef(ctx, lvalue);

	const struct type *rtype = lvalue.type;
	assert(rtype->storage == STORAGE_FUNCTION);
	// TODO: Run deferred expressions if rtype->func.flags & FN_NORETURN

	struct qbe_statement call = {
		.type = Q_INSTR,
		.instr = Q_CALL,
	};
	struct gen_value rval = gv_void;
	if (type_dealias(rtype->func.result)->storage != STORAGE_VOID) {
		rval = mktemp(ctx, rtype->func.result, "returns.%d");
		call.out = xcalloc(1, sizeof(struct qbe_value));
		*call.out = mkqval(ctx, &rval);
	}

	struct qbe_arguments *args, **next = &call.args;
	args = *next = xcalloc(1, sizeof(struct qbe_arguments));
	args->value = mkqval(ctx, &lvalue);
	next = &args->next;
	for (struct call_argument *carg = expr->call.args;
			carg; carg = carg->next) {
		args = *next = xcalloc(1, sizeof(struct qbe_arguments));
		struct gen_value arg = gen_expr(ctx, carg->value);
		args->value = mkqval(ctx, &arg);
		next = &args->next;
	}
	push(&ctx->current->body, &call);

	return rval;
}

static void
gen_const_array_at(struct gen_context *ctx,
	const struct expression *expr, struct gen_value out)
{
	struct array_constant *aexpr = expr->constant.array;
	assert(!aexpr->expand); // TODO
	struct qbe_value base = mkqval(ctx, &out);

	size_t index = 0;
	const struct type *atype = type_dealias(expr->result);
	struct gen_value item = mktemp(ctx, atype->array.members, "item.%d");
	for (const struct array_constant *ac = aexpr; ac; ac = ac->next) {
		struct qbe_value offs = constl(index * atype->array.members->size);
		struct qbe_value ptr = mklval(ctx, &item);
		pushi(ctx->current, &ptr, Q_ADD, &base, &offs, NULL);
		gen_expr_at(ctx, ac->value, item);
		++index;
	}
}

static void
gen_const_string_at(struct gen_context *ctx,
	const struct expression *expr, struct gen_value out)
{
	const struct expression_constant *constexpr = &expr->constant;
	const char *val = constexpr->string.value;
	size_t len = constexpr->string.len;

	// TODO: Generate string data structure as global also?
	struct qbe_value global = mkqtmp(ctx, ctx->arch.ptr, "strdata.%d");
	global.kind = QV_GLOBAL;

	struct qbe_def *def = xcalloc(1, sizeof(struct qbe_def));
	def->name = global.name;
	def->kind = Q_DATA;
	def->data.items.type = QD_STRING;
	def->data.items.str = xcalloc(1, len);
	memcpy(def->data.items.str, val, len);
	def->data.items.sz = len;

	if (len != 0) {
		qbe_append_def(ctx->out, def);
	} else {
		free(def);
		global = constl(0);
	}

	enum qbe_instr store = store_for_type(ctx, &builtin_type_size);
	struct qbe_value strp = mkcopy(ctx, &out, ".%d");
	struct qbe_value qlen = constl(len);
	struct qbe_value offs = constl(builtin_type_size.size);
	pushi(ctx->current, NULL, store, &qlen, &strp, NULL);
	pushi(ctx->current, &strp, Q_ADD, &strp, &offs, NULL);
	pushi(ctx->current, NULL, store, &qlen, &strp, NULL);
	pushi(ctx->current, &strp, Q_ADD, &strp, &offs, NULL);
	pushi(ctx->current, NULL, store, &global, &strp, NULL);
}

static void
gen_expr_const_at(struct gen_context *ctx,
	const struct expression *expr, struct gen_value out)
{
	if (!type_is_aggregate(type_dealias(expr->result))) {
		gen_store(ctx, out, gen_expr(ctx, expr));
		return;
	}

	switch (type_dealias(expr->result)->storage) {
	case STORAGE_ARRAY:
		gen_const_array_at(ctx, expr, out);
		break;
	case STORAGE_STRING:
		gen_const_string_at(ctx, expr, out);
		break;
	default:
		abort(); // Invariant
	}
}

static struct gen_value
gen_expr_const(struct gen_context *ctx, const struct expression *expr)
{
	if (type_is_aggregate(type_dealias(expr->result))) {
		struct gen_value out = mktemp(ctx, expr->result, "object.%d");
		struct qbe_value base = mkqval(ctx, &out);
		struct qbe_value sz = constl(expr->result->size);
		enum qbe_instr alloc = alloc_for_align(expr->result->align);
		pushprei(ctx->current, &base, alloc, &sz, NULL);
		gen_expr_at(ctx, expr, out);
		return out;
	}

	struct gen_value val = {
		.kind = GV_CONST,
		.type = expr->result,
	};

	// Special cases
	switch (type_dealias(expr->result)->storage) {
	case STORAGE_BOOL:
		val.wval = expr->constant.bval ? 1 : 0;
		return val;
	case STORAGE_VOID:
		return val;
	case STORAGE_NULL:
		val.lval = 0;
		return val;
	default:
		// Moving right along
		break;
	}

	const struct qbe_type *qtype = qtype_lookup(ctx, expr->result, false);
	switch (qtype->stype) {
	case Q_BYTE:
	case Q_HALF:
	case Q_WORD:
		val.wval = (uint32_t)expr->constant.uval;
		return val;
	case Q_LONG:
		val.lval = expr->constant.uval;
		return val;
	case Q_SINGLE:
		val.sval = (float)expr->constant.fval;
		return val;
	case Q_DOUBLE:
		val.dval = expr->constant.fval;
		return val;
	case Q__VOID:
		return val;
	case Q__AGGREGATE:
		assert(0); // Invariant
	}

	abort(); // Invariant
}

static struct gen_value
gen_expr_list_with(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value *out)
{
	// TODO: Set up defer scope
	for (const struct expressions *exprs = &expr->list.exprs;
			exprs; exprs = exprs->next) {
		if (!exprs->next) {
			return gen_expr_with(ctx, exprs->expr, out);
		}
		gen_expr(ctx, exprs->expr);
	}
	abort(); // Unreachable
}

static struct gen_value
gen_expr_return(struct gen_context *ctx, const struct expression *expr)
{
	// TODO: Run defers
	struct gen_value ret = gen_expr(ctx, expr->_return.value);
	struct qbe_value qret = mkqval(ctx, &ret);
	pushi(ctx->current, NULL, Q_RET, &qret, NULL);
	return gv_void;
}

static void
gen_expr_struct_at(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value out)
{
	// TODO: Merge me into constant expressions
	struct qbe_value base = mkqval(ctx, &out);

	if (expr->_struct.autofill) {
		struct qbe_value rtfunc = mkrtfunc(ctx, "rt.memset");
		struct qbe_value size =
			constl(expr->result->size), zero = constl(0);
		pushi(ctx->current, NULL, Q_CALL, &rtfunc,
			&base, &zero, &size, NULL);
	}

	struct gen_value ftemp = mktemp(ctx, &builtin_type_void, "field.%d");
	for (const struct expr_struct_field *field = &expr->_struct.fields;
			field; field = field->next) {
		if (!field->value) {
			assert(expr->_struct.autofill);
			field = field->next;
			continue;
		}

		struct qbe_value offs = constl(field->field->offset);
		ftemp.type = field->value->result;
		struct qbe_value ptr = mklval(ctx, &ftemp);
		pushi(ctx->current, &ptr, Q_ADD, &base, &offs, NULL);
		gen_expr_at(ctx, field->value, ftemp);
	}
}

static void
gen_expr_tuple_at(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value out)
{
	// TODO: Merge me into constant expressions
	struct qbe_value base = mkqval(ctx, &out);

	const struct type *type = type_dealias(expr->result);
	struct gen_value vtemp = mktemp(ctx, &builtin_type_void, "value.%d");
	const struct expression_tuple *value = &expr->tuple;
	for (const struct type_tuple *tuple = &type->tuple;
			tuple; tuple = tuple->next) {
		struct qbe_value offs = constl(tuple->offset);
		vtemp.type = value->value->result;
		struct qbe_value ptr = mklval(ctx, &vtemp);
		pushi(ctx->current, &ptr, Q_ADD, &base, &offs, NULL);
		gen_expr_at(ctx, value->value, vtemp);
		value = value->next;
	}
}

static struct gen_value
gen_expr_unarithm(struct gen_context *ctx,
	const struct expression *expr)
{
	struct gen_value val;
	const struct expression *operand = expr->unarithm.operand;
	switch (expr->unarithm.op) {
	case UN_ADDRESS:
		assert(operand->type == EXPR_ACCESS);
		val = gen_expr_access_addr(ctx, operand);
		val.type = expr->result;
		return val;
	case UN_DEREF:
		val = gen_expr(ctx, operand);
		assert(type_dealias(val.type)->storage == STORAGE_POINTER);
		val.type = type_dealias(val.type)->pointer.referent;
		return gen_load(ctx, val);
	case UN_BNOT:
	case UN_LNOT:
	case UN_MINUS:
	case UN_PLUS:
		assert(0); // TODO
	}
	abort(); // Invariant
}

static struct gen_value
gen_expr(struct gen_context *ctx, const struct expression *expr)
{
	switch (expr->type) {
	case EXPR_ACCESS:
		return gen_expr_access(ctx, expr);
	case EXPR_ALLOC:
	case EXPR_APPEND:
		assert(0); // TODO
	case EXPR_ASSERT:
		return gen_expr_assert(ctx, expr);
	case EXPR_ASSIGN:
		return gen_expr_assign(ctx, expr);
	case EXPR_BINARITHM:
		return gen_expr_binarithm(ctx, expr);
	case EXPR_BINDING:
		return gen_expr_binding(ctx, expr);
	case EXPR_BREAK:
		assert(0); // TODO
	case EXPR_CALL:
		return gen_expr_call(ctx, expr);
	case EXPR_CAST:
		assert(0); // TODO
	case EXPR_CONSTANT:
		return gen_expr_const(ctx, expr);
	case EXPR_CONTINUE:
	case EXPR_DEFER:
	case EXPR_DELETE:
	case EXPR_FOR:
	case EXPR_FREE:
	case EXPR_IF:
	case EXPR_INSERT:
		assert(0); // TODO
	case EXPR_LIST:
		return gen_expr_list_with(ctx, expr, NULL);
	case EXPR_MATCH:
	case EXPR_MEASURE:
		assert(0); // TODO
	case EXPR_PROPAGATE:
		assert(0); // Lowered in check (for now?)
	case EXPR_RETURN:
		return gen_expr_return(ctx, expr);
	case EXPR_SLICE:
	case EXPR_SWITCH:
		assert(0); // TODO
	case EXPR_UNARITHM:
		return gen_expr_unarithm(ctx, expr);
	case EXPR_STRUCT:
	case EXPR_TUPLE:
		break; // Prefers -at style
	}

	struct gen_value out = mktemp(ctx, expr->result, "object.%d");
	struct qbe_value base = mkqval(ctx, &out);
	struct qbe_value sz = constl(expr->result->size);
	enum qbe_instr alloc = alloc_for_align(expr->result->align);
	pushprei(ctx->current, &base, alloc, &sz, NULL);
	gen_expr_at(ctx, expr, out);
	return out;
}

static void
gen_expr_at(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value out)
{
	assert(out.kind != GV_CONST);

	switch (expr->type) {
	case EXPR_CONSTANT:
		gen_expr_const_at(ctx, expr, out);
		return;
	case EXPR_LIST:
		gen_expr_list_with(ctx, expr, &out);
		return;
	case EXPR_STRUCT:
		gen_expr_struct_at(ctx, expr, out);
		return;
	case EXPR_TUPLE:
		gen_expr_tuple_at(ctx, expr, out);
		return;
	default:
		break; // Prefers non-at style
	}

	gen_store(ctx, out, gen_expr(ctx, expr));
}

static struct gen_value
gen_expr_with(struct gen_context *ctx,
	const struct expression *expr,
	struct gen_value *out)
{
	if (out) {
		gen_expr_at(ctx, expr, *out);
		return *out;
	}
	return gen_expr(ctx, expr);
}

static void
gen_function_decl(struct gen_context *ctx, const struct declaration *decl)
{
	const struct function_decl *func = &decl->func;
	const struct type *fntype = func->type;
	if (func->body == NULL) {
		return; // Prototype
	}
	// TODO: Attributes
	assert(!func->flags);

	struct qbe_def *qdef = xcalloc(1, sizeof(struct qbe_def));
	qdef->kind = Q_FUNC;
	qdef->exported = decl->exported;
	qdef->name = decl->symbol ? strdup(decl->symbol)
		: ident_to_sym(&decl->ident);
	ctx->current = &qdef->func;

	struct qbe_statement start_label = {0};
	genl(&start_label, &ctx->id, "start.%d");
	push(&qdef->func.prelude, &start_label);

	if (type_dealias(fntype->func.result)->storage != STORAGE_VOID) {
		qdef->func.returns = qtype_lookup(
			ctx, fntype->func.result, false);
	} else {
		qdef->func.returns = &qbe_void;
	}

	struct qbe_func_param *param, **next = &qdef->func.params;
	for (struct scope_object *obj = decl->func.scope->objects;
			obj; obj = obj->lnext) {
		const struct type *type = obj->type;
		param = *next = xcalloc(1, sizeof(struct qbe_func_param));
		assert(!obj->ident.ns); // Invariant
		param->name = strdup(obj->ident.name);
		param->type = qtype_lookup(ctx, type, false);

		struct gen_binding *gb =
			xcalloc(1, sizeof(struct gen_binding));
		gb->value.kind = GV_TEMP;
		gb->value.type = type;
		gb->object = obj;
		if (type_is_aggregate(type)) {
			// No need to copy to stack
			gb->value.name = strdup(param->name);
		} else {
			gb->value.name = gen_name(ctx, "param.%d");

			struct qbe_value qv = mklval(ctx, &gb->value);
			struct qbe_value sz = constl(type->size);
			enum qbe_instr alloc = alloc_for_align(type->align);
			pushprei(ctx->current, &qv, alloc, &sz, NULL);
			struct gen_value src = {
				.kind = GV_TEMP,
				.type = type,
				.name = param->name,
			};
			gen_store(ctx, gb->value, src);
		}

		gb->next = ctx->bindings;
		ctx->bindings = gb;
		next = &param->next;
	}

	pushl(&qdef->func, &ctx->id, "body.%d");
	struct gen_value ret = gen_expr(ctx, decl->func.body);

	if (decl->func.body->terminates) {
		// XXX: This is a bit hacky, to appease qbe
		size_t ln = ctx->current->body.ln;
		struct qbe_statement *last = &ctx->current->body.stmts[ln - 1];
		if (last->type != Q_INSTR || last->instr != Q_RET) {
			pushi(ctx->current, NULL, Q_RET, NULL);
		}
	} else if (type_dealias(fntype->func.result)->storage != STORAGE_VOID) {
		struct qbe_value qret = mkqval(ctx, &ret);
		pushi(ctx->current, NULL, Q_RET, &qret, NULL);
	} else {
		pushi(ctx->current, NULL, Q_RET, NULL);
	}

	qbe_append_def(ctx->out, qdef);
	ctx->current = NULL;
}

static void
gen_decl(struct gen_context *ctx, const struct declaration *decl)
{
	switch (decl->type) {
	case DECL_FUNC:
		gen_function_decl(ctx, decl);
		break;
	case DECL_GLOBAL:
		assert(0); // TODO
	case DECL_CONST:
	case DECL_TYPE:
		break; // Nothing to do
	}
}

void
gen(const struct unit *unit, struct type_store *store, struct qbe_program *out)
{
	struct gen_context ctx = {
		.out = out,
		.store = store,
		.ns = unit->ns,
		.arch = {
			.ptr = &qbe_long,
			.sz = &qbe_long,
		},
	};
	ctx.out->next = &ctx.out->defs;
	const struct declarations *decls = unit->declarations;
	while (decls) {
		gen_decl(&ctx, decls->decl);
		decls = decls->next;
	}
}

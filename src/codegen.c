/**
 * See Copyright Notice in picrin.h
 */

#include <stdio.h>
#include <assert.h>

#include "picrin.h"
#include "picrin/pair.h"
#include "picrin/irep.h"
#include "picrin/proc.h"
#include "picrin/lib.h"
#include "picrin/macro.h"
#include "xhash/xhash.h"

#if PIC_NONE_IS_FALSE
# define OP_PUSHNONE OP_PUSHFALSE
#else
# error enable PIC_NONE_IS_FALSE
#endif

#define FALLTHROUGH ((void)0)

static struct pic_irep *
new_irep(pic_state *pic)
{
  struct pic_irep *irep;

  irep = (struct pic_irep *)pic_obj_alloc(pic, sizeof(struct pic_irep), PIC_TT_IREP);
  irep->code = NULL;
  irep->clen = 0;
  irep->argc = -1;
  irep->localc = -1;
  irep->varg = false;
  irep->irep = NULL;
  irep->pool = NULL;
  return irep;
}

static bool
valid_formal(pic_state *pic, pic_value formal)
{
  if (pic_symbol_p(formal))
    return true;

  while (pic_pair_p(formal)) {
    if (! pic_symbol_p(pic_car(pic, formal))) {
      return false;
    }
    formal = pic_cdr(pic, formal);
  }
  if (pic_nil_p(formal))
    return true;
  if (pic_symbol_p(formal))
    return true;

  return false;
}

typedef struct analyze_scope {
  bool varg;
  /* rest args variable is counted by localc */
  size_t argc, localc;
  /* local variables are 1-indexed, 0 is reserved for the callee */
  struct xhash *local_tbl;
  /* if local var i is captured, then dirty_flags[i] == 1 */
  int *dirty_flags;

  struct analyze_scope *up;
} analyze_scope;

static analyze_scope *
global_scope(pic_state *pic)
{
  analyze_scope *scope;

  scope = (analyze_scope *)pic_alloc(pic, sizeof(analyze_scope));
  scope->up = NULL;
  scope->local_tbl = pic->global_tbl;
  scope->argc = -1;
  scope->localc = -1;
  scope->varg = false;
  scope->dirty_flags = NULL;

  return scope;
}

typedef struct analyze_state {
  pic_state *pic;
  analyze_scope *scope;
  pic_sym rCONS, rCAR, rCDR, rNILP;
  pic_sym rADD, rSUB, rMUL, rDIV;
  pic_sym rEQ, rLT, rLE, rGT, rGE;
  pic_sym sCALL, sTAILCALL;
  pic_sym sGREF, sLREF, sCREF;
  pic_sym sGSET, sLSET, sCSET;
} analyze_state;

#define register_symbol(pic, state, slot, name) do {    \
    state->slot = pic_intern_cstr(pic, name);           \
  } while (0)

#define register_renamed_symbol(pic, state, slot, lib, name) do {       \
    struct xh_entry *e;                                                 \
    if (! (e = xh_get(lib->senv->tbl, name)))                           \
      pic_error(pic, "internal error! native VM procedure not found");  \
    state->slot = e->val;                                               \
  } while (0)

static analyze_state *
new_analyze_state(pic_state *pic)
{
  analyze_state *state;
  struct pic_lib *stdlib;

  state = (analyze_state *)pic_alloc(pic, sizeof(analyze_state));
  state->pic = pic;
  state->scope = global_scope(pic);

  stdlib = pic_find_library(pic, pic_parse(pic, "(scheme base)"));

  /* native VM procedures */
  register_renamed_symbol(pic, state, rCONS, stdlib, "cons");
  register_renamed_symbol(pic, state, rCAR, stdlib, "car");
  register_renamed_symbol(pic, state, rCDR, stdlib, "cdr");
  register_renamed_symbol(pic, state, rNILP, stdlib, "null?");
  register_renamed_symbol(pic, state, rADD, stdlib, "+");
  register_renamed_symbol(pic, state, rSUB, stdlib, "-");
  register_renamed_symbol(pic, state, rMUL, stdlib, "*");
  register_renamed_symbol(pic, state, rDIV, stdlib, "/");
  register_renamed_symbol(pic, state, rEQ, stdlib, "=");
  register_renamed_symbol(pic, state, rLT, stdlib, "<");
  register_renamed_symbol(pic, state, rLE, stdlib, "<=");
  register_renamed_symbol(pic, state, rGT, stdlib, ">");
  register_renamed_symbol(pic, state, rGE, stdlib, ">=");

  register_symbol(pic, state, sCALL, "call");
  register_symbol(pic, state, sTAILCALL, "tail-call");
  register_symbol(pic, state, sGREF, "gref");
  register_symbol(pic, state, sLREF, "lref");
  register_symbol(pic, state, sCREF, "cref");
  register_symbol(pic, state, sGREF, "gref");
  register_symbol(pic, state, sLREF, "lref");
  register_symbol(pic, state, sCREF, "cref");

  return state;
}

static void
push_scope(analyze_state *state, pic_value args)
{
  pic_state *pic = state->pic;
  analyze_scope *scope;
  struct xhash *x;
  pic_value v;
  int i, l;

  scope = (analyze_scope *)pic_alloc(pic, sizeof(analyze_scope));
  scope->up = state->scope;
  scope->local_tbl = x = xh_new();
  scope->varg = false;

  i = 1;
  l = 0;
  for (v = args; pic_pair_p(v); v = pic_cdr(pic, v)) {
    pic_value sym;

    sym = pic_car(pic, v);
    xh_put(x, pic_symbol_name(pic, pic_sym(sym)), i++);
  }
  if (pic_nil_p(v)) {
    /* pass */
  }
  else if (pic_symbol_p(v)) {
    scope->varg = true;
    xh_put(x, pic_symbol_name(pic, pic_sym(v)), i + l++);
  }
  else {
    pic_error(pic, "logic flaw");
  }
  scope->argc = i;
  scope->localc = l;
  scope->dirty_flags = (int *)pic_calloc(pic, i + l, sizeof(int));

  state->scope = scope;
}

static void
pop_scope(analyze_state *state)
{
  analyze_scope *scope;

  scope = state->scope;
  xh_destroy(scope->local_tbl);
  pic_free(state->pic, scope->dirty_flags);

  scope = scope->up;
  pic_free(state->pic, state->scope);
  state->scope = scope;
}

static analyze_scope *
lookup_var(analyze_state *state, const char *key, int *depth, int *idx)
{
  analyze_scope *scope = state->scope;
  struct xh_entry *e;
  int d = 0;

 enter:

  e = xh_get(scope->local_tbl, key);
  if (e && e->val >= 0) {
    if (scope->up == NULL) {	/* global */
      *depth = -1;
    }
    else {			/* non-global */
      *depth = d;
    }
    *idx = e->val;
    return scope;
  }
  if (scope->up) {
    scope = scope->up;
    ++d;
    goto enter;
  }
  return NULL;
}

static int
define_global_var(pic_state *pic, const char *name)
{
  struct xh_entry *e;

  if ((e = xh_get(pic->global_tbl, name))) {
    pic_warn(pic, "redefining global");
    return e->val;
  }
  e = xh_put(pic->global_tbl, name, pic->glen++);
  if (pic->glen >= pic->gcapa) {
    pic_error(pic, "global table overflow");
  }
  return e->val;
}

static int
define_local_var(pic_state *pic, const char *name, analyze_scope *scope)
{
  struct xh_entry *e;

  e = xh_put(scope->local_tbl, name, scope->argc + scope->localc++);
  scope->dirty_flags = (int *)pic_realloc(pic, scope->dirty_flags, (scope->argc + scope->localc) * sizeof(int));
  scope->dirty_flags[e->val] = 0;
  return e->val;
}

static bool
is_global_scope(analyze_scope *scope)
{
  return scope->up == NULL;
}

static pic_value
new_gref(analyze_state *state, int idx)
{
  return pic_list(state->pic, 2, pic_symbol_value(state->sGREF), pic_int_value(idx));
}

static pic_value
new_gset(analyze_state *state, int idx, pic_value value)
{
  return pic_list(state->pic, 3, pic_symbol_value(state->sGSET), pic_int_value(idx), value);
}

static pic_value
new_cref(analyze_state *state, int depth, int idx)
{
  return pic_list(state->pic, 3, pic_symbol_value(state->sCREF), pic_int_value(depth), pic_int_value(idx));
}

static pic_value
new_cset(analyze_state *state, int depth, int idx, pic_value value)
{
  return pic_list(state->pic, 4, pic_symbol_value(state->sCSET), pic_int_value(depth), pic_int_value(idx), value);
}

static pic_value analyze_node(analyze_state *, pic_value, bool);
static pic_value analyze_call(analyze_state *, pic_value, bool);
static pic_value analyze_lambda(analyze_state *, pic_value);

static pic_value
analyze(analyze_state *state, pic_value obj, bool tailpos)
{
  int ai = pic_gc_arena_preserve(state->pic);
  pic_value res;

  res = analyze_node(state, obj, tailpos);

  pic_gc_arena_restore(state->pic, ai);
  pic_gc_protect(state->pic, res);
  return res;
}

static pic_value
analyze_node(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  analyze_scope *scope = state->scope;

  switch (pic_type(obj)) {
  case PIC_TT_SYMBOL: {
    analyze_scope *s;
    int depth = -1, idx = -1;
    const char *name = pic_symbol_name(pic, pic_sym(obj));

    s = lookup_var(state, name, &depth, &idx);
    if (! s) {
#if DEBUG
      printf("%s\n", name);
#endif
      pic_error(pic, "symbol: unbound variable");
    }

    switch (depth) {
    case -1:			/* global */
      return new_gref(state, idx);
    default:			/* nonlocal */
      s->dirty_flags[idx] = 1;
      /* at this stage, lref and cref are not distinguished */
      FALLTHROUGH;
    case 0:			/* local */
      return new_cref(state, depth, idx);
    }
  }
  case PIC_TT_PAIR: {
    pic_value proc;

    if (! pic_list_p(pic, obj)) {
      pic_error(pic, "invalid expression given");
    }

    proc = pic_car(pic, obj);
    if (pic_symbol_p(proc)) {
      pic_sym sym = pic_sym(proc);

      if (sym == pic->sDEFINE) {
	int idx;
	pic_value var, val;

	if (pic_length(pic, obj) < 2) {
	  pic_error(pic, "syntax error");
	}

	var = pic_car(pic, pic_cdr(pic, obj));
	if (pic_pair_p(var)) {
	  val = pic_cons(pic, pic_symbol_value(pic->sLAMBDA),
			 pic_cons(pic, pic_cdr(pic, var),
				  pic_cdr(pic, pic_cdr(pic, obj))));
	  var = pic_car(pic, var);
	}
	else {
	  if (pic_length(pic, obj) != 3) {
	    pic_error(pic, "syntax error");
	  }
	  val = pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj)));
	}
	if (! pic_symbol_p(var)) {
	  pic_error(pic, "syntax error");
	}

	if (is_global_scope(scope)) {
	  idx = define_global_var(pic, pic_symbol_name(pic, pic_sym(var)));
          return new_gset(state, idx, analyze(state, val, false));
	}
	else {
	  idx = define_local_var(pic, pic_symbol_name(pic, pic_sym(var)), scope);
          return new_cset(state, 0, idx, analyze(state, val, false));
	}
      }
      else if (sym == pic->sLAMBDA) {
        return analyze_lambda(state, obj);
      }
      else if (sym == pic->sIF) {
	pic_value if_true, if_false;

	if_false = pic_none_value();
	switch (pic_length(pic, obj)) {
	default:
	  pic_error(pic, "syntax error");
	  break;
	case 4:
	  if_false = pic_car(pic, pic_cdr(pic, pic_cdr(pic, pic_cdr(pic, obj))));
	  FALLTHROUGH;
	case 3:
	  if_true = pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj)));
	}

        return pic_list(pic, 4,
                        pic_symbol_value(pic->sIF),
                        analyze(state, pic_car(pic, pic_cdr(pic, obj)), false),
                        analyze(state, if_true, tailpos),
                        analyze(state, if_false, tailpos));
      }
      else if (sym == pic->sBEGIN) {
	pic_value seq;
        bool tail;

        /* TODO: unwrap if the number of objects is 1 or 0 */
        seq = pic_list(pic, 1, pic_symbol_value(pic->sBEGIN));
        for (obj = pic_cdr(pic, obj); ! pic_nil_p(obj); obj = pic_cdr(pic, obj)) {
          if (pic_nil_p(pic_cdr(pic, obj))) {
            tail = tailpos;
          } else {
            tail = false;
          }
          seq = pic_cons(pic, analyze(state, pic_car(pic, obj), tail), seq);
        }
        return pic_reverse(pic, seq);
      }
      else if (sym == pic->sSETBANG) {
	analyze_scope *s;
	pic_value var, val;
	int depth = -1, idx = -1;

	if (pic_length(pic, obj) != 3) {
	  pic_error(pic, "syntax error");
	}

	var = pic_car(pic, pic_cdr(pic, obj));
	if (! pic_symbol_p(var)) {
	  pic_error(pic, "syntax error");
	}

	s = lookup_var(state, pic_symbol_name(pic, pic_sym(var)), &depth, &idx);
	if (! s) {
	  pic_error(pic, "unbound variable");
	}

        val = pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj)));

	switch (depth) {
	case -1:			/* global */
          return new_gset(state, idx, val);
	default:			/* nonlocal */
	  s->dirty_flags[idx] = 1;
	  /* at this stage, lset and cset are not distinguished */
	  FALLTHROUGH;
	case 0:			/* local */
          return new_cset(state, depth, idx, val);
	}
      }
      else if (sym == pic->sQUOTE) {
	if (pic_length(pic, obj) != 2) {
	  pic_error(pic, "syntax error");
	}
        return obj;             /* TODO: quote only if necessary */
      }

#define ARGC_ASSERT(n) do {				\
	if (pic_length(pic, obj) != (n) + 1) {		\
	  pic_error(pic, "wrong number of arguments");	\
	}						\
      } while (0)

#define CONSTRUCT_OP1(op)                                               \
      pic_list(pic, 2,                                                  \
               pic_symbol_value(op),                                    \
               analyze(state, pic_car(pic, pic_cdr(pic, obj)), false))

#define CONSTRUCT_OP2(op)                                               \
      pic_list(pic, 3,                                                  \
               pic_symbol_value(op),                                    \
               analyze(state, pic_car(pic, pic_cdr(pic, obj)), false),  \
               analyze(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false))

      else if (sym == state->rCONS) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sCONS);
      }
      else if (sym == state->rCAR) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sCAR);
      }
      else if (sym == state->rCDR) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sCDR);
      }
      else if (sym == state->rNILP) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sNILP);
      }

#define ARGC_ASSERT_GE(n) do {				\
	if (pic_length(pic, obj) < (n) + 1) {		\
	  pic_error(pic, "wrong number of arguments");	\
	}						\
      } while (0)

#define FOLD_ARGS(sym) do {                                             \
        obj = analyze(state, pic_car(pic, args), false);                \
        for (args = pic_cdr(pic, obj); ! pic_nil_p(args); args = pic_cdr(pic, args)) { \
          obj = pic_list(pic, 3, pic_symbol_value(sym), obj,            \
                         analyze(state, pic_car(pic, args), false));    \
        }                                                               \
      } while (0)

      else if (sym == state->rADD) {
	pic_value args;

	ARGC_ASSERT_GE(0);
	switch (pic_length(pic, obj)) {
	case 1:
          return pic_int_value(0);
	case 2:
	  return analyze(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
	default:
	  args = pic_cdr(pic, obj);
          FOLD_ARGS(pic->sADD);
          return obj;
	}
      }
      else if (sym == state->rSUB) {
	pic_value args;

	ARGC_ASSERT_GE(1);
	switch (pic_length(pic, obj)) {
	case 2:
          return pic_list(pic, 2, pic_symbol_value(pic->sMINUS),
                          analyze(state, pic_car(pic, pic_cdr(pic, obj)), false));
	default:
	  args = pic_cdr(pic, obj);
          FOLD_ARGS(pic->sSUB);
          return obj;
	}
      }
      else if (sym == state->rMUL) {
	pic_value args;

	ARGC_ASSERT_GE(0);
	switch (pic_length(pic, obj)) {
	case 1:
          return pic_int_value(1);
	case 2:
	  return analyze(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
	default:
	  args = pic_cdr(pic, obj);
          FOLD_ARGS(pic->sMUL);
          return obj;
	}
      }
      else if (sym == state->rDIV) {
	pic_value args;

	ARGC_ASSERT_GE(1);
	switch (pic_length(pic, obj)) {
	case 2:
          args = pic_cdr(pic, obj);
          obj = pic_list(pic, 3, proc, pic_float_value(1), pic_car(pic, args));
          return analyze(state, obj, tailpos);
	default:
	  args = pic_cdr(pic, obj);
          FOLD_ARGS(pic->sDIV);
          return obj;
	}
	break;
      }
      else if (sym == state->rEQ) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sEQ);
      }
      else if (sym == state->rLT) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sLT);
      }
      else if (sym == state->rLE) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sLE);
      }
      else if (sym == state->rGT) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sGT);
      }
      else if (sym == state->rGE) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sGE);
      }
    }
    return analyze_call(state, obj, tailpos);
  }
  case PIC_TT_BOOL:
  case PIC_TT_FLOAT:
  case PIC_TT_INT:
  case PIC_TT_NIL:
  case PIC_TT_CHAR: {
    return obj;
  }
  case PIC_TT_STRING:
  case PIC_TT_VECTOR:
  case PIC_TT_BLOB: {
    return pic_list(pic, 2, pic_symbol_value(pic->sQUOTE), obj);
  }
  case PIC_TT_CONT:
  case PIC_TT_ENV:
  case PIC_TT_PROC:
  case PIC_TT_UNDEF:
  case PIC_TT_EOF:
  case PIC_TT_PORT:
  case PIC_TT_ERROR:
  case PIC_TT_SENV:
  case PIC_TT_SYNTAX:
  case PIC_TT_SC:
  case PIC_TT_LIB:
  case PIC_TT_VAR:
  case PIC_TT_IREP:
    pic_error(pic, "invalid expression given");
  }
}

static pic_value
analyze_call(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  int ai = pic_gc_arena_preserve(pic);
  pic_value seq;
  pic_sym call;

  if (tailpos) {
    call = state->sCALL;
  } else {
    call = state->sTAILCALL;
  }
  seq = pic_list(pic, 1, pic_symbol_value(call));
  for (; ! pic_nil_p(obj); obj = pic_cdr(pic, obj)) {
    seq = pic_cons(pic, analyze(state, pic_car(pic, obj), false), seq);
  }
  seq = pic_reverse(pic, seq);

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, seq);
  return seq;
}

static pic_value
analyze_lambda(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  int ai = pic_gc_arena_preserve(pic);
  pic_value args, body;

  if (pic_length(pic, obj) < 2) {
    pic_error(pic, "syntax error");
  }

  /* formal arguments */
  args = pic_car(pic, pic_cdr(pic, obj));
  if (! valid_formal(pic, args)) {
    pic_error(pic, "syntax error");
  }

  /* analyze body in inner environment */
  push_scope(state, args);
  {
    body = pic_cdr(pic, pic_cdr(pic, obj));
    body = pic_cons(pic, pic_symbol_value(pic->sBEGIN), body);
    body = analyze(state, body, true);
  }
  pop_scope(state);

  obj = pic_list(pic, 3, pic_symbol_value(pic->sLAMBDA), args, body);
  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, obj);
  return obj;
}

/**
 * scope object
 */

typedef struct codegen_scope {
  bool varg;
  /* local variables are 1-indexed, 0 is reserved for the callee */
  struct xhash *local_tbl;
  /* rest args variable is counted by localc */
  size_t argc, localc;
  /* if local var i is captured, then dirty_flags[i] == 1 */
  int *dirty_flags;
  /* actual bit code sequence */
  struct pic_code *code;
  size_t clen, ccapa;
  /* child ireps */
  struct pic_irep **irep;
  size_t ilen, icapa;
  /* constant object pool */
  pic_value *pool;
  size_t plen, pcapa;

  struct codegen_scope *up;
} codegen_scope;

static codegen_scope *
new_global_scope(pic_state *pic)
{
  codegen_scope *scope;

  scope = (codegen_scope *)pic_alloc(pic, sizeof(codegen_scope));
  scope->up = NULL;
  scope->local_tbl = pic->global_tbl;
  scope->argc = -1;
  scope->localc = -1;
  scope->dirty_flags = NULL;
  scope->varg = false;
  scope->code = (struct pic_code *)pic_calloc(pic, PIC_ISEQ_SIZE, sizeof(struct pic_code));
  scope->clen = 0;
  scope->ccapa = PIC_ISEQ_SIZE;
  scope->irep = (struct pic_irep **)pic_calloc(pic, PIC_IREP_SIZE, sizeof(struct pic_irep *));
  scope->ilen = 0;
  scope->icapa = PIC_IREP_SIZE;
  scope->pool = (pic_value *)pic_calloc(pic, PIC_POOL_SIZE, sizeof(pic_value));
  scope->plen = 0;
  scope->pcapa = PIC_POOL_SIZE;

  return scope;
}

static codegen_scope *
new_local_scope(pic_state *pic, pic_value args, codegen_scope *scope)
{
  codegen_scope *new_scope;
  pic_value v;
  int i, l;
  struct xhash *x;

  new_scope = (codegen_scope *)pic_alloc(pic, sizeof(codegen_scope));
  new_scope->up = scope;
  new_scope->local_tbl = x = xh_new();
  new_scope->varg = false;

  i = 1; l = 0;
  for (v = args; pic_pair_p(v); v = pic_cdr(pic, v)) {
    pic_value sym;

    sym = pic_car(pic, v);
    xh_put(x, pic_symbol_name(pic, pic_sym(sym)), i++);
  }
  if (pic_nil_p(v)) {
    /* pass */
  }
  else if (pic_symbol_p(v)) {
    new_scope->varg = true;
    xh_put(x, pic_symbol_name(pic, pic_sym(v)), i + l++);
  }
  else {
    pic_error(pic, "logic flaw");
  }
  new_scope->argc = i;
  new_scope->localc = l;
  new_scope->dirty_flags = (int *)pic_calloc(pic, i + l, sizeof(int));

  new_scope->code = (struct pic_code *)pic_calloc(pic, PIC_ISEQ_SIZE, sizeof(struct pic_code));
  new_scope->clen = 0;
  new_scope->ccapa = PIC_ISEQ_SIZE;

  new_scope->irep = (struct pic_irep **)pic_calloc(pic, PIC_IREP_SIZE, sizeof(struct pic_irep *));
  new_scope->ilen = 0;
  new_scope->icapa = PIC_IREP_SIZE;

  new_scope->pool = (pic_value *)pic_calloc(pic, PIC_POOL_SIZE, sizeof(pic_value));
  new_scope->plen = 0;
  new_scope->pcapa = PIC_POOL_SIZE;

  return new_scope;
}

static void
destroy_scope(pic_state *pic, codegen_scope *scope)
{
  if (scope->up) {
    xh_destory(scope->local_tbl);
    pic_free(pic, scope->dirty_flags);
  }
  pic_free(pic, scope);
}

/**
 * global codegen state
 */

typedef struct codegen_state {
  pic_state *pic;
  codegen_scope *scope;
  pic_sym rCONS, rCAR, rCDR, rNILP;
  pic_sym rADD, rSUB, rMUL, rDIV;
  pic_sym rEQ, rLT, rLE, rGT, rGE;
} codegen_state;

#define register_renamed_symbol(pic, state, slot, lib, name) do {       \
    struct xh_entry *e;                                                 \
    if (! (e = xh_get(lib->senv->tbl, name)))                           \
      pic_error(pic, "internal error! native VM procedure not found");  \
    state->slot = e->val;                                               \
  } while (0)

static codegen_state *
new_codegen_state(pic_state *pic)
{
  codegen_state *state;
  struct pic_lib *stdlib;

  state = (codegen_state *)pic_alloc(pic, sizeof(codegen_state));
  state->pic = pic;
  state->scope = new_global_scope(pic);

  stdlib = pic_find_library(pic, pic_parse(pic, "(scheme base)"));

  /* native VM procedures */
  register_renamed_symbol(pic, state, rCONS, stdlib, "cons");
  register_renamed_symbol(pic, state, rCAR, stdlib, "car");
  register_renamed_symbol(pic, state, rCDR, stdlib, "cdr");
  register_renamed_symbol(pic, state, rNILP, stdlib, "null?");
  register_renamed_symbol(pic, state, rADD, stdlib, "+");
  register_renamed_symbol(pic, state, rSUB, stdlib, "-");
  register_renamed_symbol(pic, state, rMUL, stdlib, "*");
  register_renamed_symbol(pic, state, rDIV, stdlib, "/");
  register_renamed_symbol(pic, state, rEQ, stdlib, "=");
  register_renamed_symbol(pic, state, rLT, stdlib, "<");
  register_renamed_symbol(pic, state, rLE, stdlib, "<=");
  register_renamed_symbol(pic, state, rGT, stdlib, ">");
  register_renamed_symbol(pic, state, rGE, stdlib, ">=");

  return state;
}

static void
destroy_codegen_state(pic_state *pic, codegen_state *state)
{
  destroy_scope(pic, state->scope);
  pic_free(pic, state);
}

static codegen_scope *
scope_lookup(codegen_state *state, const char *key, int *depth, int *idx)
{
  codegen_scope *scope = state->scope;
  struct xh_entry *e;
  int d = 0;

 enter:

  e = xh_get(scope->local_tbl, key);
  if (e && e->val >= 0) {
    if (scope->up == NULL) {	/* global */
      *depth = -1;
    }
    else {			/* non-global */
      *depth = d;
    }
    *idx = e->val;
    return scope;
  }
  if (scope->up) {
    scope = scope->up;
    ++d;
    goto enter;
  }
  return NULL;
}

static int
scope_global_define(pic_state *pic, const char *name)
{
  struct xh_entry *e;

  if ((e = xh_get(pic->global_tbl, name))) {
    pic_warn(pic, "redefining global");
    return e->val;
  }
  e = xh_put(pic->global_tbl, name, pic->glen++);
  if (pic->glen >= pic->gcapa) {
    pic_error(pic, "global table overflow");
  }
  return e->val;
}

static int
scope_local_define(pic_state *pic, const char *name, codegen_scope *scope)
{
  struct xh_entry *e;

  e = xh_put(scope->local_tbl, name, scope->argc + scope->localc++);
  scope->dirty_flags = (int *)pic_realloc(pic, scope->dirty_flags, (scope->argc + scope->localc) * sizeof(int));
  scope->dirty_flags[e->val] = 0;
  return e->val;
}

static bool
scope_is_global(codegen_scope *scope)
{
  return scope->up == NULL;
}

static void codegen_call(codegen_state *, pic_value, bool);
static struct pic_irep *codegen_lambda(codegen_state *, pic_value);

static void
codegen(codegen_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  codegen_scope *scope = state->scope;

  switch (pic_type(obj)) {
  case PIC_TT_SYMBOL: {
    codegen_scope *s;
    int depth = -1, idx = -1;
    const char *name;

    name = pic_symbol_name(pic, pic_sym(obj));
    s = scope_lookup(state, name, &depth, &idx);
    if (! s) {
#if DEBUG
      printf("%s\n", name);
#endif
      pic_error(pic, "symbol: unbound variable");
    }

    switch (depth) {
    case -1:			/* global */
      scope->code[scope->clen].insn = OP_GREF;
      scope->code[scope->clen].u.i = idx;
      scope->clen++;
      break;
    default:			/* nonlocal */
      s->dirty_flags[idx] = 1;
      /* at this stage, lref and cref are not distinguished */
      FALLTHROUGH;
    case 0:			/* local */
      scope->code[scope->clen].insn = OP_CREF;
      scope->code[scope->clen].u.r.depth = depth;
      scope->code[scope->clen].u.r.idx = idx;
      scope->clen++;
      break;
    }
    break;
  }
  case PIC_TT_PAIR: {
    pic_value proc;

    if (! pic_list_p(pic, obj)) {
      pic_error(pic, "invalid expression given");
    }

    proc = pic_car(pic, obj);
    if (pic_symbol_p(proc)) {
      pic_sym sym = pic_sym(proc);

      if (sym == pic->sDEFINE) {
	int idx;
	pic_value var, val;

	if (pic_length(pic, obj) < 2) {
	  pic_error(pic, "syntax error");
	}

	var = pic_car(pic, pic_cdr(pic, obj));
	if (pic_pair_p(var)) {
	  val = pic_cons(pic, pic_symbol_value(pic->sLAMBDA),
			 pic_cons(pic, pic_cdr(pic, var),
				  pic_cdr(pic, pic_cdr(pic, obj))));
	  var = pic_car(pic, var);
	}
	else {
	  if (pic_length(pic, obj) != 3) {
	    pic_error(pic, "syntax error");
	  }
	  val = pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj)));
	}
	if (! pic_symbol_p(var)) {
	  pic_error(pic, "syntax error");
	}

	if (scope_is_global(scope)) {
	  idx = scope_global_define(pic, pic_symbol_name(pic, pic_sym(var)));
	  codegen(state, val, false);
	  scope->code[scope->clen].insn = OP_GSET;
	  scope->code[scope->clen].u.i = idx;
	  scope->clen++;
	  scope->code[scope->clen].insn = OP_PUSHNONE;
	  scope->clen++;
	  break;
	}
	else {
	  idx = scope_local_define(pic, pic_symbol_name(pic, pic_sym(var)), scope);
	  codegen(state, val, false);
	  scope->code[scope->clen].insn = OP_CSET;
	  scope->code[scope->clen].u.r.depth = 0;
	  scope->code[scope->clen].u.r.idx = idx;
	  scope->clen++;
	  scope->code[scope->clen].insn = OP_PUSHNONE;
	  scope->clen++;
	  break;
	}
      }
      else if (sym == pic->sLAMBDA) {
	int k;

	if (scope->ilen >= scope->icapa) {
#if DEBUG
	  puts("irep realloced");
#endif
	  scope->icapa *= 2;
	  scope->irep = (struct pic_irep **)pic_realloc(pic, scope->irep, sizeof(struct pic_irep *) * scope->icapa);
	}
	k = scope->ilen++;
	scope->code[scope->clen].insn = OP_LAMBDA;
	scope->code[scope->clen].u.i = k;
	scope->clen++;

	scope->irep[k] = codegen_lambda(state, obj);
	break;
      }
      else if (sym == pic->sIF) {
	int s,t;
	pic_value if_true, if_false;

	if_false = pic_none_value();
	switch (pic_length(pic, obj)) {
	default:
	  pic_error(pic, "syntax error");
	  break;
	case 4:
	  if_false = pic_car(pic, pic_cdr(pic, pic_cdr(pic, pic_cdr(pic, obj))));
	  FALLTHROUGH;
	case 3:
	  if_true = pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj)));
	}

	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);

	scope->code[scope->clen].insn = OP_JMPIF;
	s = scope->clen++;

	/* if false branch */
	codegen(state, if_false, tailpos);
	scope->code[scope->clen].insn = OP_JMP;
	t = scope->clen++;

	scope->code[s].u.i = scope->clen - s;

	/* if true branch */
	codegen(state, if_true, tailpos);
	scope->code[t].u.i = scope->clen - t;
	break;
      }
      else if (sym == pic->sBEGIN) {
	int i, len;
	pic_value v, seq;

	seq = pic_cdr(pic, obj);
	len = pic_length(pic, seq);
	for (i = 0; i < len; ++i) {
	  v = pic_car(pic, seq);
	  if (i + 1 >= len) {
	    codegen(state, v, tailpos);
	  }
	  else {
	    codegen(state, v, false);
	    scope->code[scope->clen].insn = OP_POP;
	    scope->clen++;
	  }
	  seq = pic_cdr(pic, seq);
	}
	break;
      }
      else if (sym == pic->sSETBANG) {
	codegen_scope *s;
	pic_value var;
	int depth = -1, idx = -1;

	if (pic_length(pic, obj) != 3) {
	  pic_error(pic, "syntax error");
	}

	var = pic_car(pic, pic_cdr(pic, obj));
	if (! pic_symbol_p(var)) {
	  pic_error(pic, "syntax error");
	}

	s = scope_lookup(state, pic_symbol_name(pic, pic_sym(var)), &depth, &idx);
	if (! s) {
	  pic_error(pic, "unbound variable");
	}

	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);

	switch (depth) {
	case -1:			/* global */
	  scope->code[scope->clen].insn = OP_GSET;
	  scope->code[scope->clen].u.i = idx;
	  scope->clen++;
	  break;
	default:			/* nonlocal */
	  s->dirty_flags[idx] = 1;
	  /* at this stage, lset and cset are not distinguished */
	  FALLTHROUGH;
	case 0:			/* local */
	  scope->code[scope->clen].insn = OP_CSET;
	  scope->code[scope->clen].u.r.depth = depth;
	  scope->code[scope->clen].u.r.idx = idx;
	  scope->clen++;
	  break;
	}

	scope->code[scope->clen].insn = OP_PUSHNONE;
	scope->clen++;
	break;
      }
      else if (sym == pic->sQUOTE) {
	int pidx;

	if (pic_length(pic, obj) != 2) {
	  pic_error(pic, "syntax error");
	}

	if (scope->plen >= scope->pcapa) {
          scope->pcapa *= 2;
          scope->pool = (pic_value *)pic_realloc(pic, scope->pool, sizeof(pic_value) * scope->pcapa);
	}
	pidx = scope->plen++;
	scope->pool[pidx] = pic_car(pic, pic_cdr(pic, obj));
	scope->code[scope->clen].insn = OP_PUSHCONST;
	scope->code[scope->clen].u.i = pidx;
	scope->clen++;
	break;
      }

#define ARGC_ASSERT(n) do {				\
	if (pic_length(pic, obj) != (n) + 1) {		\
	  pic_error(pic, "wrong number of arguments");	\
	}						\
      } while (0)

      else if (sym == state->rCONS) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	scope->code[scope->clen].insn = OP_CONS;
	scope->clen++;
	break;
      }
      else if (sym == state->rCAR) {
	ARGC_ASSERT(1);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	scope->code[scope->clen].insn = OP_CAR;
	scope->clen++;
	break;
      }
      else if (sym == state->rCDR) {
	ARGC_ASSERT(1);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	scope->code[scope->clen].insn = OP_CDR;
	scope->clen++;
	break;
      }
      else if (sym == state->rNILP) {
	ARGC_ASSERT(1);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	scope->code[scope->clen].insn = OP_NILP;
	scope->clen++;
	break;
      }

#define ARGC_ASSERT_GE(n) do {				\
	if (pic_length(pic, obj) < (n) + 1) {		\
	  pic_error(pic, "wrong number of arguments");	\
	}						\
      } while (0)

      else if (sym == state->rADD) {
	pic_value args;

	ARGC_ASSERT_GE(0);
	switch (pic_length(pic, obj)) {
	case 1:
	  scope->code[scope->clen].insn = OP_PUSHINT;
	  scope->code[scope->clen].u.i = 0;
	  scope->clen++;
	  break;
	case 2:
	  codegen(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
	  break;
	default:
	  args = pic_cdr(pic, obj);
	  codegen(state, pic_car(pic, args), false);
	  while (pic_length(pic, args) >= 2) {
	    codegen(state, pic_car(pic, pic_cdr(pic, args)), false);
	    scope->code[scope->clen].insn = OP_ADD;
	    scope->clen++;
	    args = pic_cdr(pic, args);
	  }
	  break;
	}
	break;
      }
      else if (sym == state->rSUB) {
	pic_value args;

	ARGC_ASSERT_GE(1);
	switch (pic_length(pic, obj)) {
	case 2:
	  codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	  scope->code[scope->clen].insn = OP_MINUS;
	  scope->clen++;
	  break;
	default:
	  args = pic_cdr(pic, obj);
	  codegen(state, pic_car(pic, args), false);
	  while (pic_length(pic, args) >= 2) {
	    codegen(state, pic_car(pic, pic_cdr(pic, args)), false);
	    scope->code[scope->clen].insn = OP_SUB;
	    scope->clen++;
	    args = pic_cdr(pic, args);
	  }
	  break;
	}
	break;
      }
      else if (sym == state->rMUL) {
	pic_value args;

	ARGC_ASSERT_GE(0);
	switch (pic_length(pic, obj)) {
	case 1:
	  scope->code[scope->clen].insn = OP_PUSHINT;
	  scope->code[scope->clen].u.i = 1;
	  scope->clen++;
	  break;
	case 2:
	  codegen(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
	  break;
	default:
	  args = pic_cdr(pic, obj);
	  codegen(state, pic_car(pic, args), false);
	  while (pic_length(pic, args) >= 2) {
	    codegen(state, pic_car(pic, pic_cdr(pic, args)), false);
	    scope->code[scope->clen].insn = OP_MUL;
	    scope->clen++;
	    args = pic_cdr(pic, args);
	  }
	  break;
	}
	break;
      }
      else if (sym == state->rDIV) {
	pic_value args;

	ARGC_ASSERT_GE(1);
	switch (pic_length(pic, obj)) {
	case 2:
	  scope->code[scope->clen].insn = OP_PUSHINT;
	  scope->code[scope->clen].u.i = 1;
	  scope->clen++;
	  codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	  scope->code[scope->clen].insn = OP_DIV;
	  scope->clen++;
	  break;
	default:
	  args = pic_cdr(pic, obj);
	  codegen(state, pic_car(pic, args), false);
	  while (pic_length(pic, args) >= 2) {
	    codegen(state, pic_car(pic, pic_cdr(pic, args)), false);
	    scope->code[scope->clen].insn = OP_DIV;
	    scope->clen++;
	    args = pic_cdr(pic, args);
	  }
	  break;
	}
	break;
      }
      else if (sym == state->rEQ) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	scope->code[scope->clen].insn = OP_EQ;
	scope->clen++;
	break;
      }
      else if (sym == state->rLT) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	scope->code[scope->clen].insn = OP_LT;
	scope->clen++;
	break;
      }
      else if (sym == state->rLE) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	scope->code[scope->clen].insn = OP_LE;
	scope->clen++;
	break;
      }
      else if (sym == state->rGT) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	scope->code[scope->clen].insn = OP_LT;
	scope->clen++;
	break;
      }
      else if (sym == state->rGE) {
	ARGC_ASSERT(2);
	codegen(state, pic_car(pic, pic_cdr(pic, pic_cdr(pic, obj))), false);
	codegen(state, pic_car(pic, pic_cdr(pic, obj)), false);
	scope->code[scope->clen].insn = OP_LE;
	scope->clen++;
	break;
      }
    }

    codegen_call(state, obj, tailpos);
    break;
  }
  case PIC_TT_BOOL: {
    if (pic_true_p(obj)) {
      scope->code[scope->clen].insn = OP_PUSHTRUE;
    }
    else {
      scope->code[scope->clen].insn = OP_PUSHFALSE;
    }
    scope->clen++;
    break;
  }
  case PIC_TT_FLOAT: {
    scope->code[scope->clen].insn = OP_PUSHFLOAT;
    scope->code[scope->clen].u.f = pic_float(obj);
    scope->clen++;
    break;
  }
  case PIC_TT_INT: {
    scope->code[scope->clen].insn = OP_PUSHINT;
    scope->code[scope->clen].u.i = pic_int(obj);
    scope->clen++;
    break;
  }
  case PIC_TT_NIL: {
    scope->code[scope->clen].insn = OP_PUSHNIL;
    scope->clen++;
    break;
  }
  case PIC_TT_CHAR: {
    scope->code[scope->clen].insn = OP_PUSHCHAR;
    scope->code[scope->clen].u.c = pic_char(obj);
    scope->clen++;
    break;
  }
  case PIC_TT_STRING:
  case PIC_TT_VECTOR:
  case PIC_TT_BLOB: {
    int pidx;
    if (scope->plen >= scope->pcapa) {
      scope->pcapa *= 2;
      scope->pool = (pic_value *)pic_realloc(pic, scope->pool, sizeof(pic_value) * scope->pcapa);
    }
    pidx = scope->plen++;
    scope->pool[pidx] = obj;
    scope->code[scope->clen].insn = OP_PUSHCONST;
    scope->code[scope->clen].u.i = pidx;
    scope->clen++;
    break;
  }
  case PIC_TT_CONT:
  case PIC_TT_ENV:
  case PIC_TT_PROC:
  case PIC_TT_UNDEF:
  case PIC_TT_EOF:
  case PIC_TT_PORT:
  case PIC_TT_ERROR:
  case PIC_TT_SENV:
  case PIC_TT_SYNTAX:
  case PIC_TT_SC:
  case PIC_TT_LIB:
  case PIC_TT_VAR:
  case PIC_TT_IREP:
    pic_error(pic, "invalid expression given");
  }
}

static void
codegen_call(codegen_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  codegen_scope *scope = state->scope;
  pic_value seq;
  int i = 0;

  for (seq = obj; ! pic_nil_p(seq); seq = pic_cdr(pic, seq)) {
    pic_value v;

    v = pic_car(pic, seq);
    codegen(state, v, false);
    ++i;
  }
  scope->code[scope->clen].insn = tailpos ? OP_TAILCALL : OP_CALL;
  scope->code[scope->clen].u.i = i;
  scope->clen++;
}

static void
lift_cv(pic_state *pic, struct pic_irep *irep, int d)
{
  int i;
  struct pic_code c;

  for (i = 0; i < irep->clen; ++i) {
    c = irep->code[i];
    switch (c.insn) {
    default:
      /* pass */
      break;
    case OP_LAMBDA:
      if (irep->irep[c.u.i]->cv_num == 0)
	lift_cv(pic, irep->irep[c.u.i], d);
      else
	lift_cv(pic, irep->irep[c.u.i], d + 1);
      break;
    case OP_CREF:
    case OP_CSET:
      if (irep->code[i].u.r.depth > d)
	irep->code[i].u.r.depth--;
      break;
    }
  }
}

static void
slide_cv(pic_state *pic, unsigned *cv_tbl, unsigned cv_num, struct pic_irep *irep, int d)
{
  int i, j;
  struct pic_code c;

  for (i = 0; i < irep->clen; ++i) {
    c = irep->code[i];
    switch (c.insn) {
    default:
      /* pass */
      break;
    case OP_LAMBDA:
      if (irep->irep[c.u.i]->cv_num == 0) {
	slide_cv(pic, cv_tbl, cv_num, irep->irep[c.u.i], d);
      }
      else {
	slide_cv(pic, cv_tbl, cv_num, irep->irep[c.u.i], d + 1);
      }
      break;
    case OP_CREF:
    case OP_CSET:
      if (d != c.u.r.depth)
	break;
      for (j = 0; j < cv_num; ++j) {
	if (c.u.r.idx == cv_tbl[j]) {
	  irep->code[i].u.r.idx = j;
	  break;
	}
      }
      break;
    }
  }
}

static struct pic_irep *
codegen_lambda(codegen_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  struct pic_irep *irep;
  pic_value args, body, v;
  int i, c, k;

  if (pic_length(pic, obj) < 2) {
    pic_error(pic, "syntax error");
  }

  args = pic_car(pic, pic_cdr(pic, obj));
  if (! valid_formal(pic, args)) {
    pic_error(pic, "syntax error");
  }

  /* inner environment */
  state->scope = new_local_scope(pic, args, state->scope);
  {
    /* body */
    body = pic_cdr(pic, pic_cdr(pic, obj));
    for (v = body; ! pic_nil_p(v); v = pic_cdr(pic, v)) {
      if (pic_nil_p(pic_cdr(pic, v))) {
	codegen(state, pic_car(pic, v), true);
      }
      else {
	codegen(state, pic_car(pic, v), false);
	state->scope->code[state->scope->clen].insn = OP_POP;
	state->scope->clen++;
      }
    }
    state->scope->code[state->scope->clen].insn = OP_RET;
    state->scope->clen++;

    /* create irep */
    irep = new_irep(pic);
    irep->varg = state->scope->varg;
    irep->argc = state->scope->argc;
    irep->localc = state->scope->localc;
    irep->code = pic_realloc(pic, state->scope->code, sizeof(struct pic_code) * state->scope->clen);
    irep->clen = state->scope->clen;
    irep->irep = pic_realloc(pic, state->scope->irep, sizeof(struct pic_irep *) * state->scope->ilen);
    irep->ilen = state->scope->ilen;
    irep->pool = pic_realloc(pic, state->scope->pool, sizeof(pic_value) * state->scope->plen);
    irep->plen = state->scope->plen;

    /* fixup local references */
    for (i = 0; i < irep->clen; ++i) {
      struct pic_code c = irep->code[i];
      switch (c.insn) {
      default:
	/* pass */
	break;
      case OP_CREF:
	if (c.u.r.depth == 0 && ! state->scope->dirty_flags[c.u.r.idx]) {
	  irep->code[i].insn = OP_LREF;
	  irep->code[i].u.i = irep->code[i].u.r.idx;
	}
	break;
      case OP_CSET:
	if (c.u.r.depth == 0 && ! state->scope->dirty_flags[c.u.r.idx]) {
	  irep->code[i].insn = OP_LSET;
	  irep->code[i].u.i = irep->code[i].u.r.idx;
	}
	break;
      }
    }

    /* fixup closed variables */
    c = 0;
    for (i = 0; i < irep->argc + irep->localc; ++i) {
      if (state->scope->dirty_flags[i])
	++c;
    }
    if (c == 0) {
      lift_cv(pic, irep, 0);
      irep->cv_tbl = NULL;
      irep->cv_num = 0;
    }
    else {
      irep->cv_tbl = (unsigned *)pic_calloc(pic, c, sizeof(unsigned));
      k = 0;
      for (i = 0; i < irep->argc + irep->localc; ++i) {
	if (state->scope->dirty_flags[i]) {
	  irep->cv_tbl[k] = i;
	  ++k;
	}
      }
      irep->cv_num = c;
      slide_cv(pic, irep->cv_tbl, irep->cv_num, irep, 0);
    }
  }
  destroy_scope(pic, state->scope);

  state->scope = state->scope->up;

#if VM_DEBUG
  printf("* generated lambda:\n");
  pic_dump_irep(pic, irep);
  puts("");
#endif

  return irep;
}

struct pic_proc *
pic_codegen(pic_state *pic, pic_value obj)
{
  struct pic_proc *proc;
  codegen_state *state;
  struct pic_irep *irep;
  jmp_buf jmp, *prev_jmp = pic->jmp;
  int ai = pic_gc_arena_preserve(pic);

  state = new_codegen_state(pic);

  if (setjmp(jmp) == 0) {
    pic->jmp = &jmp;
  }
  else {
    /* error occured */
    proc = NULL;
    goto exit;
  }

  codegen(state, pic_macroexpand(pic, obj), false);
  state->scope->code[state->scope->clen].insn = OP_RET;
  state->scope->clen++;

  irep = new_irep(pic);
  irep->varg = false;
  irep->argc = 1;
  irep->localc = 0;
  irep->code = pic_realloc(pic, state->scope->code, sizeof(struct pic_code) * state->scope->clen);
  irep->clen = state->scope->clen;
  irep->irep = pic_realloc(pic, state->scope->irep, sizeof(struct pic_irep *) * state->scope->ilen);
  irep->ilen = state->scope->ilen;
  irep->pool = pic_realloc(pic, state->scope->pool, sizeof(pic_value) * state->scope->plen);
  irep->plen = state->scope->plen;
  irep->cv_num = 0;
  irep->cv_tbl = NULL;

  proc = pic_proc_new_irep(pic, irep, NULL);

  destroy_codegen_state(pic, state);

#if VM_DEBUG
  pic_dump_irep(pic, proc->u.irep);
#endif

 exit:
  pic->jmp = prev_jmp;

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, pic_obj_value(proc));

  return proc;
}

void
pic_define(pic_state *pic, const char *name, pic_value val)
{
  int idx;
  pic_sym gsym;

  gsym = pic_gensym(pic, pic_intern_cstr(pic, name));

  /* push to the global arena */
  idx = scope_global_define(pic, pic_symbol_name(pic, gsym));
  pic->globals[idx] = val;

  /* register to the senv */
  xh_put(pic->lib->senv->tbl, name, gsym);

  /* export! */
  pic_export(pic, pic_intern_cstr(pic, name));
}

static int
global_ref(pic_state *pic, const char *name)
{
  struct xh_entry *e;

  if (! (e = xh_get(pic->lib->senv->tbl, name))) {
    pic_error(pic, "symbol not defined");
  }
  assert(e->val >= 0);
  if (! (e = xh_get(pic->global_tbl, pic_symbol_name(pic, (pic_sym)e->val)))) {
    pic_abort(pic, "logic flaw");
  }
  return e->val;
}

pic_value
pic_ref(pic_state *pic, const char *name)
{
  int gid;

  gid = global_ref(pic, name);
  return pic->globals[gid];
}

void
pic_set(pic_state *pic, const char *name, pic_value value)
{
  int gid;

  gid = global_ref(pic, name);
  pic->globals[gid] = value;
}

void
print_code(pic_state *pic, struct pic_code c)
{
    printf("[%2d] ", c.insn);
    switch (c.insn) {
    case OP_POP:
      puts("OP_POP");
      break;
    case OP_PUSHNIL:
      puts("OP_PUSHNIL");
      break;
    case OP_PUSHTRUE:
      puts("OP_PUSHTRUE");
      break;
    case OP_PUSHFALSE:
      puts("OP_PUSHFALSE");
      break;
    case OP_PUSHFLOAT:
      printf("OP_PUSHFLOAT\t%f\n", c.u.f);
      break;
    case OP_PUSHINT:
      printf("OP_PUSHINT\t%d\n", c.u.i);
      break;
    case OP_PUSHCHAR:
      printf("OP_PUSHCHAR\t%c\n", c.u.c);
      break;
    case OP_PUSHCONST:
      printf("OP_PUSHCONST\t%d\n", c.u.i);
      break;
    case OP_GREF:
      printf("OP_GREF\t%i\n", c.u.i);
      break;
    case OP_GSET:
      printf("OP_GSET\t%i\n", c.u.i);
      break;
    case OP_LREF:
      printf("OP_LREF\t%d\n", c.u.i);
      break;
    case OP_LSET:
      printf("OP_LSET\t%d\n", c.u.i);
      break;
    case OP_CREF:
      printf("OP_CREF\t%d\t%d\n", c.u.r.depth, c.u.r.idx);
      break;
    case OP_CSET:
      printf("OP_CSET\t%d\t%d\n", c.u.r.depth, c.u.r.idx);
      break;
    case OP_JMP:
      printf("OP_JMP\t%d\n", c.u.i);
      break;
    case OP_JMPIF:
      printf("OP_JMPIF\t%d\n", c.u.i);
      break;
    case OP_CALL:
      printf("OP_CALL\t%d\n", c.u.i);
      break;
    case OP_TAILCALL:
      printf("OP_TAILCALL\t%d\n", c.u.i);
      break;
    case OP_RET:
      puts("OP_RET");
      break;
    case OP_LAMBDA:
      printf("OP_LAMBDA\t%d\n", c.u.i);
      break;
    case OP_CONS:
      puts("OP_CONS");
      break;
    case OP_CAR:
      puts("OP_CAR");
      break;
    case OP_NILP:
      puts("OP_NILP");
      break;
    case OP_CDR:
      puts("OP_CDR");
      break;
    case OP_ADD:
      puts("OP_ADD");
      break;
    case OP_SUB:
      puts("OP_SUB");
      break;
    case OP_MUL:
      puts("OP_MUL");
      break;
    case OP_DIV:
      puts("OP_DIV");
      break;
    case OP_MINUS:
      puts("OP_MINUS");
      break;
    case OP_EQ:
      puts("OP_EQ");
      break;
    case OP_LT:
      puts("OP_LT");
      break;
    case OP_LE:
      puts("OP_LE");
      break;
    case OP_STOP:
      puts("OP_STOP");
      break;
    }
}

void
pic_dump_irep(pic_state *pic, struct pic_irep *irep)
{
  int i;

  printf("## irep %p\n", (void *)irep);
  printf("[clen = %zd, argc = %d, localc = %d]\n", irep->clen, irep->argc, irep->localc);
  printf(":: cv_num = %d\n", irep->cv_num);
  for (i = 0; i < irep->cv_num; ++i) {
    printf(": %d -> %d\n", irep->cv_tbl[i], i);
  }
  for (i = 0; i < irep->clen; ++i) {
    print_code(pic, irep->code[i]);
  }
}

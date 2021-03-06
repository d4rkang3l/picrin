/**
 * See Copyright Notice in picrin.h
 */

#include <picrin.h>
#include "value.h"
#include "object.h"
#include "state.h"

/* FIXME: arena is consumed every time hash/cmp is executed */
#define kh_pic_str_hash(a) (pic_str_hash(pic, obj_value(pic, (a))))
#define kh_pic_str_equal(a,b) (pic_str_cmp(pic, obj_value(pic, (a)), obj_value(pic, (b))) == 0)

KHASH_DEFINE(oblist, struct string *, struct symbol *, kh_pic_str_hash, kh_pic_str_equal)

pic_value
pic_intern(pic_state *pic, pic_value str)
{
  khash_t(oblist) *h = &pic->oblist;
  struct symbol *sym;
  int it;
  int ret;

  it = kh_put(oblist, h, str_ptr(pic, str), &ret);
  if (ret == 0) {               /* if exists */
    sym = kh_val(h, it);
    pic_protect(pic, obj_value(pic, sym));
    return obj_value(pic, sym);
  }

  kh_val(h, it) = NULL;         /* dummy */

  sym = (struct symbol *)pic_obj_alloc(pic, PIC_TYPE_SYMBOL);
  sym->str = str_ptr(pic, str);
  kh_val(h, it) = sym;

  return obj_value(pic, sym);
}

pic_value
pic_sym_name(pic_state *pic, pic_value sym)
{
  return obj_value(pic, sym_ptr(pic, sym)->str);
}

static pic_value
pic_symbol_symbol_p(pic_state *pic)
{
  pic_value v;

  pic_get_args(pic, "o", &v);

  return pic_bool_value(pic, pic_sym_p(pic, v));
}

static pic_value
pic_symbol_symbol_eq_p(pic_state *pic)
{
  int argc, i;
  pic_value *argv;

  pic_get_args(pic, "*", &argc, &argv);

  for (i = 0; i < argc; ++i) {
    if (! pic_sym_p(pic, argv[i])) {
      return pic_false_value(pic);
    }
    if (! pic_eq_p(pic, argv[i], argv[0])) {
      return pic_false_value(pic);
    }
  }
  return pic_true_value(pic);
}

static pic_value
pic_symbol_symbol_to_string(pic_state *pic)
{
  pic_value sym;

  pic_get_args(pic, "m", &sym);

  return pic_sym_name(pic, sym);
}

static pic_value
pic_symbol_string_to_symbol(pic_state *pic)
{
  pic_value str;

  pic_get_args(pic, "s", &str);

  return pic_intern(pic, str);
}

void
pic_init_symbol(pic_state *pic)
{
  pic_defun(pic, "symbol?", pic_symbol_symbol_p);
  pic_defun(pic, "symbol=?", pic_symbol_symbol_eq_p);
  pic_defun(pic, "symbol->string", pic_symbol_symbol_to_string);
  pic_defun(pic, "string->symbol", pic_symbol_string_to_symbol);
}

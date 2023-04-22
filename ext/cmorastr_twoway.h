#include "cmorastr_pre.h"


#ifndef TWOWAY_TABLE_SIZE
  #define TWOWAY_TABLE_SIZE KATAKANA_RNG
#endif
#ifndef CHAR_INDEX
  #define CHAR_INDEX(ch) KANA_ID(ch)
#endif
#ifndef TWOWAY_SSIZE_T
  #define TWOWAY_SSIZE_T MINDEX_T
#endif
#ifndef TWOWAY_SSIZE_MAX
  #define TWOWAY_SSIZE_MAX MINDEX_MAX
#endif


static Py_ssize_t
two_way_search (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count);

static Py_ssize_t
two_way_mora_search (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off, 
    Py_ssize_t p_moracnt, const TWOWAY_SSIZE_T *indices, Py_ssize_t count);

static bool
two_way_prepare(
    const Katakana *needle, Py_ssize_t length, Py_ssize_t mora_cnt);

static void
two_way_set_needle(void *needle);

static void *
two_way_cache_new(void);

static void
two_way_cache_dealloc(void *needle);
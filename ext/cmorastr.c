#define MORASTR_DEBUG_MODE 0
#define MORASTR_USING_PYMEM_MALLOC

#include "cmorastr_pre.h"


typedef struct {
    PyObject_VAR_HEAD
    PyObject *string;
    MINDEX_T *indices;
} MoraStrObject;

static PyTypeObject MoraStrType;


typedef struct {
    const wchar_t *kana_data;
    uint32_t column;
} KanaColumn;


enum {
    COLUMN_A = 1,
    COLUMN_I,
    COLUMN_U,
    COLUMN_E,
    COLUMN_O,
    COLUMN_N = 8,
};

enum {
    SMALL_KANA_OFF = 5,
    COLUMN_MASK = (1 << 4) - 1,
    GLIDE = 1 << (SMALL_KANA_OFF - 1)
};


static KanaColumn KANA_COLUMNS[] = {
    {L"ァアカガサザタダナハバパマャヤラヮワ\x30f7", /*ヷ*/ COLUMN_A},
    {L"ィイキギシジチヂニヒビピミリヰ\x30f8", /*ヸ*/ COLUMN_I},
    {L"ゥウクグスズツヅヌフブプムュユルヴ", COLUMN_U},
    {L"ェエケゲセゼテデネヘベペメレヱ\x30f9", /*ヹ*/ COLUMN_E},
    {L"ォオコゴソゾトドノホボポモョヨロヲ\x30fa", /*ヺ*/ COLUMN_O},
    {L"ン", COLUMN_N},
    {NULL, 0}
};


static PyObject *converter_func = NULL;
static Py_ssize_t mapping_extra_len = 0;
static PyObject *hankaku_pair_map;
static unsigned char katakana_rimes[KATAKANA_RNG] = {
    [KANA_ID(L'ァ')] = (COLUMN_A << SMALL_KANA_OFF),
    [KANA_ID(L'ィ')] = (COLUMN_I << SMALL_KANA_OFF),
    [KANA_ID(L'ゥ')] = (COLUMN_U << SMALL_KANA_OFF),
    [KANA_ID(L'ェ')] = (COLUMN_E << SMALL_KANA_OFF),
    [KANA_ID(L'ォ')] = (COLUMN_O << SMALL_KANA_OFF),

    [KANA_ID(L'ャ')] = (COLUMN_A << SMALL_KANA_OFF) | GLIDE,
    [KANA_ID(L'ュ')] = (COLUMN_U << SMALL_KANA_OFF) | GLIDE,
    [KANA_ID(L'ョ')] = (COLUMN_O << SMALL_KANA_OFF) | GLIDE,
    [KANA_ID(L'ヮ')] = (COLUMN_A << SMALL_KANA_OFF) | GLIDE,
};


static void
init_katakana_table(void) {
    for (KanaColumn *kc = KANA_COLUMNS;; ++kc) {
        const wchar_t *kana_ch = kc->kana_data;
        uint32_t column = kc->column;
        if (!kana_ch) {break;}
        while (*kana_ch) {
            katakana_rimes[KANA_ID(*kana_ch)] |= column;
            kana_ch++;
        }
    }
}


static inline int
small_kana_vowel(Katakana k) {
    return katakana_rimes[KANA_ID(k)] >> SMALL_KANA_OFF;
}


#define VOWEL_FROM_KATAKANA(ch) (katakana_rimes[KANA_ID(ch)] & COLUMN_MASK)

static inline int
VALIDATE_MORA_BOUNDARY_(Katakana k1, Katakana k2) {
    int small_kana = small_kana_vowel(k2);

    if (small_kana && small_kana != VOWEL_FROM_KATAKANA(k1)) {
        PyErr_SetString(PyExc_ValueError, "ill-formed mora string");
        return -1;
    }
    return 0;
}

static inline int
VALIDATE_MORA_BOUNDARY__VOWEL(int vowel, Katakana k) {
    if (vowel == -1) {return 0;}
    int small_kana = small_kana_vowel(k);

    if (small_kana && small_kana != vowel) {
        PyErr_SetString(PyExc_ValueError, "ill-formed mora string");
        return -1;
    }
    return 0;
}

enum {Bounds_KANA = 0, Bounds_VOWEL};
#define VALIDATE_MORA_BOUNDARY(boundary, c1, c2) \
    ((boundary) ? \
    VALIDATE_MORA_BOUNDARY__VOWEL(c1, c2) : \
    VALIDATE_MORA_BOUNDARY_(c1, c2))


#define KatakanaArray_from_str(str) ((Katakana *)PyUnicode_DATA(str))

static inline Katakana
KATAKANA_STR_READ_(PyObject *str, Py_ssize_t idx) {
    assert(PyUnicode_KIND(str) == KATAKANA_KIND);
    const Katakana *a = KatakanaArray_from_str(str);
    return a[idx];
}
#define KATAKANA_STR_READ(str, idx) \
    KATAKANA_STR_READ_((PyObject *)(str), (idx))


static inline bool is_zenkaku_katakana(Py_UCS4 c) {
    // "ァ" to "ヾ"
    return (0x30a1 <= c && c <= 0x30fe);
}

static inline bool is_hiragana(Py_UCS4 c) {
    // "ぁ" to "ゞ", "ゖ", "ゝ", 
    return (0x3041 <= c && c <= 0x309e && (c <= 0x3096 || 0x309d <= c));
}

static PyObject *
morastr__register(PyObject *self, PyObject *mapping) {
#define CHECK_KEY_CHAR_(ch) \
    (!is_zenkaku_katakana((Py_UCS4)(ch)) && !is_hiragana((Py_UCS4)(ch)))

    static bool called = false;

    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a dict");
        return NULL;
    }

    PyObject *residue = PyDict_New();
    if (!residue) {return NULL;}
{ /* got ownership */
    if (called) {
        Py_CLEAR(converter_func);
        PyDict_Clear(hankaku_pair_map);
    }
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(mapping, &pos, &key, &value)) {
        if (!PyUnicode_CheckExact(key) || !PyUnicode_CheckExact(value)) {
            PyErr_SetString(PyExc_TypeError, "mapping must be str-to-str");
            goto error;
        }
        Py_ssize_t len;
        len = PyUnicode_GET_LENGTH(value);
        if (len != 1 || !is_zenkaku_katakana(PyUnicode_ReadChar(value, 0))) {
            PyErr_SetString(PyExc_ValueError, \
                "each value of mapping must be a single full-width katakana");
            goto error;
        }
        len = PyUnicode_GET_LENGTH(key);
        if (!len) {
            PyErr_SetString(PyExc_ValueError,
                "keys must be non-empty strings");
            goto error;
        }

        DEF_TAGGED_UCS(chs, key);
        PyObject *new_key;
        int status;
        if (len == 1) {
            long c = (long)UCSX_READ(chs, 0);
            if (!CHECK_KEY_CHAR_(c)) {goto ch_error;}
            new_key = PyLong_FromLong(c);
            if (!new_key) {goto error;}
            status = PyDict_SetItem(hankaku_pair_map, new_key, value);
            Py_DECREF(new_key);
            if (status < 0) {goto error;}
        } else if (len == 2) {
            uint64_t c1 = (uint64_t)UCSX_READ(chs, 0);
            uint64_t c2 = (uint64_t)UCSX_READ(chs, 1);
            if (!CHECK_KEY_CHAR_(c1) || !CHECK_KEY_CHAR_(c2)) {
                goto ch_error;
            }
            c2 ^= c1; c2 += 0x1000000UL;
            new_key = PyLong_FromUnsignedLongLong((c1 << 30) | c2);
            if (!new_key) {goto error;}
            status = PyDict_SetItem(hankaku_pair_map, new_key, value);
            Py_DECREF(new_key);
            if (status < 0) {goto error;}
            if (mapping_extra_len < len - 1) {
                mapping_extra_len = len - 1;
            }
        } else {
            for (Py_ssize_t i = 0; i < len; ++i) {
                Py_UCS4 c = UCSX_READ(chs, i);
                if (!CHECK_KEY_CHAR_(c)) {goto ch_error;}
            }
            if (PyDict_SetItem(residue, key, value) < 0) {
                goto error;
            }
        }
    }
    called = true;
    return residue;
}

ch_error:
    PyErr_SetString(PyExc_ValueError,
        "keys must be neither zenkaku katakana nor hiragana");

error:
    mapping_extra_len = 0;
    PyDict_Clear(hankaku_pair_map);
    Py_DECREF(residue);
    return NULL;

#undef CHECK_KEY_CHAR_
}


static PyObject *
morastr__set_converter(PyObject *self, PyObject *converter) {
    if (converter == Py_None) {
        Py_CLEAR(converter_func);
    } else if (PyCallable_Check(converter)) {
        Py_INCREF(converter);
        Py_XSETREF(converter_func, converter);
    } else {
        PyErr_SetString(PyExc_TypeError, "argument must be callable");
        return NULL;
    }
    return Py_NewRef(Py_None);
}


static MoraStrObject *empty_morastr = NULL;

static inline PyObject *Empty_MoraStr(void);
static inline bool
IS_EMPTY_MORASTR_(MoraStrObject *self) {
    return self == empty_morastr;
}
#define IS_EMPTY_MORASTR(op) IS_EMPTY_MORASTR_((MoraStrObject *)(op))
static inline bool IS_MORASTR_TYPE(PyTypeObject *);
static inline bool MoraStr_Check_(PyObject *);
#define MoraStr_Check(op) MoraStr_Check_((PyObject *)(op))
#define MoraStr_CheckExact(op) IS_MORASTR_TYPE(Py_TYPE(op))
#define MoraStr_STRING(op) ((PyObject *)((MoraStrObject *)(op))->string)
static PyObject *MoraStr_from_unicode_(PyObject *u, bool validate);
static PyObject *MoraStr_SubMoraStr(MoraStrObject *, Py_ssize_t, Py_ssize_t);


enum {MORA_CONTENT_MAX = 3};

static inline Py_ssize_t
count_morae_wo_indices(PyObject *text, Py_ssize_t length) {
    MoraStr_assert(length >= 0);
    MoraStr_assert(PyUnicode_KIND(text) == KATAKANA_KIND);

    Py_ssize_t mora_cnt = 0;
    const Katakana *text_buf = KatakanaArray_from_str(text);
    int rime, p_rime;
    int small_kana, increment, check = 1 << (MORA_CONTENT_MAX-1); /* 0b100 */
    if (length) {
        Katakana k = text_buf[0];
        rime = katakana_rimes[KANA_ID(k)];
        small_kana = rime >> SMALL_KANA_OFF;
        if (small_kana) {
            if (PyErr_WarnEx(PyExc_Warning,
                    "base string starts with a small kana", 1) < 0) {
                return -1;
            }
        }
        p_rime = rime;
    }
    for (Py_ssize_t idx = 1; idx < length; ++idx) {
        Katakana k = text_buf[idx];
        rime = katakana_rimes[KANA_ID(k)];
        small_kana = rime >> SMALL_KANA_OFF;
        increment = (!small_kana) || \
            (small_kana == (p_rime & COLUMN_MASK));
        mora_cnt += increment;
        if (increment) {check |= 1 << MORA_CONTENT_MAX;}
        check >>= 1;
        if (!check) {check = -1;}
        p_rime = rime;
    }
    if (length) {++mora_cnt;}
    if (check < 0) {
        PyErr_SetString(PyExc_ValueError,
            "each mora must have at most 3 characters");
        return -1;
    }
    return mora_cnt;
}


#if defined(_PyUnicodeWriter_Prepare)

#define KWRITER_TYPE _PyUnicodeWriter
#define KWriter_NEW() NULL
#define KWriter_READY(var) (1)

#define KWRITER_MAX_CHAR 0xffff
#define KWRITER_MIN_LENGTH 0
#define KWRITER_DEFAULT_ { \
    .kind = KATAKANA_KIND, \
    .maxchar = KWRITER_MAX_CHAR, \
    .min_length = KWRITER_MIN_LENGTH, \
    .min_char = 127, \
}
static const KWRITER_TYPE KWriter_PROTO = KWRITER_DEFAULT_;
static KWRITER_TYPE katakana_writer_ = KWRITER_DEFAULT_;


static int
KWriter_INIT_(
    KWRITER_TYPE *w, Py_ssize_t length, bool overallocate)
{
#ifdef MS_WINDOWS
  #define KWRITER_ALLOC_FACTOR 2
#else
  #define KWRITER_ALLOC_FACTOR 4
#endif

    Py_ssize_t newlen;
    PyObject *newbuffer;

    newlen = length;
    if (overallocate
        && newlen <= (PY_SSIZE_T_MAX - newlen / KWRITER_ALLOC_FACTOR))
    {
        newlen += newlen / KWRITER_ALLOC_FACTOR;
    }
    if (newlen < KWRITER_MIN_LENGTH) {
        newlen = KWRITER_MIN_LENGTH;
    }
    newbuffer = PyUnicode_New(newlen, KWRITER_MAX_CHAR);
    if (!newbuffer) {return -1;}

    w->buffer = newbuffer;
    w->pos = 0;
    w->size = PyUnicode_GET_LENGTH(newbuffer);
    w->data = PyUnicode_DATA(newbuffer);
    if (overallocate) {w->overallocate = true;}
    return 0;

#undef KWRITER_ALLOC_FACTOR
}

static inline KWRITER_TYPE *
KWriter_MAKE_(
    KWRITER_TYPE *w, Py_ssize_t length, Py_UCS4 ch, bool overallocate)
{
    if (!w) {
        w = &katakana_writer_;
        if (length > 0) {
            if (KWriter_INIT_(w, length, overallocate) == -1) {
                return NULL;
            }
            if (ch != 0x110000UL) {
                MoraStr_assert(ch <= KWRITER_MAX_CHAR);
                PyUnicode_WRITE(KATAKANA_KIND, w->data, w->pos++, ch);
            }
        }
    }
    return w;
}
#define KWriter_INIT(w, length, overallocate) ( \
    *(w) = KWriter_MAKE_(*(w), length, 0x110000UL, overallocate), \
    (*(w) ? 0 : -1) \
)

#define KWriter_Prepare(w, length, ch) ( \
    (length) <= (w)->size - (w)->pos ? 0 : \
    _PyUnicodeWriter_PrepareInternal(w, length, ch) \
)


static inline int
KWriter_WriteChar_(KWRITER_TYPE *w, Py_UCS4 ch) {
    MoraStr_assert(ch <= KWRITER_MAX_CHAR);
    if (KWriter_Prepare(w, 1, ch) < 0) {
        return -1;
    }
    Py_ssize_t pos = w->pos;
    PyUnicode_WRITE(KATAKANA_KIND, w->data, pos, ch);
    w->pos = ++pos;
    return 0;
}

#define KWriter_WriteChar(w, c, length) ( \
    *(w) ? \
    (KWriter_WriteChar_(*(w), c)) : ( \
        *(w) = KWriter_MAKE_(*(w), length, (Katakana)c, 0), \
        (*(w) ? 0 : -1) \
    ) \
)

#define KWriter_WriteCharStr(w, str, length) ( \
    *(w) ? \
    (KWriter_WriteChar_(*(w), KATAKANA_STR_READ(str, 0)) ) : ( \
        *(w) = KWriter_MAKE_(*(w), length, KATAKANA_STR_READ(str, 0), 0), \
        (*(w) ? 0 : -1) \
    ) \
)

#define KWriter_WriteStr(w, str, length) ( \
    *(w) ? \
    (_PyUnicodeWriter_WriteStr(*(w), str)) : ( \
        assert(PyUnicode_KIND(str) <= KATAKANA_KIND), \
        *(w) = KWriter_MAKE_(*(w), length, 0x110000UL, 0), \
        (*(w) ? (_PyUnicodeWriter_WriteStr(*(w), str)) : -1) \
    ) \
)

#define KWriter_WriteSubstring(w, str, start, end, length) ( \
    *(w) ? \
    (_PyUnicodeWriter_WriteSubstring(*(w), str, start, end)) : ( \
        *(w) = KWriter_MAKE_(*(w), length, 0x110000UL, 0), \
        (*(w) ? \
            (_PyUnicodeWriter_WriteSubstring(*(w), str, start, end)) : -1) \
    ) \
)


static inline PyObject *
KWriter_Finish(KWRITER_TYPE *w) {
    if (!w) {return PyUnicode_New(0, 0);}
    PyObject *result = w->buffer;
    if (!result){
        result = PyUnicode_New(0, 0);
    } else if (PyUnicode_GET_LENGTH(result) != w->pos) {
        result = _PyUnicodeWriter_Finish(w);
    }
    katakana_writer_ = KWriter_PROTO;
    return result;
}

static inline void
KWriter_Dealloc(KWRITER_TYPE *w) {
    if (w) {
        Py_CLEAR(w->buffer);
        katakana_writer_ = KWriter_PROTO;
    }
}

#undef KWRITER_MAX_CHAR

#else

#define KWRITER_TYPE PyObject
#define KWriter_NEW() PyUnicode_New(0, 0)
#define KWriter_READY(var) (var)
#define KWriter_INIT(obj, v, overallocate) (0)

#define KWriter_Prepare(obj, length, ch) (0)

#define KWriter_WriteChar(w, c, length) ( \
    PyUnicode_AppendAndDel(w, PyUnicode_FromOrdinal(c)), \
    (*(w) ? 0 : -1) \
)

#define KWriter_WriteCharStr(w, str, length) ( \
    PyUnicode_Append(w, str), \
    (*(w) ? 0 : -1) \
)

#define KWriter_WriteStr KWriter_WriteCharStr

#define KWriter_WriteSubstring(w, str, start, end, length) ( \
    PyUnicode_AppendAndDel(w, PyUnicode_Substring(str, start, end)), \
    (*(w) ? 0 : -1) \
)

static inline PyObject *
KWriter_Finish(KWRITER_TYPE *w) {return w;}

static inline void
KWriter_Dealloc(KWRITER_TYPE *w) {Py_CLEAR(w);}

#endif


static inline bool all_in_katakana_range4(uint64_t x) {
    static const uint64_t start = 0x30a1;
    static const uint64_t end = 0x30fe + 1;

    return (
        (
            ~0ULL / 0xffff * (0x7fff + end)
            - (x & ~0ULL / 0xffff * 0x7fff)
            & ~x & (x & ~0ULL / 0xffff * 0x7fff)
            + ~0ULL / 0xffff * (0x8000 - start)
        )
        & ~0ULL / 0xffff * 0x8000
    ) == 0x8000800080008000ULL;
}

static inline Py_ssize_t
skip_katakana(Py_ssize_t length, int kind, void *data) {
    Py_ssize_t i = 0;
#if defined(__LP64__) || defined(_WIN64)
    if (kind != KATAKANA_KIND || (uintptr_t)data & 7) {
        return i;
    }
    Py_ssize_t limit = length >> 2 << 2;
    uint64_t v;
    while (i < limit) {
        v = *((uint64_t *)((Katakana *)data + i));
        if (all_in_katakana_range4(v)) {
            i += 4;
        } else {
            break;
        }
    }
#endif
    return i;
}

static PyObject *
normalize_text_x(PyObject *text, bool validate) {
    assert(PyUnicode_Check(text));
    if (PyUnicode_READY(text) == -1) {return NULL;}
    Py_ssize_t length = PyUnicode_GET_LENGTH(text);
    if (length <= 0) {
        Py_INCREF(text);
        return text;
    }

    KWRITER_TYPE *new_text = KWriter_NEW();
    if (!KWriter_READY(new_text)) {return NULL;}
{ /* got ownership */
    DEF_TAGGED_UCS(text, text);
    Py_ssize_t p = 0;
    Py_ssize_t i = length < 16 ? 0 : \
        skip_katakana(length, UCSX_KIND(text), UCSX_DATA(text));
    if (i == length) {goto loop_end;}
    do {
        Py_UCS4 c = UCSX_READ(text, i);
        PyObject *substr;

        if (is_zenkaku_katakana(c)) {
            if (++i >= length) {break;}
            continue;
        }
        if (p != i) {
            if (KWriter_WriteSubstring(
                &new_text, text, p, i, length) == -1) {goto error;}
        }
        // "ぁ" to "ゖ", "ゝ", "ゞ"
        if ((0x3041 <= c && c <= 0x3096) || c == 0x309d || c == 0x309e) {
            if (KWriter_WriteChar(
                &new_text, c + 0x60, length) == -1) {goto error;}
        } else {
            PyObject *key;
            if (mapping_extra_len && i + 1 < length) {
                uint64_t rc = (uint64_t)UCSX_READ(text, i + 1);
                rc ^= c; rc += 0x1000000UL;
                key = PyLong_FromUnsignedLongLong(((uint64_t)c << 30) | rc);
                if (!key) {goto error;}
                substr = PyDict_GetItem(hankaku_pair_map, key);
                Py_DECREF(key);
                if (substr) {
                    if (KWriter_WriteCharStr(
                        &new_text, substr, length) == -1) {goto error;}
                    p = ++i + 1;
                    if (++i >= length) {break;}
                    continue;
                }
            }
            key = PyLong_FromLong((long)c);
            if (!key) {goto error;}
            substr = PyDict_GetItem(hankaku_pair_map, key);
            Py_DECREF(key);
            if (substr) {
                if (KWriter_WriteCharStr(
                    &new_text, substr, length) == -1) {goto error;}
            } else if (validate) {
                substr = PyUnicode_FromOrdinal(c);
                if (!substr) {goto error;}
                PyErr_Format(PyExc_ValueError,
                    "invalid character: '%U' (u+%04x)", substr, c);
                Py_DECREF(substr);
                goto error;
            }
        }
        p = ++i;
    } while (i < length);

loop_end:
    if (!p) {
        Py_INCREF(text);
        KWriter_Dealloc(new_text);
        return text;
    }
    if (p < length) {
        if (KWriter_WriteSubstring(
            &new_text, text, p, length, length) == -1) {goto error;}
    }
    return KWriter_Finish(new_text);
}

error:
    KWriter_Dealloc(new_text);
    return NULL;
}


static inline PyObject *
normalize_text(PyObject *text, bool validate) {
    if (!converter_func) {
        return normalize_text_x(text, validate);
    }
    PyObject *args = PyTuple_Pack(1, text);
    if (!args) {return NULL;}
    text = PyObject_CallObject(converter_func, args);
    Py_DECREF(args);
    if (!text) {return NULL;}
    PyObject *result = normalize_text_x(text, validate);
    Py_DECREF(text);
    return result;
}


static PyObject *
MoraStr_count_all(PyObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "ignore", NULL};
    PyObject *string;
    BoolPred ignore = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "U|$p", kwlist, &string, &ignore)) {
        return NULL;
    }
    string = normalize_text(string, !ignore);
    if (!string) {return NULL;}
{ /* got ownership */
    Py_ssize_t length = PyUnicode_GET_LENGTH(string);
    if (length) {
        length = count_morae_wo_indices(string, length);
        if (length == -1) {goto error;}
    }
    Py_DECREF(string);
    return PyLong_FromSsize_t(length);
}

error:
    Py_DECREF(string);
    return NULL;
}

PyDoc_STRVAR(morastr_count_all_docstring,
    "count_all(kana_string, /, *, ignore=False)\n"
    "--\n\n"
    "Returns the total number of morae that kana_string holds in its phonemic \n"
    "form. This is roughly equivalent to len(MoraStr(kana_string)) but slightly \n"
    "more efficient as this method avoids generating intermediate objects. The \n"
    "signature is the same as that of MoraStr(), except that the first argument \n"
    "of this method is non-optional and accepts only str objects.\n"
    "\n"
    "See also MoraStr()");


static inline int
get_column_if_katakana(Py_UCS4 ch) {
    return is_zenkaku_katakana(ch) ? VOWEL_FROM_KATAKANA(ch) : 0;
}

static PyObject *
with_prolonged_sound_marks_x(
    int kind, PyObject *source, Py_ssize_t length, uint64_t flags)
{
    static const Py_UCS4 LONG_MARK = L'ー';
    static const uint64_t MASK32 = (1ULL << 32) - 1;
    static const int WIDTH = 16;

    PyObject *target = NULL;

    Py_UCS4 Nn = (Py_UCS4)(flags & MASK32);
    flags >>= WIDTH * 2;
    int Ei = (int)(flags & (MASK32 >> WIDTH));  // & COLUMN_E;
    flags >>= WIDTH;
    int Ou = (int)flags;                        // & COLUMN_O;
    void *s = PyUnicode_DATA(source);
    void *t = s;

    Py_UCS4 p = MoraStr_Unicode_READ(kind, s, 0);
    for (Py_ssize_t i = 1; i < length; ++i) {
        Py_UCS4 c = MoraStr_Unicode_READ(kind, s, i);
        bool matched = false;
        int v1, v2 = 0;
        switch (c) {
            case L'ァ':
            case L'ア':
                v2 = COLUMN_A; break;
            case L'ィ':
            case L'イ':
                v1 = get_column_if_katakana(p);
                if (v1 && (v1 == COLUMN_I || v1 == Ei)) {
                    matched = true;
                }
                break;
            case L'ゥ':
            case L'ウ':
                v1 = get_column_if_katakana(p);
                if (v1 && (v1 == COLUMN_U || v1 == Ou)) {
                    matched = true;
                }
                break;
            case L'ェ':
            case L'エ':
                v2 = COLUMN_E; break;
            case L'ォ':
            case L'オ':
                v2 = COLUMN_O; break;
            default:
                if (c == Nn) {v2 = COLUMN_N;}
        }
        if (v2 && is_zenkaku_katakana(p)) {
            v1 = get_column_if_katakana(p);
            matched = (v1 == v2);
        }
        if (!matched) {
            p = c; continue;
        }
        if (!target) {
            target = PyUnicode_New(length, PyUnicode_MAX_CHAR_VALUE(source));
            if (!target) {return NULL;}
            _PyUnicode_FastCopyCharacters(target, 0, source, 0, length);
            t = PyUnicode_DATA(target);
        }
        MoraStr_Unicode_WRITE(kind, t, i, LONG_MARK);
        p = MoraStr_Unicode_READ(kind, s, ++i);
    }
    if (target) {return target;}

    Py_INCREF(source);
    return source;
}


static PyObject *
with_prolonged_sound_marks(
    PyObject *source, uint64_t flags, size_t rep)
{
    static const Py_UCS4 LONG_MARK = L'ー';
    static const uint64_t MASK32 = (1ULL << 32) - 1;
    static const int WIDTH = 16;

    if (!rep) {goto unchanged;}

    int kind = PyUnicode_KIND(source);
    Py_ssize_t length = PyUnicode_GET_LENGTH(source);
    if (kind == PyUnicode_1BYTE_KIND || !length) {
        goto unchanged;
    }

    PyObject* result;
    if (rep == 1) {
        if (kind == PyUnicode_2BYTE_KIND) {
            result = with_prolonged_sound_marks_x(
                PyUnicode_2BYTE_KIND, source, length, flags);
        } else {
            result = with_prolonged_sound_marks_x(
                PyUnicode_4BYTE_KIND, source, length, flags);
        }
        return result;
    }

    PyObject *target = NULL;

#define Nn ( (Py_UCS4)((flags) & MASK32) )
#define Ei ( (int)(((flags) >> WIDTH * 2) & (MASK32 >> WIDTH)) )
#define Ou ( (int)((flags) >> (WIDTH * 3)) )

    void *s = PyUnicode_DATA(source);
    void *t = s;

    Py_UCS4 p = MoraStr_Unicode_READ(kind, s, 0);
    for (Py_ssize_t i = 1; i < length; ++i) {
        Py_UCS4 c = MoraStr_Unicode_READ(kind, s, i);
        size_t count = rep;
        bool matched = false;
        int v1, v2 = 0;
        switch (c) {
            case L'ァ':
            case L'ア':
                v2 = COLUMN_A; break;
            case L'ィ':
            case L'イ':
                v1 = get_column_if_katakana(p);
                if (v1) {
                    if (v1 == COLUMN_I) {
                        matched = true;
                    } else if (v1 == Ei) {
                        count = 1;
                        matched = true;
                    }
                }
                break;
            case L'ゥ':
            case L'ウ':
                v1 = get_column_if_katakana(p);
                if (v1) {
                    if (v1 == COLUMN_U) {
                        matched = true;
                    } else if (v1 == Ou) {
                        count = 1;
                        matched = true;
                    }
                }
                break;
            case L'ェ':
            case L'エ':
                v2 = COLUMN_E; break;
            case L'ォ':
            case L'オ':
                v2 = COLUMN_O; break;
            default:
                if (c == Nn) {v2 = COLUMN_N;}
        }
        if (v2 && is_zenkaku_katakana(p)) {
            v1 = get_column_if_katakana(p);
            matched = (v1 == v2);
        }
        if (!matched) {
            p = c; continue;
        }
        if (!target) {
            target = PyUnicode_New(length, PyUnicode_MAX_CHAR_VALUE(source));
            if (!target) {goto error;}
            _PyUnicode_FastCopyCharacters(target, 0, source, 0, length);
            t = PyUnicode_DATA(target);
        }
        if (c == L'ン') {
            p = c;
            do {
                MoraStr_Unicode_WRITE(kind, t, i, LONG_MARK);
                c = MoraStr_Unicode_READ(kind, s, ++i);
                if (!(--count)) {break;}
            } while (p == c);
        } else {
            p = (p + 1) / 2;
            do {
                MoraStr_Unicode_WRITE(kind, t, i, LONG_MARK);
                c = MoraStr_Unicode_READ(kind, s, ++i);
                if (!(--count)) {break;}
            } while (p == (c + 1) / 2);
        }
        p = c;
    }
    if (target) {return target;}

unchanged:
    Py_INCREF(source);
    return source;

error:
    Py_XDECREF(target);
    return NULL;

#undef Nn
#undef Ei
#undef Ou
}


static PyObject*
MoraStr_vowel_to_choon(
    PyObject *Py_UNUSED(ignored), PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"", "maxrep", "clean", "ou", "ei", "nn", NULL};
    static const int WIDTH = 16;

    PyObject *kana_string;
    Py_ssize_t rep = 1;
    BoolPred clean = false, ou = false, ei = false, nn = true;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|n$pppp", kwlist,
            &kana_string, &rep, &clean, &ou, &ei, &nn)) {
        return NULL;
    }

    if (rep < 0) {rep = PY_SSIZE_T_MAX;}
    uint64_t flags = ou * COLUMN_O;
    flags <<= WIDTH;
    flags |= ei * COLUMN_E;
    flags <<= WIDTH * 2;
    flags |= nn ? L'ン' : 0x110000UL;

    PyObject *result;
    if (PyUnicode_Check(kana_string)) {
        kana_string = PyUnicode_FromObject(kana_string);
        if (!kana_string) {return NULL;}
        result = \
            with_prolonged_sound_marks(kana_string, flags, (size_t)rep);
        Py_DECREF(kana_string);
        if (clean) {
            if (!result) {return NULL;}
            kana_string = result;
            result = normalize_text(kana_string, false);
            Py_DECREF(kana_string);
        }
        return result;
    } else if (MoraStr_Check(kana_string)) {
        Py_ssize_t length = Py_SIZE(kana_string);
        kana_string = MoraStr_STRING(kana_string);
        kana_string = \
            with_prolonged_sound_marks(kana_string, flags, (size_t)rep);
        if (!kana_string) {return NULL;}
        result = MoraStr_from_unicode_(kana_string, true);
        Py_DECREF(kana_string);
        if (!result) {return NULL;}
        if (Py_SIZE(result) != length) {
            PyErr_SetString(PyExc_ValueError, "mora length inconsistency");
            Py_DECREF(result);
            return NULL;
        }
        return result;
    } else {
        PyErr_Format(PyExc_TypeError,
            "argument must be a kana string "
            "or a MoraStr object, not '%.200s'",
            Py_TYPE(kana_string)->tp_name);
    }
    return NULL;
}


static inline PyObject *
replace_prolonged_sound_marks_x(
    int kind, PyObject *source, Py_ssize_t length, Py_UCS4 *err_ch)
{
    static const Py_UCS4 LONG_MARK = L'ー';
    static const Py_UCS4 VOWEL_CHARS[] = {
        L'ア', L'イ', L'ウ', L'エ', L'オ', L'\0', L'\0', L'ン'
    };

    PyObject *target = NULL;
    void *s = PyUnicode_DATA(source);
    void *t = s;

    Py_UCS4 p = MoraStr_Unicode_READ(kind, s, 0);
    if (err_ch && p == LONG_MARK) {goto error;}
    for (Py_ssize_t i = 1; i < length; ++i) {
        Py_UCS4 c = MoraStr_Unicode_READ(kind, s, i);
        if (c != LONG_MARK) {
            p = c; continue;
        }
        int column = get_column_if_katakana(p);
        if (!column) {
            if (err_ch) {
                *err_ch = p;
                goto error;
            }
            /* Note: PyUnicode data are null-terminated */
            /* so it's safe to read s[length] */
            do {
                c = MoraStr_Unicode_READ(kind, s, ++i);
            } while (c == LONG_MARK);
            p = c;
            continue;
        }
        Py_UCS4 k = VOWEL_CHARS[column-1];
        if (!target) {
            target = PyUnicode_New(length, PyUnicode_MAX_CHAR_VALUE(source));
            if (!target) {goto error;}
            _PyUnicode_FastCopyCharacters(target, 0, source, 0, length);
            t = PyUnicode_DATA(target);
        }
        do {
            c = MoraStr_Unicode_READ(kind, s, i+1);
            MoraStr_Unicode_WRITE(kind, t, i++, k);
        } while (c == LONG_MARK);
        p = c;
    }
    if (target) {return target;}

    Py_INCREF(source);
    return source;

error:
    Py_XDECREF(target);
    return NULL;
}


static PyObject *
replace_prolonged_sound_marks(PyObject *kana_string, bool strict) {
    static const uint32_t INVALID_CHAR = 0x110000UL;

    MoraStr_assert(PyUnicode_CheckExact(kana_string));
    Py_ssize_t length = PyUnicode_GET_LENGTH(kana_string);
    int kind = PyUnicode_KIND(kana_string);
    if (!length || kind == PyUnicode_1BYTE_KIND) {
        Py_INCREF(kana_string);
        return kana_string;
    }

    PyObject *result;
    Py_UCS4 err_ch = INVALID_CHAR;
    if (kind == PyUnicode_2BYTE_KIND) {
        result = replace_prolonged_sound_marks_x(
            PyUnicode_2BYTE_KIND,
            kana_string, length, strict ? &err_ch : NULL);
    } else {
        result = replace_prolonged_sound_marks_x(
            PyUnicode_4BYTE_KIND,
            kana_string, length, strict ? &err_ch : NULL);
    }
    if (!result) {
        if (!PyErr_Occurred()) {
            if (err_ch == INVALID_CHAR) {
                PyErr_SetString(PyExc_ValueError, \
                    "unexpected prolonged sound mark; "
                    "at the beginning of the whole string");
            } else {
                PyObject *err_str = PyUnicode_FromOrdinal(err_ch);
                if (err_str) {
                    PyErr_Format(PyExc_ValueError, \
                        "unexpected prolonged sound mark; "
                        "appeared after '%U' (u+%04x)", err_str, err_ch);
                    Py_DECREF(err_str);
                }
            }
        }
        return NULL;
    }
    return result;
}


static PyObject*
MoraStr_choon_to_vowel(
    PyObject *Py_UNUSED(ignored), PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"", "strict", "clean", NULL};
    PyObject *kana_string;
    BoolPred strict = true, clean = false;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|$pp", kwlist,
            &kana_string, &strict, &clean)) {
        return NULL;
    }

    PyObject *result;
    if (PyUnicode_Check(kana_string)) {
        kana_string = PyUnicode_FromObject(kana_string);
        if (!kana_string) {return NULL;}
        result = replace_prolonged_sound_marks(kana_string, strict);
        Py_DECREF(kana_string);
        if (clean) {
            if (!result) {return NULL;}
            kana_string = result;
            result = normalize_text(kana_string, false);
            Py_DECREF(kana_string);
        }
        return result;
    } else if (MoraStr_Check(kana_string)) {
        Py_ssize_t length = Py_SIZE(kana_string);
        kana_string = MoraStr_STRING(kana_string);
        kana_string = replace_prolonged_sound_marks(kana_string, strict);
        if (!kana_string) {return NULL;}
        result = MoraStr_from_unicode_(kana_string, true);
        Py_DECREF(kana_string);
        if (!result) {return NULL;}
        if (Py_SIZE(result) != length) {
            PyErr_SetString(PyExc_ValueError, "mora length inconsistency");
            Py_DECREF(result);
            return NULL;
        }
        return result;
    } else {
        PyErr_Format(PyExc_TypeError,
            "argument must be a kana string "
            "or a MoraStr object, not '%.200s'",
            Py_TYPE(kana_string)->tp_name);
    }
    return NULL;
}


#define MoraStr_INDICES(self) (((MoraStrObject *)self)->indices)

static inline MINDEX_T *
MoraStr_INDICES_ALLOC_(uint32_t size) {
    MINDEX_T *indices;
    if (size <= PY_SSIZE_T_MAX / sizeof(MINDEX_T)) {
        indices = (MINDEX_T *)MoraStr_Malloc(sizeof(MINDEX_T)*size);
        if (indices) {return indices;}
    }
    PyErr_NoMemory();
    return NULL;
}
#define MoraStr_INDICES_ALLOC(size) MoraStr_INDICES_ALLOC_((uint32_t)(size))

#define MoraStr_INDICES_DEL(indices) do { \
    MoraStr_Free(indices); \
    (indices) = NULL; \
} while(0)

#define INDICES_FILL_COPY(target, source, count, initial) do { \
    MINDEX_T *RESTRICT _target = (target); \
    MINDEX_T *_source = (source); \
    Py_ssize_t _count = (count); \
    MINDEX_T _initial = (initial); \
    if (_source) { \
        for (Py_ssize_t i = 0; i < _count; ++i) { \
            _target[i] = _initial + _source[i]; \
        } \
    } else { \
        for (MINDEX_T i = 0; i < MINDEX(_count); ++i) { \
            _target[i] = _initial + i + 1; \
        } \
    } \
} while(0)


static Py_ssize_t
count_morae(PyObject *text, Py_ssize_t length, MINDEX_T **indices_p) {
    static MINDEX_T pool[32] = {0};

    MoraStr_assert(length >= 0);
    MoraStr_assert(PyUnicode_KIND(text) == KATAKANA_KIND);

    if (length > MINDEX_MAX) {
        PyErr_SetString(PyExc_OverflowError, "base string is too long");
        return -1;
    }

#define length MINDEX(length)
    MINDEX_T *indices;
    MINDEX_T mora_cnt = 0;
    bool allocated = false;

    if (length > 32) {
        indices = MoraStr_INDICES_ALLOC(length);
        if (!indices) {return -1;}
        allocated = true;
    } else {
        indices = pool;
    }
{ /* got ownership */
    const Katakana *text_buf = KatakanaArray_from_str(text);
    int rime, p_rime;
    int small_kana, increment, check = 1 << (MORA_CONTENT_MAX-1); /* 0b100 */
    if (length) {
        Katakana k = text_buf[0];
        rime = katakana_rimes[KANA_ID(k)];
        small_kana = rime >> SMALL_KANA_OFF;
        if (small_kana) {
            if (PyErr_WarnEx(PyExc_Warning,
                    "base string starts with a small kana", 1) < 0) {
                goto error;
            }
        }
        p_rime = rime;
    }
    for (MINDEX_T idx = 1; idx < length; ++idx) {
        Katakana k = text_buf[idx];
        rime = katakana_rimes[KANA_ID(k)];
        small_kana = rime >> SMALL_KANA_OFF;
        increment = (!small_kana) || \
            (small_kana == (p_rime & COLUMN_MASK));
        if (increment) {
            indices[mora_cnt++] = idx;
            check |= 1 << MORA_CONTENT_MAX;
        }
        check >>= 1;
        if (!check) {check = -1;}
        p_rime = rime;
    }
    if (length) {indices[mora_cnt++] = length;}
    if (check < 0) {
        PyErr_SetString(PyExc_ValueError,
            "each mora must have at most 3 characters");
        goto error;
    }
    if (length == mora_cnt) {
        if (allocated) {MoraStr_INDICES_DEL(indices);}
        *indices_p = NULL;
    } else if (!allocated) {
        MINDEX_T *indices = MoraStr_INDICES_ALLOC(mora_cnt);
        if (!indices) {goto error;}
        if (length < 8) {
            for (int i = 0; i < mora_cnt; ++i) {indices[i] = pool[i];}
        } else {
            memcpy(indices, pool, sizeof(MINDEX_T)*mora_cnt);
        }
        *indices_p = indices;
    } else {
        MINDEX_T *adjusted = indices;
        MoraStr_RESIZE(adjusted, MINDEX_T, mora_cnt);
        if (!adjusted) {
            PyErr_NoMemory();
            goto error;
        }
        *indices_p = adjusted;
    }
    return mora_cnt;
}

error:
    if (allocated) {MoraStr_INDICES_DEL(indices);}
    return -1;

#undef length
}


static PyObject *
MoraStr_copy_(PyTypeObject *type, MoraStrObject *obj) {
    Py_ssize_t mora_cnt = Py_SIZE(obj);
    if (!mora_cnt && IS_MORASTR_TYPE(type)) {
        return Empty_MoraStr();
    }

    PyObject *string = MoraStr_STRING(obj);
    MINDEX_T *indices = NULL;
    if (MoraStr_INDICES(obj)) {
        indices = MoraStr_INDICES_ALLOC(mora_cnt);
        if (!indices) {return NULL;}
        Py_MEMCPY(indices, MoraStr_INDICES(obj), sizeof(MINDEX_T)*mora_cnt);
    }

    MoraStrObject *self = (MoraStrObject *)type->tp_alloc(type, 0);
    if (!self) {
        MoraStr_INDICES_DEL(indices);
        return NULL;
    }
    Py_INCREF(string);

    Py_SET_SIZE(self, mora_cnt);
    self->string = string;
    self->indices = indices;
    return (PyObject *)self;
}


static PyObject *
MoraStr_from_unicode_(PyObject *u, bool validate) {
    static PyTypeObject *type = &MoraStrType;

    assert(PyUnicode_Check(u));
    PyObject *string = normalize_text(u, validate);
    if (!string) {return NULL;}
    MINDEX_T *indices = NULL;
{ /* got ownership */
    if (!PyUnicode_CheckExact(string)) {
        u = PyUnicode_FromObject(string);
        if (!u) {goto error;}
        Py_XSETREF(string, u);
    }
    Py_ssize_t length = PyUnicode_GET_LENGTH(string), mora_cnt;
    mora_cnt = length ? count_morae(string, length, &indices) : 0LL;

    if (mora_cnt == -1) {goto error;}
    if (!mora_cnt) {
        Py_DECREF(string);
        return Empty_MoraStr();
    }
    MoraStrObject *morastr = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!morastr) {goto error;}

    Py_SET_SIZE(morastr, mora_cnt);
    morastr->string = string;
    morastr->indices = indices;
    return (PyObject *) morastr;
}

error:
    Py_DECREF(string);
    MoraStr_INDICES_DEL(indices);
    return NULL;
}


static PyObject *
MoraStr_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "ignore", NULL};
    PyObject *obj = NULL;
    BoolPred ignore = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O$p", kwlist, &obj, &ignore)) {
        return NULL;
    }
    if (!obj) {
        if (IS_MORASTR_TYPE(type)) {return Empty_MoraStr();}
        obj = PyUnicode_New(0, 0);
        if (!obj) {return NULL;}
    } else if (!PyUnicode_CheckExact(obj)) {
        if (IS_MORASTR_TYPE(type) && MoraStr_CheckExact(obj)) {
            Py_INCREF(obj);
            return obj;
        } else if (MoraStr_Check(obj)) {
            return MoraStr_copy_(type, (MoraStrObject *)obj);
        } else if (!PyUnicode_Check(obj)) {
            PyErr_Format(PyExc_TypeError,
                "MoraStr() argument must be a kana string "
                "or a MoraStr object, not '%.200s'",
                Py_TYPE(obj)->tp_name);
            return NULL;
        }
        obj = PyUnicode_FromObject(obj);
        if (!obj) {return NULL;}
    } else {
        Py_INCREF(obj);
    }

    PyObject *string;
    string = normalize_text(obj, !ignore);
    Py_DECREF(obj);
    if (!string) {return NULL;}
    MINDEX_T *indices = NULL;
{ /* got ownership */
    Py_ssize_t length = PyUnicode_GET_LENGTH(string), mora_cnt;
    mora_cnt = length ? count_morae(string, length, &indices) : 0LL;

    if (mora_cnt == -1) {goto error;}
    if (!mora_cnt && IS_MORASTR_TYPE(type)) {
        Py_DECREF(string);
        return Empty_MoraStr();
    }
    MoraStrObject *self = (MoraStrObject *)type->tp_alloc(type, 0);
    if (!self) {goto error;}

    Py_SET_SIZE(self, mora_cnt);
    self->string = string;
    self->indices = indices;
    return (PyObject *)self;
}

error:
    Py_DECREF(string);
    MoraStr_INDICES_DEL(indices);
    return NULL;
}


static void
MoraStr_dealloc(MoraStrObject *self) {
    Py_XDECREF(self->string);
    MoraStr_Free(self->indices);
    Py_TYPE(self)->tp_free((PyObject *) self);
}


static inline int
MoraStr_VALIDATE_NUM_ARGS(const char *fn_name,
        size_t least, size_t most, size_t given)
{
    int exceeded = given < least ? 0 : most < given ? 1 : -1;
    if (exceeded == -1) {return 0;}
    size_t req_num = exceeded ? most : least;
    PyErr_Format(PyExc_TypeError,
        "%.200s() takes at %s %zu %s (%zu given)",
        fn_name, exceeded ? "most" : "least", req_num,
        req_num == 1 ? "argument" : "arguments" , given);
    return -1;
}


static inline int
MoraStr_SliceIndex(PyObject *val, Py_ssize_t *idx_p) {
    static const char *err_msg = \
        "slice indices must be integers or None or have an __index__ method";

    if (val != Py_None) {
        if (PyIndex_Check(val)) {
            Py_ssize_t idx = PyNumber_AsSsize_t(val, NULL);
            if (idx == -1 && PyErr_Occurred()) {return 0;}
            *idx_p = idx;
        } else {
            PyErr_SetString(PyExc_TypeError, err_msg);
            return 0;
        }
    }
    return 1;
}


static PyObject *
MoraStr_repr(PyObject *self) {
    const char *name = get_type_name(self);
    Py_ssize_t mora_cnt = Py_SIZE(self);
    PyObject *string = MoraStr_STRING(self), *result;
    if (!mora_cnt) {
        return PyUnicode_FromFormat("%s()", name);
    }
    Py_ssize_t s_len = PyUnicode_GET_LENGTH(string), offset;
    long long t_len;
    PyObject *prefix = PyUnicode_FromString(name);
    if (!prefix) {return NULL;}
{ /* got ownership */
    offset = PyUnicode_GET_LENGTH(prefix);
    t_len = s_len + mora_cnt * 3LL + 1;
    if (t_len > (long long)PY_SSIZE_T_MAX - offset) {
        PyErr_SetString(PyExc_OverflowError, "repr string is too long");
        goto error;
    }
    t_len += offset;

    Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(prefix);
    result = PyUnicode_New((Py_ssize_t)t_len, Py_MAX(maxchar, 0xffff));
    if (!result) {goto error;}
    _PyUnicode_FastCopyCharacters(result, 0, prefix, 0, offset);
    Py_DECREF(prefix);
}
    const Katakana *source = KatakanaArray_from_str(string);
    DEF_TAGGED_UCS(target, result);
    MoraStr_assert(UCSX_KIND(target) != PyUnicode_1BYTE_KIND);

    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t i = 0, j = offset;
    UCSX_WRITE(target, j, '('); ++j;
    if (!indices) {
        do {
            Py_UCS4 ch = source[i++];
            UCSX_WRITE(target, j, '\''); ++j;
            UCSX_WRITE(target, j, ch); ++j;
            UCSX_WRITE(target, j, '\''); ++j;
            UCSX_WRITE(target, j, ' '); ++j;
        } while (i < mora_cnt);
    } else {
        MINDEX_T *indices_end = indices + mora_cnt;
        Py_ssize_t next, p;
        do {
            next = *indices++;
            p = j++;
            while (i < next) {
                Py_UCS4 ch = source[i++];
                UCSX_WRITE(target, j, ch); ++j;
            }
            UCSX_WRITE(target, p, '\'');
            UCSX_WRITE(target, j, '\''); ++j;
            UCSX_WRITE(target, j, ' '); ++j;
        } while (indices < indices_end);
    }
    UCSX_WRITE(target, j - 1, ')');
    return result;

error:
    Py_DECREF(prefix);
    return NULL;
}


static inline PyObject *
MoraStr_subtype_call(PyTypeObject *type, PyObject *arg) {
    PyObject *args = PyTuple_Pack(1, arg);
    if (!args) {return NULL;}
    PyObject *result = PyObject_Call((PyObject *)type, args, NULL);
    Py_DECREF(args);
    return result;
}


static PyObject *
MoraStr_get_slice_typed(
    MoraStrObject *self, Py_ssize_t start, Py_ssize_t end, PyTypeObject *type)
{
    PyObject *string;
    if (!IS_MORASTR_TYPE(type)) {goto subclass;}

    if (start == end) {return Empty_MoraStr();}
    MINDEX_T *indices = MoraStr_INDICES(self);
    MoraStrObject *morastr;
    if (!indices) {
        string = PyUnicode_Substring(MoraStr_STRING(self), start, end);
        if (!string) {return NULL;}
{ /* got ownership */
        morastr = (MoraStrObject *) type->tp_alloc(type, 0);
        if (!morastr) {goto error;}
        Py_SET_SIZE(morastr, end - start);
        morastr->string = string;
        morastr->indices = NULL;
        return (PyObject *)morastr;
}
    }

    Py_ssize_t s_start = start ? indices[start-1] : 0LL;
    Py_ssize_t s_end = indices[end-1];
    string = PyUnicode_Substring(MoraStr_STRING(self), s_start, s_end);
    if (!string) {return NULL;}
{ /* got ownership */
    MINDEX_T *new_indices;
    Py_ssize_t mora_cnt = end - start;
    if (mora_cnt == s_end - s_start) {
        new_indices = NULL;
    } else {
        new_indices = MoraStr_INDICES_ALLOC(mora_cnt);
        if (!new_indices) {goto error;}
        if (!start) {
            INDICES_FILL_COPY(new_indices, indices, mora_cnt, 0);
        } else {
            for (Py_ssize_t i = 0; i < mora_cnt; ++i) {
                new_indices[i] = indices[start+i] - MINDEX(s_start);
            }
        }
    }
    morastr = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!morastr) {
        MoraStr_INDICES_DEL(new_indices);
        goto error;
    }
    Py_SET_SIZE(morastr, mora_cnt);
    morastr->string = string;
    morastr->indices = new_indices;
    return (PyObject *) morastr;
}

error:
    Py_DECREF(string);
    return NULL;

subclass:
    if (start == end) {
        string = PyUnicode_New(0, 0);
    } else {
        MINDEX_T *indices = MoraStr_INDICES(self);
        if (indices) {
            start = start ? indices[start-1] : 0LL;
            end = indices[end-1];
        }
        string = PyUnicode_Substring(MoraStr_STRING(self), start, end);
    }
    if (!string) {return NULL;}
    PyObject *result = MoraStr_subtype_call(type, string);
    Py_DECREF(string);
    if (!result) {
        PyObject *err = PyErr_Occurred();
        if (err && PyErr_GivenExceptionMatches(err, PyExc_TypeError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError,
                "'%.200s' object doesn't support slicing",
                type->tp_name);
        }
    }
    return result;
}


static PyObject *
MoraStr_SubMoraStr(MoraStrObject *self, Py_ssize_t start, Py_ssize_t end) {
    return MoraStr_get_slice_typed(self, start, end, &MoraStrType);
}


static Py_ssize_t
MoraStr_length(MoraStrObject *self) {
    return Py_SIZE(self);
}


static PyObject *
MoraStr_concat(MoraStrObject *self, PyObject *arg) {
    static const char *err_fmt = \
        "can only concatenate MoraStr (not \"%.200s\") to MoraStr";

    MoraStrObject *other;
    if (MoraStr_Check(arg)) {
        Py_INCREF(arg);
        if (IS_EMPTY_MORASTR(self) && MoraStr_CheckExact(arg)) {
            return arg;
        }
        other = (MoraStrObject *)arg;
    } else if (PyUnicode_Check(arg)) {
        PyObject *morastr = MoraStr_from_unicode_(arg, true);
        if (!morastr) {return NULL;}
        if (IS_EMPTY_MORASTR(self)) {return morastr;}
        other = (MoraStrObject *)morastr;
    } else {
        PyErr_Format(PyExc_TypeError,
            err_fmt, Py_TYPE(arg)->tp_name);
        return NULL;
    }

    PyTypeObject *type = Py_TYPE(self);
    Py_ssize_t r_mora_cnt, l_mora_cnt, mora_cnt;
    PyObject *l_string, *r_string, *string = NULL;
    MINDEX_T *indices = NULL;
    size_t l_length, r_length, length;
{ /* got ownership */
    r_mora_cnt = Py_SIZE(other);
    if (!r_mora_cnt) {
        if (IS_MORASTR_TYPE(type)) {
            Py_INCREF(self);
            Py_DECREF(other);
            return (PyObject *)self;
        }
        PyObject *morastr;
        morastr = MoraStr_subtype_call(type, MoraStr_STRING(self));
        if (!morastr) {goto error;}
        Py_DECREF(other);
        return morastr;
    }
    l_mora_cnt = Py_SIZE(self);
    if (!l_mora_cnt) {
        PyObject *morastr;
        morastr = MoraStr_subtype_call(type, MoraStr_STRING(other));
        if (!morastr) {goto error;}
        Py_DECREF(other);
        return morastr;
    }

    l_string = MoraStr_STRING(self);
    r_string = MoraStr_STRING(other);
    l_length = PyUnicode_GET_LENGTH(l_string);
    r_length = PyUnicode_GET_LENGTH(r_string);
    length = l_length + r_length;
    if (length > MINDEX_MAX) {
        PyErr_SetString(PyExc_OverflowError, "base string is too long");
        goto error;
    }
    string = PyUnicode_Concat(l_string, r_string);
    if (!string) {goto error;}

    Katakana k = KATAKANA_STR_READ(r_string, 0);
    if (!small_kana_vowel(k)) {
        mora_cnt = l_mora_cnt + r_mora_cnt;
        if ((size_t)mora_cnt != length) {
            indices = MoraStr_INDICES_ALLOC(mora_cnt);
            if (!indices) {goto error;}
            INDICES_FILL_COPY(indices, 
                MoraStr_INDICES(self), l_mora_cnt, 0);
            INDICES_FILL_COPY(indices + l_mora_cnt,
                MoraStr_INDICES(other), r_mora_cnt, MINDEX(l_length));
        }
    } else {
        mora_cnt = count_morae(string, length, &indices);
        if (mora_cnt == -1) {goto error;}
        if (mora_cnt != l_mora_cnt + r_mora_cnt) {
            PyErr_SetString(PyExc_ValueError, "mora length inconsistency");
            goto error;
        }
    }
    MoraStrObject *new_morastr;
    new_morastr = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!new_morastr) {goto error;}

    Py_SET_SIZE(new_morastr, mora_cnt);
    new_morastr->string = string;
    new_morastr->indices = indices;
    Py_DECREF(other);
    return (PyObject *)new_morastr;
}

error:
    Py_DECREF(other);
    Py_XDECREF(string);
    MoraStr_INDICES_DEL(indices);
    return NULL;
}


static PyObject *
MoraStr_repeat(MoraStrObject *self, Py_ssize_t n) {
    PyTypeObject *type = Py_TYPE(self);
    Py_ssize_t mora_cnt = Py_SIZE(self);
    PyObject *morastr;
    if (n < 0) {n = 0;}
    if (IS_MORASTR_TYPE(type)) {
        if (!n || !mora_cnt) {return Empty_MoraStr();}
        if (n == 1) {
            Py_INCREF(self);
            return (PyObject *)self;
        }
    } else {
        if (!n || !mora_cnt) {
            PyObject *empty_str = PyUnicode_New(0, 0);
            if (!empty_str) {return NULL;}
            morastr = MoraStr_subtype_call(type, empty_str);
            Py_DECREF(empty_str);
            return morastr;
        }
        if (n == 1) {
            return MoraStr_subtype_call(type, MoraStr_STRING(self));
        }
    }

    PyObject *string;
    long long length, new_length;

    string = MoraStr_STRING(self);
    length = PyUnicode_GET_LENGTH(string);
    if (n > MINDEX_MAX || (new_length = length * n) > MINDEX_MAX) {
        PyErr_SetString(PyExc_OverflowError, "base string is too long");
        return NULL;
    }

    Py_ssize_t new_mora_cnt;
    PyObject *new_string = PySequence_Repeat(string, n);
    if (!new_string) {return NULL;}
    MINDEX_T *new_indices = NULL;
{ /* got ownership */
    Katakana k = KATAKANA_STR_READ(new_string, 0);
    new_mora_cnt = mora_cnt * n;
    if (!small_kana_vowel(k)) {
        MINDEX_T *indices = MoraStr_INDICES(self);
        if (indices) {
            new_indices = MoraStr_INDICES_ALLOC(new_mora_cnt);
            if (!new_indices) {goto error;}
            for (Py_ssize_t i = 0; i < n; ++i) {
                INDICES_FILL_COPY(new_indices + i * mora_cnt,
                    indices, mora_cnt, MINDEX(i * length));
            }
        }
    } else {
        mora_cnt = count_morae(
            new_string, (Py_ssize_t)new_length, &new_indices);
        if (mora_cnt == -1) {goto error;}
        if (mora_cnt != new_mora_cnt) {
            PyErr_SetString(PyExc_ValueError, "mora length inconsistency");
            goto error;
        }
    }
    MoraStrObject *new_morastr;
    new_morastr = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!new_morastr) {goto error;}

    Py_SET_SIZE(new_morastr, new_mora_cnt);
    new_morastr->string = new_string;
    new_morastr->indices = new_indices;
    return (PyObject *)new_morastr;
}

error:
    Py_DECREF(new_string);
    MoraStr_INDICES_DEL(new_indices);
    return NULL;
}


static inline PyObject *
MoraStr_Item(MoraStrObject *self, Py_ssize_t i) {
    PyObject *string = MoraStr_STRING(self);
    MINDEX_T *indices = MoraStr_INDICES(self);
    assert(PyUnicode_Check(string));

    if (!indices) {
        Katakana k = KATAKANA_STR_READ(string, i);
        return PyUnicode_FromOrdinal(k);
    }

    Py_ssize_t s_start = i ? indices[i-1] : 0LL, s_end = indices[i];
    const Katakana *buf = KatakanaArray_from_str(string);
    return PyUnicode_FromKindAndData(
        KATAKANA_KIND, (const void*)(buf+s_start), s_end - s_start);
}


static PyObject *
MoraStr_sq_item(MoraStrObject *self, Py_ssize_t i) {
    if (!( 0 <= i && i < Py_SIZE(self) )) {
        PyErr_SetString(PyExc_IndexError, "MoraStr index out of range");
        return NULL;
    }
    return MoraStr_Item(self, i);
}


static PyObject *
MoraStr___getnewargs__(MoraStrObject *self, PyObject *Py_UNUSED(ignored)) {
    return PyTuple_Pack(1, MoraStr_STRING(self));
}


static PyObject *
MoraStr___reduce__(MoraStrObject *self, PyObject *Py_UNUSED(ignored)) {
    static unaryfunc object_reduce = NULL;

    PyTypeObject *type = Py_TYPE(self);
    if (IS_MORASTR_TYPE(type)) {
        return Py_BuildValue("O(O)", type, MoraStr_STRING(self));
    }
    if (!object_reduce) {
        PyMethodDef *meth = PyBaseObject_Type.tp_methods;
        while (meth->ml_name) {
            if (!strcmp(meth->ml_name, "__reduce__")) {
                object_reduce = (unaryfunc)meth->ml_meth;
                break;
            }
            ++meth;
        }
        if (!object_reduce) {
            PyErr_SetString(PyExc_AttributeError,
                "type object 'object' has no attribute '__reduce__'");
            return NULL;
        }
    }
    return object_reduce((PyObject *)self);
}


typedef union {
    Katakana chs[2];
    const Katakana *str;
} KanaPattern;


static inline void
KanaPattern_init(
    KanaPattern *kp, const Katakana *p, Py_ssize_t len)
{
    switch (len) {
        case 1: *kp = (KanaPattern){0}; kp->chs[0] = p[0]; break;
        case 2: kp->chs[0] = p[0]; kp->chs[1] = p[1]; break;
        default: kp->str = p;
    }
}

static inline bool
KanaPattern_str_eq(
    const Katakana *p, const Katakana *s, Py_ssize_t len)
{
    MoraStr_assert(len > 0);
    do {
        if (*p++ != *s++) {return false;}
    } while (--len);
    return true;
}

static inline bool
KanaPattern_eq(
    const KanaPattern *kp, const Katakana *s, Py_ssize_t len)
{
    return (
        len <= 2 ? (s[0] == kp->chs[0] && (len == 1 || s[1] == kp->chs[1])) :
        KanaPattern_str_eq(kp->str, s, len)
    );
}


static unsigned char katakana_hash_[96] = {
// "゠" "ァ" "ア" "ィ" "イ" "ゥ" "ウ" "ェ" "エ" "ォ" "オ" "カ" "ガ" "キ" "ギ" "ク"
    0,  0,  1,   2,  3,  4,  5,  6,  7,  8,  9,  10, 10, 11,  11, 12,
// "グ" "ケ" "ゲ" "コ" "ゴ" "サ" "ザ" "シ" "ジ" "ス" "ズ" "セ" "ゼ" "ソ" "ゾ" "タ"
    12, 13, 13, 14, 14,  15, 15, 16, 63, 17,  17, 18, 18, 19,  19, 20,
// "ダ" "チ" "ヂ" "ッ" "ツ" "ヅ" "テ" "デ" "ト" "ド" "ナ" "ニ" "ヌ" "ネ" "ノ" "ハ"
    20, 21, 63, 22, 23, 17, 24, 24, 25, 25,  26, 27, 28, 29, 30, 31,
// "バ" "パ" "ヒ" "ビ" "ピ" "フ" "ブ" "プ" "ヘ" "ベ" "ペ" "ホ" "ボ" "ポ" "マ" "ミ"
    32, 32, 33, 34, 34, 35, 36, 36, 37,  38, 38, 39, 40,  40, 41, 42,
// "ム" "メ" "モ" "ャ" "ヤ" "ュ" "ユ" "ョ" "ヨ" "ラ" "リ" "ル" "レ" "ロ" "ヮ" "ワ"
    43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 57,
// "ヰ" "ヱ" "ヲ" "ン" "ヴ" "ヵ" "ヶ" "ヷ" "ヸ" "ヹ" "ヺ" "・" "ー" "ヽ" "ヾ" "ヿ"
    3,  7,  9,  58, 59, 10, 13, 0,   2,  6,  8,   60, 61, 62, 62, 0
};

#define KATAKANA_HASH64(key) (1ULL << katakana_hash_[KANA_ID(key)])


static Py_ssize_t
katakana_mora_rev_search_x(
    const Katakana *s, Py_ssize_t s_len, Py_ssize_t s_moracnt,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t p_moracnt,
    Py_ssize_t mora_off, const MINDEX_T *indices)
{
    MoraStr_assert(p_len > 0);
    Py_ssize_t end = mora_off ? indices[mora_off-1] : 0LL;
    if (p_len > s_len - end) {return -1;}

    Py_ssize_t last_idx = p_len - 1;
    Katakana first = p[0];
    uint64_t mask = KATAKANA_HASH64(first);
    Py_ssize_t gap = last_idx;
    for (Py_ssize_t i = last_idx; i; --i) {
        Katakana k = p[i];
        mask |= KATAKANA_HASH64(k);
        if (k == first) {
            gap = i - 1;
        }
    }

    Py_ssize_t m_idx = s_moracnt - p_moracnt - 1;
    Py_ssize_t i = m_idx < 0 ? 0LL : indices[m_idx];
    while (i >= end) {
        if (s[i] == first) {
            if (indices[m_idx+p_moracnt] != MINDEX(i + p_len)) {
                --m_idx;
                i = m_idx < 0 ? m_idx+1 : indices[m_idx];
                continue;
            }
            for (Py_ssize_t j = 1; j < p_len; ++j) {
                if (s[i+j] != p[j]) {goto mismatch;}
            }
            return m_idx + 1;

        mismatch:
            --m_idx;
            i = m_idx < 0 ? m_idx+1 : indices[m_idx];
            if (i < end) {break;}
            if (!(KATAKANA_HASH64(s[i]) & mask)) {
                i -= p_len;
            } else if (gap) {
                i -= gap;
            } else {
                continue;
            }
        } else {
            --m_idx;
            i = m_idx < 0 ? m_idx+1 : indices[m_idx];
            if (i < end) {break;}
            if (!(KATAKANA_HASH64(s[i]) & mask)) {
                i -= p_len;
            } else {
                continue;
            }
        }
        if (i < end) {break;}
        Py_ssize_t imin = i;
        do {
            --m_idx;
            i = m_idx < 0 ? m_idx+1 : indices[m_idx];
        } while (i > imin);
    };

    return -1;
}


static inline Py_ssize_t
katakana_mora_rev_search(
    const Katakana *s, Py_ssize_t s_len, Py_ssize_t s_moracnt,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t p_moracnt,
    Py_ssize_t mora_off, const MINDEX_T *indices)
{
    MoraStr_assert(p_len > 0);

    if (p_len <= 2) {
        Py_ssize_t end = mora_off ? indices[mora_off-1] : 0LL;
        if (p_len > s_len - end) {return -1;}
        Py_ssize_t m_idx = s_moracnt - p_moracnt - 1;
        Py_ssize_t i = m_idx < 0 ? 0LL : indices[m_idx];
        KanaPattern kp;
        KanaPattern_init(&kp, p, p_len);
        while (i >= end) {
            if (KanaPattern_eq(&kp, s+i, p_len) &&
                indices[m_idx+p_moracnt] == MINDEX(i + p_len))
            {
                return m_idx + 1;
            } else {
                --m_idx;
                i = m_idx < 0 ? m_idx+1 : indices[m_idx];
            }
        }
        return -1;
    }
    return katakana_mora_rev_search_x(
        s, s_len, s_moracnt, p, p_len, p_moracnt, mora_off, indices);
}

#define BITAP_TABLE_SIZE KATAKANA_RNG
#define CHAR_INDEX(ch) KANA_ID(ch)

#define BITAP_UINT_T uint32_t
#include "cmorastr_bitap.h"
#undef BITAP_UINT_T

#define BITAP_UINT_T uint64_t
#include "cmorastr_bitap.h"
#undef BITAP_UINT_T

#undef BITAP_TABLE_SIZE

#define TWOWAY_TABLE_SIZE KATAKANA_RNG
#define TWOWAY_SSIZE_T MINDEX_T
#define TWOWAY_SSIZE_MAX MINDEX_MAX
#include "cmorastr_twoway.c"
#undef TWOWAY_TABLE_SIZE
#undef TWOWAY_SSIZE_T
#undef TWOWAY_SSIZE_MAX

#undef CHAR_INDEX


#define SEARCH_DEFAULT 0
#define SEARCH_TWOWAY 1
#define SEARCH_EXHAUSTIVE 2
#define SEARCH_BITAP 3
#define SEARCH_BITAP64 4
#define SEARCH_ADAPTIVE 5

#define MoraStr_ALGORITHM SEARCH_DEFAULT


static inline int
select_search_algorithm(
    Py_ssize_t s_len, Py_ssize_t p_len, Py_ssize_t offset,
    bool using_mora_search)
{
    return (
#if   MoraStr_ALGORITHM == SEARCH_EXHAUSTIVE
        SEARCH_EXHAUSTIVE
#elif MoraStr_ALGORITHM == SEARCH_BITAP
        p_len <= 32 ? SEARCH_BITAP :
        p_len <= 64 ? SEARCH_BITAP64 :
        SEARCH_EXHAUSTIVE
#elif MoraStr_ALGORITHM == SEARCH_BITAP64
        p_len <= 64 ? SEARCH_BITAP64 :
        SEARCH_EXHAUSTIVE
#elif MoraStr_ALGORITHM == SEARCH_TWOWAY
        SEARCH_TWOWAY
#else
        ( !using_mora_search ?
            p_len <= 2 || s_len - offset <= 16 :
            p_len <= 3 || s_len - offset <= 24 ) ? SEARCH_EXHAUSTIVE :
        p_len <= 32 ? SEARCH_BITAP :
#if defined(__LP64__) || defined(_WIN64)
        p_len <= 64 ? SEARCH_BITAP64 :
#endif
        s_len - p_len > 3 && SEARCH_TWOWAY ? SEARCH_TWOWAY :
        SEARCH_EXHAUSTIVE
#endif
    );
}

enum {USING_KANA_SEARCH = 0, USING_MORA_SEARCH = 1};

static Py_ssize_t
generic_katakana_search_x(
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count)
{
    MoraStr_assert(p_len > 0 && s_len >= 0 && count != 0);

    int algorithm = select_search_algorithm(
        s_len, p_len, mora_off, USING_KANA_SEARCH);

    if (algorithm == SEARCH_TWOWAY) {
        return two_way_search(
            s, s_len, p, p_len, mora_off, count);
    }
    if (algorithm == SEARCH_BITAP) {
        return bitap_search_uint32_t(
            s, s_len, p, p_len, mora_off, count);
    }
    if (algorithm == SEARCH_BITAP64) {
        return bitap_search_uint64_t(
            s, s_len, p, p_len, mora_off, count);
    }
    MoraStr_assert(algorithm == SEARCH_EXHAUSTIVE);
    
    s += mora_off; s_len -= mora_off;
    Py_ssize_t i = 0, limit = s_len - p_len + 1;
    while (i < limit && p[0] != s[i]) {++i;}
    if (i >= limit) {return count;}
    Py_ssize_t rem = count;
    KanaPattern kp;
    KanaPattern_init(&kp, p, p_len);
    do {
        if (KanaPattern_eq(&kp, s+i, p_len)) {
            if (count == -1) {
                return mora_off + i;
            } else {
                if (!(--rem)) {break;}
                i += p_len;
            }
        } else {
            ++i;
        }
    } while (i < limit);
    return rem;
}

static inline Py_ssize_t
generic_katakana_search(
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count)
{
    if (CHAR_BIT == 8 && p_len == 1 && count == -1) {
        s += mora_off; s_len -= mora_off;
        unsigned char c = p[0] & 0xff;
        void *r = memchr((const void *)s, c, sizeof(Katakana)*s_len);
        if (!r) {return -1;}
        count = (const Katakana *)ALIGN_DOWN(r, sizeof(Katakana)) - s;
        return mora_off + count;
    }
    if (!count) {return 0;}
    return generic_katakana_search_x(s, s_len, p, p_len, mora_off, count);
}


static Py_ssize_t
generic_mora_search_x(
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices, Py_ssize_t count)
{
    MoraStr_assert(p_len > 0 && s_len >= 0 && count != 0);

    int algorithm = select_search_algorithm(
        s_len, p_len,
        mora_off ? indices[mora_off-1] : 0LL,
        USING_MORA_SEARCH);

    if (algorithm == SEARCH_TWOWAY) {
        return two_way_mora_search(
            s, s_len, p, p_len,
            mora_off, p_moracnt, indices, count);
    }
    if (algorithm == SEARCH_BITAP) {
        return bitap_mora_search_uint32_t(
            s, s_len, p, p_len,
            mora_off, p_moracnt, indices, count);
    }
    if (algorithm == SEARCH_BITAP64) {
        return bitap_mora_search_uint64_t(
            s, s_len, p, p_len,
            mora_off, p_moracnt, indices, count);
    }
    MoraStr_assert(algorithm == SEARCH_EXHAUSTIVE);

    Py_ssize_t i = mora_off ? indices[mora_off-1] : 0LL;
    Py_ssize_t limit = s_len - p_len + 1;
    if (i >= limit) {return count;}

    const MINDEX_T *next_ptr = indices + mora_off;
    Py_ssize_t imax;
#if CHAR_BIT == 8
    unsigned char c = p[0] & 0xff;
    void *r = memchr(s+i, c, sizeof(Katakana)*(limit - i));
    if (!r) {return count;}
    imax = (const Katakana *)ALIGN_DOWN(r, sizeof(Katakana)) - s;
    while (i < imax) {i = *next_ptr++;}
    if (i >= limit) {return count;}
#endif
    Py_ssize_t rem = count;
    KanaPattern kp;
    KanaPattern_init(&kp, p, p_len);
    do {
        if (KanaPattern_eq(&kp, s+i, p_len) &&
            next_ptr[p_moracnt-1] == MINDEX(i + p_len))
        {
            if (count == -1) {
                return next_ptr - indices;
            } else {
                if (!(--rem)) {break;}
                i += p_len;
                if (i >= limit) {break;}
                imax = i;
                do {i = *next_ptr++;} while (i < imax);
            }
        } else {
            i = *next_ptr++;
        }
    } while (i < limit);
    return rem;
}

static inline Py_ssize_t
generic_mora_search(
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices, Py_ssize_t count)
{
    if (!count) {return 0;}
    return generic_mora_search_x(
        s, s_len, p, p_len, mora_off, p_moracnt, indices, count);
}


static inline int
search_algorithm_prepare(
    const Katakana *Py_UNUSED(s), Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices)
{
    int algorithm = select_search_algorithm(
        s_len, p_len,
        !indices ? mora_off : mora_off ? indices[mora_off-1] : 0LL,
        (bool)indices);

    if (algorithm == SEARCH_TWOWAY) {
        if (two_way_prepare(p, p_len, p_moracnt)) {
            return 1;
        }
        PyErr_SetString(PyExc_OverflowError,
            "substring is too long");
        return -1;
    }
    return 0;
}

static inline void
search_algorithm_disable_cache(void) {
    two_way_set_needle(NULL);
}


static inline PyObject *
parse_submora(PyObject *submora,
        Py_ssize_t *cnt, Py_ssize_t *len, const char *err_fmt)
{
    Py_ssize_t submora_cnt, substr_len;
    PyObject *substr;
    if (PyUnicode_Check(submora)) {
        substr = normalize_text(submora, true);
        if (!substr) {
            *cnt = *len = -1;
            return NULL;
        }
        substr_len = PyUnicode_GET_LENGTH(substr);
        if (substr_len) {
            submora_cnt = count_morae_wo_indices(substr, substr_len);
            if (submora_cnt == -1) {
                Py_DECREF(substr);
                *cnt = *len = -1;
                return NULL;
            }
        } else {
            Py_DECREF(substr);
            *cnt = *len = 0;
            return NULL;
        }
    } else if (MoraStr_Check(submora)) {
        submora_cnt = Py_SIZE(submora);
        if (!submora_cnt) {
            *cnt = *len = 0;
            return NULL;
        }
        substr = MoraStr_STRING(submora);
        substr_len = PyUnicode_GET_LENGTH(substr);
        Py_INCREF(substr);
    } else {
        PyErr_Format(PyExc_TypeError,
            err_fmt, Py_TYPE(submora)->tp_name);
        *cnt = *len = -1;
        return NULL;
    }
    *cnt = submora_cnt; *len = substr_len;
    return substr;
}


static int
MoraStr_contains(MoraStrObject *self, PyObject *submora) {
    static const char *err_fmt = \
        "string requeired as left operand, not %.200s";

    Py_ssize_t submora_cnt, substr_len;
    PyObject *substr;

    substr = parse_submora(submora, &submora_cnt, &substr_len, err_fmt);
    if (!substr) {return submora_cnt ? -1 : 1;}

    PyObject *string = MoraStr_STRING(self);
    Py_ssize_t len = PyUnicode_GET_LENGTH(string);
    if (len < substr_len) {
        Py_DECREF(substr);
        return 0;
    }

    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t result;
    if (!indices && submora_cnt != substr_len) {
        Py_DECREF(substr);
        return 0;
    }
    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);

    if (!indices) {
        result = generic_katakana_search(s, len, p, substr_len, 0, -1);
    } else {
        result = generic_mora_search(
            s, len, p, substr_len, 0, submora_cnt, indices, -1);
    }
    Py_DECREF(substr);
    return result != -1;
}


static Py_ssize_t
MoraStr_findindex(MoraStrObject *self, PyObject *submora,
        Py_ssize_t start, Py_ssize_t end)
{
    static const char *err_fmt = \
        "argument 1 must be a kana string or a MoraStr object, not '%.200s'";

    BoolPred charwise = false;
    if (start == -1) {
        charwise = true; start = 0;
    }
    Py_ssize_t submora_cnt, substr_len;
    PyObject *substr;

    substr = parse_submora(submora, &submora_cnt, &substr_len, err_fmt);
    if (!substr) {return submora_cnt ? -2 : start;}

    PyObject *string = MoraStr_STRING(self);
    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t len;
    if (!indices) {
        if (submora_cnt != substr_len) {
            Py_DECREF(substr);
            return -1;
        }
        len = end;
    } else {
        len = end ? indices[end-1] : end;
    }
    if (len < substr_len) {
        Py_DECREF(substr);
        return -1;
    }
    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);

    Py_ssize_t result;
    if (!indices) {
        result = generic_katakana_search(
            s, len, p, substr_len, start, -1);
    } else {
        result = generic_mora_search(
            s, len, p, substr_len, start,
            submora_cnt, indices, -1);
        if (charwise && 0 < result) {
            result = indices[result-1];
        }
    }
    Py_DECREF(substr);
    return result;
}


static Py_ssize_t
MoraStr_rfindindex(MoraStrObject *self, PyObject *submora,
        Py_ssize_t start, Py_ssize_t end)
{
    static const char *err_fmt = \
        "argument 1 must be a kana string or a MoraStr object, not '%.200s'";

    BoolPred charwise = false;
    if (start == -1) {
        charwise = true; start = 0;
    }
    Py_ssize_t submora_cnt, substr_len;
    PyObject *substr;

    substr = parse_submora(submora, &submora_cnt, &substr_len, err_fmt);
    if (!substr) {return submora_cnt ? -2 : end;}

    PyObject *string = MoraStr_STRING(self);
    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t len;
    if (!indices) {
        if (submora_cnt != substr_len) {
            Py_DECREF(substr);
            return -1;
        }
        len = end;
    } else {
        len = end ? indices[end-1] : end;
    }
    if (len < substr_len) {
        Py_DECREF(substr);
        return -1;
    }

    Py_ssize_t result;
    if (!indices) {
        if (substr_len == 1) {
            Py_UCS4 ch = (Py_UCS4)KATAKANA_STR_READ(substr, 0);
            result = PyUnicode_FindChar(
                string, ch, start, end, -1);
        } else {
            result = PyUnicode_Find(
                string, substr, start, end, -1);
        }
    } else {
        const Katakana *s = KatakanaArray_from_str(string);
        const Katakana *p = KatakanaArray_from_str(substr);

        result = katakana_mora_rev_search(
            s, len, end, p, substr_len, submora_cnt,
            start, indices);
        if (charwise && 0 < result) {
            result = indices[result-1];
        }
    }
    Py_DECREF(substr);
    return result;
}


static Py_ssize_t
MoraStr_Count(MoraStrObject *self, PyObject *submora,
        Py_ssize_t start, Py_ssize_t end)
{
    static const char *err_fmt = \
        "argument 1 must be a kana string or a MoraStr object, not '%.200s'";

    Py_ssize_t submora_cnt, substr_len;
    PyObject *substr;

    substr = parse_submora(submora, &submora_cnt, &substr_len, err_fmt);
    switch (submora_cnt) {
        case -1: return -1;
        case 0: return end - start + 1;
        default: break;
    }

    PyObject *string = MoraStr_STRING(self);
    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t len;
    if (!indices) {
        if (submora_cnt != substr_len) {
            Py_DECREF(substr);
            return 0;
        }
        len = end;
    } else {
        len = end ? indices[end-1] : end;
    }
    if (len < substr_len) {
        Py_DECREF(substr);
        return 0;
    }
    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);

    Py_ssize_t result;
    if (!indices) {
        result = generic_katakana_search(
            s, len, p, substr_len, start, PY_SSIZE_T_MAX);
    } else {
        result = generic_mora_search(
            s, len, p, substr_len, start,
            submora_cnt, indices, PY_SSIZE_T_MAX);
    }
    Py_DECREF(substr);
    return PY_SSIZE_T_MAX - result;
}


static PyObject *
MoraStr_count(MoraStrObject *self, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *submora;
    Py_ssize_t length, start = 0, end = PY_SSIZE_T_MAX;

    if (MoraStr_VALIDATE_NUM_ARGS("count", 1, 3, nargs) == -1) {
        return NULL;
    };
    submora = args[0];
    switch (nargs) {
        case 3:
            if (!MoraStr_SliceIndex(args[2], &end)) {return NULL;}
        case 2:
            if (!MoraStr_SliceIndex(args[1], &start)) {return NULL;}
        case 1:
            break;
    }
    length = Py_SIZE(self);
    if (length < start) {return PyLong_FromLong(0);}
    PySlice_AdjustIndices(length, &start, &end, 1);

    Py_ssize_t result = MoraStr_Count(self, submora, start, end);
    if (result == -1) {return NULL;}
    return PyLong_FromSsize_t(result);
}


static PyObject *
MoraStr_find(MoraStrObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "", "", "charwise", NULL};

    PyObject *submora, *arg_x = NULL, *arg_y = NULL;
    BoolPred charwise = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|OO$p", kwlist,
            &submora, &arg_x, &arg_y, &charwise)) {return NULL;}

    if (kwds && arg_x) {
        PyErr_SetString(PyExc_TypeError,
            "keyword argument 'charwise' is available only when "
            "start/end arguments are not specified");
        return NULL;
    }

    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    if (arg_x) {
        if (!MoraStr_SliceIndex(arg_x, &start)) {return NULL;}
    }
    if (arg_y) {
        if (!MoraStr_SliceIndex(arg_y, &end)) {return NULL;}
    }

    Py_ssize_t length = Py_SIZE(self);
    if (length < start) {return PyLong_FromLong(-1);}
    PySlice_AdjustIndices(length, &start, &end, 1);

    if (charwise) {start = -1;}
    Py_ssize_t result = MoraStr_findindex(self, submora, start, end);
    if (result == -2) {return NULL;}
    return PyLong_FromSsize_t(result);
}


static PyObject *
MoraStr_index(MoraStrObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "", "", "charwise", NULL};

    PyObject *submora, *arg_x = NULL, *arg_y = NULL;
    BoolPred charwise = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|OO$p", kwlist,
            &submora, &arg_x, &arg_y, &charwise)) {return NULL;}

    if (kwds && arg_x) {
        PyErr_SetString(PyExc_TypeError,
            "keyword argument 'charwise' is available only when "
            "start/end arguments are not specified");
        return NULL;
    }

    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    if (arg_x) {
        if (!MoraStr_SliceIndex(arg_x, &start)) {return NULL;}
    }
    if (arg_y) {
        if (!MoraStr_SliceIndex(arg_y, &end)) {return NULL;}
    }

    Py_ssize_t length = Py_SIZE(self);
    if (length < start) {goto notfound;}
    PySlice_AdjustIndices(length, &start, &end, 1);

    if (charwise) {start = -1;}
    Py_ssize_t result = MoraStr_findindex(self, submora, start, end);
    if (result == -2) {return NULL;}
    if (result == -1) {goto notfound;}
    return PyLong_FromSsize_t(result);

notfound:
    PyErr_SetString(PyExc_ValueError, "submora-string not found");
    return NULL;
}


static PyObject *
MoraStr_removeprefix(MoraStrObject *self, PyObject *prefix) {
    static const char *err_fmt = \
        "removeprefix() argument must be str or MoraStr, "
        "not %.100s";
    static PyTypeObject *type = &MoraStrType;

    PyObject *substr;
    Py_ssize_t submora_cnt, substr_len, match;

    substr = parse_submora(
        prefix, &submora_cnt, &substr_len, err_fmt);
    if (submora_cnt == -1) {return NULL;}
    if (!submora_cnt) {goto unchanged;}

    PyObject *string = MoraStr_STRING(self);
    match = PyUnicode_Tailmatch(string, substr, 0, substr_len, -1);
    Py_DECREF(substr);
    if (match == -1) {return NULL;}
    if (!match) {goto unchanged;}
    MINDEX_T *indices = MoraStr_INDICES(self);
    if (indices && indices[submora_cnt-1] != substr_len) {
        goto unchanged;
    }
    return MoraStr_SubMoraStr(self, submora_cnt, Py_SIZE(self));

unchanged:
    if (MoraStr_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    return MoraStr_copy_(type, self);
}


static PyObject *
MoraStr_removesuffix(MoraStrObject *self, PyObject *suffix) {
    static const char *err_fmt = \
        "removesuffix() argument must be str or MoraStr, "
        "not %.100s";
    static PyTypeObject *type = &MoraStrType;

    PyObject *substr, *string;
    Py_ssize_t submora_cnt, substr_len, mora_cnt, len, match;

    substr = parse_submora(
        suffix, &submora_cnt, &substr_len, err_fmt);
    if (submora_cnt == -1) {return NULL;}
    if (!submora_cnt) {goto unchanged;}

    mora_cnt = Py_SIZE(self);
    string = MoraStr_STRING(self);
    len = PyUnicode_GET_LENGTH(string);
    match = PyUnicode_Tailmatch(string, substr, 0, len, +1);
    Py_DECREF(substr);
    if (match == -1) {return NULL;}
    if (!match) {goto unchanged;}

    MINDEX_T *indices = MoraStr_INDICES(self);
    Py_ssize_t tail = mora_cnt - submora_cnt;
    if (indices && (tail ? indices[tail-1]: 0LL) != len - substr_len) {
        goto unchanged;
    }
    return MoraStr_SubMoraStr(self, 0, tail);

unchanged:
    if (MoraStr_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    return MoraStr_copy_(type, self);
}


static PyObject *
MoraStr_Replace(
    MoraStrObject *self, PyObject *old, PyObject *new, Py_ssize_t count)
{
    static const char *err_fmt = \
        "argument 1 must be a kana string or a MoraStr object, not '%.200s'";
    static PyTypeObject *type = &MoraStrType;

    if (!count) {
        if (MoraStr_CheckExact(self)) {
            Py_INCREF(self);
            return (PyObject *)self;
        }
        return MoraStr_copy_(type, self);
    }
    if (count < 0) {count = PY_SSIZE_T_MAX;}

    PyObject *substr = NULL, *rpl_morastr = NULL, *new_string = NULL;
    MINDEX_T *new_indices = NULL;
    MoraStrObject *result;
    Py_ssize_t submora_cnt, substr_len;
    substr = parse_submora(
        old, &submora_cnt, &substr_len, err_fmt);
{ /* got ownership */
    if (submora_cnt == -1) {goto error;}
    if (!substr) {
        substr = PyUnicode_New(0, 0);
        if (!substr) {goto error;}
    }

    PyObject *rplstr;
    Py_ssize_t rplmora_cnt, rplstr_len;
    if (PyUnicode_Check(new)) {
        rpl_morastr = MoraStr_from_unicode_(new, true);
        if (!rpl_morastr) {goto error;}
    } else if (MoraStr_Check(new)) {
        rpl_morastr = new;
        Py_INCREF(rpl_morastr);
    } else {
        PyErr_Format(PyExc_TypeError,
            err_fmt, Py_TYPE(new)->tp_name);
        goto error;
    }
    rplmora_cnt = Py_SIZE(rpl_morastr);
    rplstr = MoraStr_STRING(rpl_morastr);
    rplstr_len = PyUnicode_GET_LENGTH(rplstr);
    const Katakana *r = KatakanaArray_from_str(rplstr);

    if (rplstr_len && small_kana_vowel(r[0])) {
        PyErr_SetString(PyExc_ValueError,
            "replacer must not start with a small kana");
        goto error;
    }
    Py_ssize_t mora_cnt = Py_SIZE(self);
    PyObject *string = MoraStr_STRING(self);
    Py_ssize_t len = PyUnicode_GET_LENGTH(string);
    if (substr == rplstr || len < substr_len) {
        goto unchanged;
    }

    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);
    MINDEX_T *indices = MoraStr_INDICES(self);

    MoraStr_assert(substr_len <= MINDEX_MAX);
    if (search_algorithm_prepare(
        s, len, p, substr_len, 0, submora_cnt, indices) < 0) {goto error;}

    if (substr_len == rplstr_len && submora_cnt == rplmora_cnt) {
        if (!submora_cnt) {goto unchanged;}

        Py_ssize_t s_prev, s_idx, m_idx;
        if (!indices) {
            if (submora_cnt != substr_len) {goto unchanged;}
            m_idx = s_idx = generic_katakana_search(
                s, len, p, substr_len, 0, -1);
        } else {
            m_idx = generic_mora_search(
                s, len, p, substr_len, 0,
                submora_cnt, indices, -1);
            s_idx = m_idx > 0 ? indices[m_idx-1] : m_idx;
        }
        if (s_idx == -1) {goto unchanged;}
        new_string = PyUnicode_New(len, 0xffff);
        if (!new_string) {goto error;}
        Katakana *t = KatakanaArray_from_str(new_string);

        memcpy(t, s, sizeof(Katakana)*len);
        Py_MEMCPY(t+s_idx, r, sizeof(Katakana)*substr_len);

        int v = VOWEL_FROM_KATAKANA(r[substr_len-1]);
        MoraStr_assert(v >= 0);

        if (len == mora_cnt) {
            s_prev = s_idx + substr_len; //
            while (s_prev < len && --count) {
                s_idx = generic_katakana_search(
                    s, len, p, substr_len, s_prev, -1);
                if (s_idx == -1) {break;}
                if (s_prev != s_idx) {
                    if (VALIDATE_MORA_BOUNDARY(
                        Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                }
                Py_MEMCPY(t+s_idx, r, sizeof(Katakana)*substr_len);
                s_prev = s_idx + substr_len;
            }
            if (s_prev < len) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
            }
        } else {
            new_indices = MoraStr_INDICES_ALLOC(mora_cnt);
            if (!new_indices) {goto error;}

            MINDEX_T *r_indices = MoraStr_INDICES(rpl_morastr);
            INDICES_FILL_COPY(new_indices,
                indices, mora_cnt, 0);
            if (r_indices) {
                INDICES_FILL_COPY(new_indices+m_idx,
                    r_indices, submora_cnt-1, MINDEX(s_idx));
            }
            s_prev = s_idx + substr_len; //
            m_idx += submora_cnt;
            while (s_prev < len && --count) {
                m_idx = generic_mora_search(
                    s, len, p, substr_len, m_idx,
                    submora_cnt, indices, -1);
                s_idx = m_idx > 0 ? indices[m_idx-1] : m_idx;
                if (s_idx == -1) {break;}
                if (s_prev != s_idx) {
                    if (VALIDATE_MORA_BOUNDARY(
                        Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                }
                Py_MEMCPY(t+s_idx, r, sizeof(Katakana)*substr_len);
                if (r_indices) {
                    INDICES_FILL_COPY(new_indices+m_idx,
                        r_indices, submora_cnt-1, MINDEX(s_idx));
                }
                s_prev = s_idx + substr_len;
                m_idx += submora_cnt;
            }
            if (s_prev < len) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
            }
        }
        result = (MoraStrObject *) type->tp_alloc(type, 0);
        if (!result) {goto error;}
        Py_SET_SIZE(result, mora_cnt);
        result->string = new_string;
        result->indices = new_indices;
        goto done;
    }

    Py_ssize_t new_mora_cnt, new_len;
    long long n;
    if (!submora_cnt) {
        n = Py_MIN(mora_cnt + 1, count);
    } else {
        if (!indices) {
            if (submora_cnt != substr_len) {goto unchanged;}
            n = count - generic_katakana_search(
                s, len, p, substr_len, 0, count);
        } else {
            n = count - generic_mora_search(
                s, len, p, substr_len, 0,
                submora_cnt, indices, count);
        }
        if (!n) {goto unchanged;}
    }

    MoraStr_assert(n);
    if (substr_len < rplstr_len &&
        rplstr_len - substr_len > (MINDEX_MAX - len) / n)
    {
        PyErr_SetString(PyExc_OverflowError,
            "replace string is too long");
        goto error;
    }
    new_len = len + n * (rplstr_len - substr_len);
    new_mora_cnt = mora_cnt + n * (rplmora_cnt - submora_cnt);

    if (!new_len) {
        result = (MoraStrObject *)Empty_MoraStr();
        goto done;
    }
    new_string = PyUnicode_New(new_len, 0xffff);
    if (!new_string) {goto error;}

    Katakana *t = KatakanaArray_from_str(new_string);
    if (substr_len) {
        int v = -1;
        if (new_len == new_mora_cnt) {
            Py_ssize_t s_prev = 0, s_idx, m_idx = 0;
            MINDEX_T i = 0;
            while (n) {
                if (!indices) {
                    s_idx = generic_katakana_search(
                        s, len, p, substr_len, s_prev, -1);
                } else {
                    m_idx = generic_mora_search(
                        s, len, p, substr_len, m_idx,
                        submora_cnt, indices, -1);
                    s_idx = m_idx > 0 ? indices[m_idx-1] : m_idx;
                    m_idx += submora_cnt;
                }
                if (s_idx == -1) {break;}
                if (s_prev != s_idx) {
                    if (VALIDATE_MORA_BOUNDARY(
                        Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                    Py_MEMCPY(t+i, s+s_prev, sizeof(Katakana)*(s_idx-s_prev));
                    i += MINDEX(s_idx - s_prev);
                }
                if (rplstr_len) {
                    if (v == -1) {v = VOWEL_FROM_KATAKANA(r[rplstr_len-1]);}
                    Py_MEMCPY(t+i, r, sizeof(Katakana)*(rplstr_len));
                    i += MINDEX(rplstr_len);
                } else if (s_idx) {
                    v = VOWEL_FROM_KATAKANA(s[s_idx-1]);
                }
                s_prev = s_idx + substr_len;
                --n;
            }
            if (s_prev < len) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                Py_MEMCPY(t+i, s+s_prev, sizeof(Katakana)*(len-s_prev));
            }
        } else {
            new_indices = MoraStr_INDICES_ALLOC(new_mora_cnt);
            if (!new_indices) {goto error;}
            MINDEX_T *r_indices = MoraStr_INDICES(rpl_morastr);
            Py_ssize_t s_prev = 0, s_idx, m_idx, m_prev = 0;
            MINDEX_T i = 0, j = 0, k = 0;

            while (n) {
                if (!indices) {
                    m_idx = s_idx = generic_katakana_search(
                        s, len, p, substr_len, m_prev, -1);
                } else {
                    m_idx = generic_mora_search(
                        s, len, p, substr_len, m_prev,
                        submora_cnt, indices, -1);
                    s_idx = m_idx > 0 ? indices[m_idx-1] : m_idx;
                }
                if (s_idx == -1) {break;}
                if (s_prev != s_idx) {
                    if (VALIDATE_MORA_BOUNDARY(
                        Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                    Py_MEMCPY(t+i, s+s_prev, sizeof(Katakana)*(s_idx-s_prev));
                    INDICES_FILL_COPY(new_indices+j,
                        indices ? indices+m_prev : NULL,
                        m_idx-m_prev, k);
                    i += MINDEX(s_idx - s_prev);
                    j += MINDEX(m_idx - m_prev);
                }
                if (rplstr_len) {
                    if (v == -1) {v = VOWEL_FROM_KATAKANA(r[rplstr_len-1]);}
                    Py_MEMCPY(t+i, r, sizeof(Katakana)*(rplstr_len));
                    INDICES_FILL_COPY(new_indices+j,
                        r_indices, rplmora_cnt, i);
                    i += MINDEX(rplstr_len);
                    j += MINDEX(rplmora_cnt);
                } else if (s_idx) {
                    v = VOWEL_FROM_KATAKANA(s[s_idx-1]);
                }
                k = indices ? \
                    k + MINDEX(rplstr_len) - MINDEX(substr_len) : i;
                s_prev = s_idx + substr_len;
                m_prev = m_idx + submora_cnt;
                --n;
            }
            if (s_prev < len) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                Py_MEMCPY(t+i, s+s_prev, sizeof(Katakana)*(len-s_prev));
                INDICES_FILL_COPY(new_indices+j,
                    indices ? indices+m_prev : NULL,
                    mora_cnt-m_prev, k);
            }
        }
    } else {
        assert(rplstr_len);
        int v = VOWEL_FROM_KATAKANA(r[rplstr_len-1]);
        if (new_len == new_mora_cnt) {
            Py_ssize_t s_idx = 0;
            MINDEX_T i = 0;
            if (n) {
                Py_MEMCPY(t, r, sizeof(Katakana)*(rplstr_len));
                i += MINDEX(rplstr_len);
                --n;
            }
            while (n) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_idx]) < 0) {goto error;}
                t[i++] = s[s_idx++];
                Py_MEMCPY(t+i, r, sizeof(Katakana)*(rplstr_len));
                i += MINDEX(rplstr_len);
                --n;
            }
            if (s_idx < len){
                Py_MEMCPY(t+i, s+s_idx, sizeof(Katakana)*(len-s_idx));
            }
        } else {
            new_indices = MoraStr_INDICES_ALLOC(new_mora_cnt);
            if (!new_indices) {goto error;}
            MINDEX_T *r_indices = MoraStr_INDICES(rpl_morastr);
            Py_ssize_t s_prev = 0, s_idx, m_idx = 0;
            MINDEX_T i = 0, j = 0, k = 0;
            if (n) {
                Py_MEMCPY(t, r, sizeof(Katakana)*(rplstr_len));
                INDICES_FILL_COPY(new_indices,
                    r_indices, rplmora_cnt, 0);
                i += MINDEX(rplstr_len);
                j += MINDEX(rplmora_cnt);
                k = indices ? k + MINDEX(rplstr_len) : i;
                --n;
            }
            while (n) {
                if (VALIDATE_MORA_BOUNDARY(
                    Bounds_VOWEL, v, s[s_prev]) < 0) {goto error;}
                if (!indices) {
                    t[i++] = s[s_prev++];
                    new_indices[j++] = i;
                    m_idx++;
                } else {
                    s_idx = indices[m_idx++];
                    do {
                        t[i++] = s[s_prev++];
                    } while (s_prev != s_idx);
                    new_indices[j++] = i;
                }
                Py_MEMCPY(t+i, r, sizeof(Katakana)*(rplstr_len));
                INDICES_FILL_COPY(new_indices+j,
                    r_indices, rplmora_cnt, i);
                i += MINDEX(rplstr_len);
                j += MINDEX(rplmora_cnt);
                k = indices ? k + MINDEX(rplstr_len) : i;
                --n;
            }
            if (s_prev < len){
                Py_MEMCPY(t+i, s+s_prev, sizeof(Katakana)*(len-s_prev));
                INDICES_FILL_COPY(new_indices+j,
                    indices ? indices+m_idx : NULL,
                    mora_cnt-m_idx, k);
            }
        }
    }
    result = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!result) {goto error;}
    Py_SET_SIZE(result, new_mora_cnt);
    result->string = new_string;
    result->indices = new_indices;
}

done:
    search_algorithm_disable_cache();
    Py_XDECREF(substr);
    Py_XDECREF(rpl_morastr);
    return (PyObject *)result;

unchanged:
    search_algorithm_disable_cache();
    Py_XDECREF(substr);
    Py_XDECREF(rpl_morastr);
    if (MoraStr_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    return MoraStr_copy_(type, self);

error:
    search_algorithm_disable_cache();
    Py_XDECREF(substr);
    Py_XDECREF(rpl_morastr);
    Py_XDECREF(new_string);
    MoraStr_INDICES_DEL(new_indices);
    return NULL;
}


static PyObject *
MoraStr_replace(
        MoraStrObject *self, PyObject *const *args, Py_ssize_t nargs) {

    PyObject *old, *new;
    Py_ssize_t count = -1;

    if (MoraStr_VALIDATE_NUM_ARGS("replace", 2, 3, nargs) == -1) {
        return NULL;
    };
    old = args[0];
    new = args[1];
    switch (nargs) {
        case 3: {
            PyObject *pyint = PyNumber_Index(args[2]);
            if (!pyint) {return NULL;}
            count = PyLong_AsSsize_t(pyint);
            Py_DECREF(pyint);
            if (count == -1 && PyErr_Occurred()) {return NULL;}
        }
        case 2:
            break;
    }

    return MoraStr_Replace(self, old, new, count);
}


static PyObject *
MoraStr_rfind(MoraStrObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "", "", "charwise", NULL};

    PyObject *submora, *arg_x = NULL, *arg_y = NULL;
    BoolPred charwise = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|OO$p", kwlist,
            &submora, &arg_x, &arg_y, &charwise)) {return NULL;}

    if (kwds && arg_x) {
        PyErr_SetString(PyExc_TypeError,
            "keyword argument 'charwise' is available only when "
            "start/end arguments are not specified");
        return NULL;
    }

    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    if (arg_x) {
        if (!MoraStr_SliceIndex(arg_x, &start)) {return NULL;}
    }
    if (arg_y) {
        if (!MoraStr_SliceIndex(arg_y, &end)) {return NULL;}
    }

    Py_ssize_t length = Py_SIZE(self);
    if (length < start) {return PyLong_FromLong(-1);}
    PySlice_AdjustIndices(length, &start, &end, 1);

    if (charwise) {start = -1;}
    Py_ssize_t result = MoraStr_rfindindex(self, submora, start, end);
    if (result == -2) {return NULL;}
    return PyLong_FromSsize_t(result);
}


static PyObject *
MoraStr_rindex(MoraStrObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "", "", "charwise", NULL};

    PyObject *submora, *arg_x = NULL, *arg_y = NULL;
    BoolPred charwise = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|OO$p", kwlist,
            &submora, &arg_x, &arg_y, &charwise)) {return NULL;}

    if (kwds && arg_x) {
        PyErr_SetString(PyExc_TypeError,
            "keyword argument 'charwise' is available only when "
            "start/end arguments are not specified");
        return NULL;
    }

    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;
    if (arg_x) {
        if (!MoraStr_SliceIndex(arg_x, &start)) {return NULL;}
    }
    if (arg_y) {
        if (!MoraStr_SliceIndex(arg_y, &end)) {return NULL;}
    }

    Py_ssize_t length = Py_SIZE(self);
    if (length < start) {goto notfound;}
    PySlice_AdjustIndices(length, &start, &end, 1);

    if (charwise) {start = -1;}
    Py_ssize_t result = MoraStr_rfindindex(self, submora, start, end);
    if (result == -2) {return NULL;}
    if (result == -1) {goto notfound;}
    return PyLong_FromSsize_t(result);

notfound:
    PyErr_SetString(PyExc_ValueError, "submora-string not found");
    return NULL;
}


static PyObject *
MoraStr_startswith(
        MoraStrObject *self, PyObject *const *args, Py_ssize_t nargs) {
    static const char *tailmatch_tuple_err_fmt = \
        "tuple for startswith must only contain str or MoraStr, "
        "not %.100s";
    static const char *tailmatch_err_fmt = \
        "startswith first arg must be a kana string "
        "or a tuple of kana strings, not %.100s";

    PyObject *subobj;
    Py_ssize_t length, start = 0, end = PY_SSIZE_T_MAX;

    if (MoraStr_VALIDATE_NUM_ARGS("startswith", 1, 3, nargs) == -1) {
        return NULL;
    };
    subobj = args[0];
    switch (nargs) {
        case 3:
            if (!MoraStr_SliceIndex(args[2], &end)) {return NULL;}
        case 2:
            if (!MoraStr_SliceIndex(args[1], &start)) {return NULL;}
        case 1:
            break;
    }
    length = Py_SIZE(self);
    PySlice_AdjustIndices(length, &start, &end, 1);

    Py_ssize_t s_start, s_end;
    MINDEX_T *indices = MoraStr_INDICES(self);
    if (!indices) {
        s_start = start; s_end = end;
    } else {
        s_start = start ? indices[start-1] : 0LL;
        s_end = end ? indices[end-1] : 0LL;
    }

    PyObject *const *objects;
    Py_ssize_t objcnt;
    const char *err_fmt;
    if (PyTuple_Check(subobj)) {
        objects = PySequence_Fast_ITEMS(subobj);
        objcnt = Py_SIZE(subobj);
        err_fmt = tailmatch_tuple_err_fmt;
    } else {
        objects = args;
        objcnt = 1;
        err_fmt = tailmatch_err_fmt;
    }

    PyObject *submora, *substr;
    Py_ssize_t submora_cnt, substr_len;
    Py_ssize_t result;
    for (Py_ssize_t i = 0; i < objcnt; ++i) {
        submora = objects[i];
        substr = parse_submora(
            submora, &submora_cnt, &substr_len, err_fmt);
        if (submora_cnt == -1) {return NULL;}
        if (length < start) {
            Py_XDECREF(substr);
            return Py_NewRef(Py_False);
        }
        if (!submora_cnt) {return Py_NewRef(Py_True);}
        result = PyUnicode_Tailmatch(
            MoraStr_STRING(self), substr, s_start, s_end, -1);
        Py_DECREF(substr);
        switch (result) {
            case -1:
                return NULL;
            case 0:
                break;
            case 1: {
                if (!indices) {return Py_NewRef(Py_True);}
                Py_ssize_t edge = start + submora_cnt;
                if (indices[edge-1] == s_start + substr_len) {
                    return Py_NewRef(Py_True);
                }
                break;
            }
            default: MoraStr_assert(false);
        }
    }
    return Py_NewRef(Py_False);
}


static PyObject *
MoraStr_char_indices(MoraStrObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"zero", NULL};

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (nargs) {
        PyErr_Format(PyExc_TypeError,
            "char_indices() takes 0 positional arguments "
            "but %zu %s given", nargs, nargs == 1 ? "was" : "were");
        return NULL;
    }

    int zero = 0;
    if (kwds) {
        if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|$p", kwlist, &zero)) {return NULL;}
    }

    Py_ssize_t offset = zero != 0;
    Py_ssize_t length = offset + Py_SIZE(self);
    PyObject *result = PyList_New(length);
    if (!result) {return NULL;}
{ /* got ownership */
    if (!length) {return result;}

    PyObject **data = PySequence_Fast_ITEMS(result);
    PyObject *item;
    if (offset) {
        item = PyLong_FromLong(0);
        if (!item) {goto error;}
        *data = item;
    }
    MINDEX_T *indices = MoraStr_INDICES(self);
    PyObject **data_end = data + length;
    if (indices) {
        for (data += offset; data < data_end; data++) {
            item = PyLong_FromLong(*indices++);
            if (!item) {goto error;}
            *data = item;
        }
    } else {
        long j = 0;
        for (data += offset; data < data_end; data++) {
            item = PyLong_FromLong(++j);
            if (!item) {goto error;}
            *data = item;
        }
    }
    return result;
}

error:
    Py_DECREF(result);
    return NULL;
}


static PyObject *
MoraStr_endswith(
        MoraStrObject *self, PyObject *const *args, Py_ssize_t nargs) {
    static const char *tailmatch_tuple_err_fmt = \
        "tuple for endswith must only contain str or MoraStr, "
        "not %.100s";
    static const char *tailmatch_err_fmt = \
        "endswith first arg must be a kana string "
        "or a tuple of kana strings, not %.100s";

    PyObject *subobj;
    Py_ssize_t length, start = 0, end = PY_SSIZE_T_MAX;

    if (MoraStr_VALIDATE_NUM_ARGS("endswith", 1, 3, nargs) == -1) {
        return NULL;
    };
    subobj = args[0];
    switch (nargs) {
        case 3:
            if (!MoraStr_SliceIndex(args[2], &end)) {return NULL;}
        case 2:
            if (!MoraStr_SliceIndex(args[1], &start)) {return NULL;}
        case 1:
            break;
    }
    length = Py_SIZE(self);
    PySlice_AdjustIndices(length, &start, &end, 1);

    Py_ssize_t s_start, s_end;
    MINDEX_T *indices = MoraStr_INDICES(self);
    if (!indices) {
        s_start = start; s_end = end;
    } else {
        s_start = start ? indices[start-1] : 0LL;
        s_end = end ? indices[end-1] : 0LL;
    }

    PyObject *const *objects;
    Py_ssize_t objcnt;
    const char *err_fmt;
    if (PyTuple_Check(subobj)) {
        objects = PySequence_Fast_ITEMS(subobj);
        objcnt = Py_SIZE(subobj);
        err_fmt = tailmatch_tuple_err_fmt;
    } else {
        objects = args;
        objcnt = 1;
        err_fmt = tailmatch_err_fmt;
    }

    PyObject *submora, *substr;
    Py_ssize_t submora_cnt, substr_len;
    Py_ssize_t result;
    for (Py_ssize_t i = 0; i < objcnt; ++i) {
        submora = objects[i];
        substr = parse_submora(
            submora, &submora_cnt, &substr_len, err_fmt);
        if (submora_cnt == -1) {return NULL;}
        if (length < start) {
            Py_XDECREF(substr);
            return Py_NewRef(Py_False);
        }
        if (!submora_cnt) {return Py_NewRef(Py_True);}
        result = PyUnicode_Tailmatch(
            MoraStr_STRING(self), substr, s_start, s_end, +1);
        Py_DECREF(substr);
        switch (result) {
            case -1:
                return NULL;
            case 0:
                break;
            case 1: {
                if (!indices) {return Py_NewRef(Py_True);}
                Py_ssize_t tail = end - submora_cnt;
                if ((tail ? indices[tail-1]: 0LL) == s_end - substr_len) {
                    return Py_NewRef(Py_True);
                }
                break;
            }
            default: MoraStr_assert(false);
        }
    }
    return Py_NewRef(Py_False);
}


static PyObject *
MoraStr_slice_with_step(MoraStrObject *self,
    Py_ssize_t start, Py_ssize_t step, Py_ssize_t slicelength)
{
    MoraStrObject *morastr;
    KWRITER_TYPE *writer = NULL;
    PyObject *string = NULL;
    MINDEX_T *new_indices = NULL;
    PyTypeObject *type = Py_TYPE(self);
{ /* using jump */
    if (!slicelength) {
        if (!IS_MORASTR_TYPE(type)) {
            string = PyUnicode_New(0, 0);
            if (!string) {goto error;}
            goto subclass;
        }
        return Empty_MoraStr();
    }

    PyObject *s_string = MoraStr_STRING(self);
    const Katakana *s = KatakanaArray_from_str(s_string);
    if (step < 0 && small_kana_vowel(s[0])) {
        PyErr_SetString(PyExc_ValueError,
            "Can't instantiate a MoraStr slice; "
            "base string starts with a small kana");
        goto error;
    }

    size_t i; Py_ssize_t j;
    MINDEX_T *indices = MoraStr_INDICES(self);
    if (!indices) {
        string = PyUnicode_New(slicelength, 0xffff);
        if (!string) {goto error;}
        Katakana *t = KatakanaArray_from_str(string);
        Katakana c, pc = s[start];
        for (i = start, j = 0; j < slicelength; i += step, ++j) {
            t[j] = s[i];
        }
        for (j = 1; j < slicelength; ++j) {
            c = t[j];
            if (VALIDATE_MORA_BOUNDARY(Bounds_KANA, pc, c) < 0) {
                goto error;
            }
            pc = c;
        }
        
        if (!IS_MORASTR_TYPE(type)) {goto subclass;}
        morastr = (MoraStrObject *) type->tp_alloc(type, 0);
        if (!morastr) {goto error;}
        Py_SET_SIZE(morastr, slicelength);
        morastr->string = string;
        morastr->indices = NULL;
        return (PyObject *)morastr;
    }

#define WRITE_KANA_INPLACE(WRITER, source, start, end, prev, ERR_LABEL) do { \
    const Katakana *_source = (source); \
    MINDEX_T _start = (start), _end = (end); \
    Katakana c = _source[_start]; \
    if (VALIDATE_MORA_BOUNDARY(Bounds_KANA, *(prev), c) < 0) { \
        goto ERR_LABEL; \
    } \
    while (true) { \
        MoraStr_assert(*(WRITER)); \
        if (KWriter_WriteChar(WRITER, c, 0) == -1) { \
            goto ERR_LABEL; \
        } \
        if (++_start == _end) {break;}  \
        c = _source[_start]; \
    } \
    *(prev) = c; \
} while(0)

    writer = KWriter_NEW();
    if (!KWriter_READY(writer)) {goto error;}
    Py_ssize_t maxlen = PyUnicode_GET_LENGTH(s_string);
    bool overallocate = false;
    if (slicelength < (maxlen+1) / Py_MIN(MORA_CONTENT_MAX, 2)) {
        maxlen = slicelength;
        overallocate = true;
    }
    if (KWriter_INIT(&writer, maxlen, overallocate) == -1) {
        goto error;
    }

    MINDEX_T s_left, s_right;
    s_left = start ? indices[start-1] : 0LL;
    s_right = indices[start];
    Katakana prev = s[s_right-1];

    if (!IS_MORASTR_TYPE(type)) {
        WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);
        for (i = start+step, j = 1; j+1 < slicelength; i += step, ++j) {
            s_left = indices[i-1]; s_right = indices[i];
            WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);
        }
        if (j < slicelength) {
            s_left = i ? indices[i-1] : 0LL; s_right = indices[i];
            WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);
        }
        string = KWriter_Finish(writer);
        if (!string) {
            writer = NULL;
            goto error;
        }
        goto subclass;
    }

    new_indices = MoraStr_INDICES_ALLOC(slicelength);
    if (!new_indices) {goto error;}
    MINDEX_T *iterator = new_indices, *last;
    *iterator = s_right - s_left;
    WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);

    last = iterator + slicelength - 1;
    for (i = start+step; ++iterator < last; i += step) {
        MINDEX_T s_cumsum = *(iterator - 1);
        s_left = indices[i-1]; s_right = indices[i];
        *iterator = s_cumsum + (s_right - s_left);
        WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);
    }
    if (iterator < last + 1) {
        MINDEX_T s_cumsum = *(iterator - 1);
        s_left = i ? indices[i-1] : 0LL; s_right = indices[i];
        *iterator = s_cumsum + (s_right - s_left);
        WRITE_KANA_INPLACE(&writer, s, s_left, s_right, &prev, error);
    }
    if ((MINDEX_T)slicelength == new_indices[slicelength-1]) {
        MoraStr_INDICES_DEL(new_indices);
    }
    string = KWriter_Finish(writer);
    if (!string) {
        writer = NULL;
        goto error;
    }
    morastr = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!morastr) {goto error;}
    Py_SET_SIZE(morastr, slicelength);
    morastr->string = string;
    morastr->indices = new_indices;
    return (PyObject *) morastr;
}
#undef WRITE_KANA_INPLACE

subclass:
    PyObject *sub = MoraStr_subtype_call(type, string);
    Py_DECREF(string);
    return sub;

error:
    KWriter_Dealloc(writer);
    Py_XDECREF(string);
    MoraStr_INDICES_DEL(new_indices);
    return NULL;
}


static PyObject *
MoraStr_subscript(MoraStrObject *self, PyObject* arg) {
    if (PySlice_Check(arg)) {
        Py_ssize_t start, stop, step, slicelength, size;
        size = Py_SIZE(self);
        if (PySlice_GetIndicesEx(
                arg, size, &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }
        if (step != 1) {
            return MoraStr_slice_with_step(self, start, step, slicelength);
        }
        PyTypeObject *type = Py_TYPE(self);
        if (size == slicelength && IS_MORASTR_TYPE(type)) {
            Py_INCREF(self);
            return (PyObject *) self;
        }
        return MoraStr_get_slice_typed(self, start, stop, type);
    }
    Py_ssize_t i;
    if (PyLong_Check(arg)) {
        i = PyLong_AsSsize_t(arg);
    } else if (PyIndex_Check(arg)) {
        PyObject *pyint = PyNumber_Index(arg);
        if (!pyint) {return NULL;}
        i = PyLong_AsSsize_t(pyint);
        Py_DECREF(pyint);
    } else {
        PyErr_Format(PyExc_TypeError,
            "%.200s indices must be integers or slices, not %.200s",
            get_type_name((PyObject *)self), Py_TYPE(arg)->tp_name);
        return NULL;
    }
    PyObject *err;
    if ( i == -1 && (err = PyErr_Occurred()) ) {
        if (PyErr_GivenExceptionMatches(err, PyExc_OverflowError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_IndexError,
                "cannot fit '%.200s' into an index-sized integer",
                Py_TYPE(arg)->tp_name);
        }
        return NULL;
    }
    Py_ssize_t size = Py_SIZE(self);
    if (i < 0) {i += size;}
    if (!(0 <= i && i < size)) {
        const char *name = get_type_name((PyObject *)self);
        PyErr_Format(PyExc_IndexError, "%.200s index out of range", name);
        return NULL;
    }
    return MoraStr_Item(self, i);
}


static Py_hash_t
MoraStr_hash(MoraStrObject *self) {
    Py_hash_t hash = ((PyASCIIObject *)self->string)->hash;

    if (hash != -1) {return hash;}
    return PyObject_Hash(self->string);
}


static PyObject *
MoraStr_richcompare(PyObject *self, PyObject *other, int op) {
    assert(MoraStr_Check(self));
    PyObject *left = MoraStr_STRING(self);
    PyObject *right;
    if (MoraStr_Check(other)) {
        right = MoraStr_STRING(other);
    } else {
        return Py_NewRef(Py_NotImplemented);
    }
    if (op == Py_EQ || op == Py_NE) {
        return PyUnicode_RichCompare(left, right, op);
    }
    return Py_NewRef(Py_NotImplemented);
}

static PyObject *
MoraStr_tostr(MoraStrObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *s = MoraStr_STRING(self);
    Py_INCREF(s);
    return s;
}


static PyObject *
MoraStr_fromstrs(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"ignore", NULL};

    Py_ssize_t nargs = Py_SIZE(args);
    PyObject *empty_str = PyUnicode_New(0, 0);
    if (!empty_str) {return NULL;}
    PyObject *seq = PyTuple_New(nargs), *string = NULL;
{ /* got ownership */
    if (!seq) {goto error;}
    for (Py_ssize_t i = 0; i < nargs; ++i) {
        PyObject *item = PyTuple_GET_ITEM(args, i);
        if (PyUnicode_Check(item)) {
            Py_INCREF(item);
        } else if (MoraStr_CheckExact(item)) {
            item = MoraStr_STRING(item);
            Py_INCREF(item);
        } else {
            item = PyUnicode_Join(empty_str, item);
            if (!item) {
                if (nargs != 1 && PyErr_ExceptionMatches(PyExc_TypeError)) {
                    PyErr_Clear();
                    PyErr_Format(PyExc_TypeError,
                        "non-string element found in argument %zd", i+1);
                }
                goto error;
            }
        }
        PyTuple_SET_ITEM(seq, i, item);
    }
    if (nargs == 1) {
        string = PyTuple_GET_ITEM(seq, 0);
        Py_INCREF(string);
    } else {
        string = PyUnicode_Join(empty_str, seq);
        if (!string) {goto error;}
    }
    Py_CLEAR(seq);

    PyObject *morastr;
    BoolPred ignore = false;
    if (IS_MORASTR_TYPE(type)) {
        if (kwds) {
            PyObject *dummy = PyTuple_New(0);
            if (!dummy) {goto error;}
            bool result = PyArg_ParseTupleAndKeywords(
                dummy, kwds, "|$p", kwlist, &ignore);
            Py_DECREF(dummy);
            if (!result) {goto error;}
        }
        morastr = MoraStr_from_unicode_(string, !ignore);
    } else {
        PyObject *new_args = PyTuple_Pack(1, string);
        if (!new_args) {goto error;}
        morastr = PyObject_Call((PyObject *)type, new_args, kwds);
        Py_DECREF(new_args);
    }
    if (!morastr) {goto error;}
    Py_DECREF(empty_str);
    Py_DECREF(string);
    return morastr;
}

error:
    Py_DECREF(empty_str);
    Py_XDECREF(seq);
    Py_XDECREF(string);
    return NULL;
}


static PySequenceMethods morastr_as_sequence = {
    .sq_length = (lenfunc)MoraStr_length,
    .sq_concat = (binaryfunc)MoraStr_concat,
    .sq_repeat = (ssizeargfunc)MoraStr_repeat,
    .sq_item = (ssizeargfunc)MoraStr_sq_item,
    .sq_contains = (objobjproc)MoraStr_contains,
};

static PyMappingMethods morastr_as_mapping = {
    .mp_length = (lenfunc)MoraStr_length,
    .mp_subscript = (binaryfunc)MoraStr_subscript,
};

static PyObject *MoraStr_finditer(PyObject *, PyObject *, PyObject *);

static PyMethodDef MoraStr_methods[] = {
    {"__getitem__", (PyCFunction)MoraStr_subscript,
     METH_O | METH_COEXIST, PyDoc_STR(
     "__getitem__($self, index, /)\n"
     "--\n\n"
     "Return self[index]")},
    {"__getnewargs__", (PyCFunction)MoraStr___getnewargs__,
     METH_NOARGS, PyDoc_STR(
     "__getnewargs__($self, /)\n"
     "--\n\n")},
    {"__reduce__", (PyCFunction)MoraStr___reduce__,
     METH_NOARGS, PyDoc_STR(
     "__reduce__($self, /)\n"
     "--\n\n"
     "Return state information for pickling.")},
    {"char_indices", (PyCFunction)MoraStr_char_indices,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "char_indices($self, *, zero=False)\n"
     "--\n\n"
     "Returns a list of accumulative character counts of each mora. If \n"
     "the keyword 'zero' is set to True, the list starts with 0 and thus \n"
     "the total length of the returned list becomes len(self) + 1.")},
    {"count", (PyCFunction)MoraStr_count,
     METH_FASTCALL, PyDoc_STR(
     "count($self, sub_morastr, start=0, end=sys.maxsize, /)\n"
     "--\n\n"
     "Returns the number of non-overlapping occurrences of sub_morastr in \n"
     "the range of morastr[start:end]. If start exceeds len(morastr), 0 is \n"
     "returned. The first argument sub_morastr must be a kana string or a \n"
     "MoraStr object.")},
    {"endswith", (PyCFunction)MoraStr_endswith,
     METH_FASTCALL, PyDoc_STR(
     "endswith($self, suffix, start=0, end=sys.maxsize, /)\n"
     "--\n\n"
     "Like str.endswith(), but mora-wise.")},
    {"find", (PyCFunction)MoraStr_find,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "find($self, sub_morastr, start=0, end=sys.maxsize, /, *, charwise=False)\n"
     "--\n\n"
     "Returns the first mora index in the MoraStr object where sub_morastr \n"
     "is found within the range of morastr[start:end]. The first argument \n"
     "sub_morastr must be a kana string or a MoraStr object. The return \n"
     "value points a position counting from the beginning of the whole mora \n"
     "string, rather than counting from the index specified by the start \n"
     "argument. If start exceeds len(morastr) or sub_morastr is not found, \n"
     "-1 is returned. If keyword argument 'charwise' is set to True, the \n"
     "index count is calculated based on the number of kana characters \n"
     "instead of morae. The 'charwise' option is available only when \n"
     "start/end arguments are not specified.")},
    {"finditer", (PyCFunction)MoraStr_finditer,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "finditer($self, sub_morastr, /, *, charwise=False)\n"
     "--\n\n"
     "Returns an iterator yielding indices that indicate non-overlapping \n"
     "occurences for sub_morastr in self. The argument sub_morastr must be \n"
     "a kana string or a MoraStr object. If sub_morastr contains more morae \n"
     "than self, sub_morastr and self will be swapped and then searching \n"
     "will be performed, leaving the original MoraStr object intact. If the \n"
     "keyword argument 'charwise' is set to True, the index count is \n"
     "calculated based on the number of kana characters instead of morae.\n")},
    {"index", (PyCFunction)MoraStr_index,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "index($self, sub_morastr, start=0, end=sys.maxsize, / , *, charwise=False)\n"
     "--\n\n"
     "Same as MoraStr.find() except to raise an IndexError instead of \n"
     "returning -1 when sub_morastr is not found. The relationship of \n"
     "MoraStr.find() and MoraStr.index() is parallel to that of str.find() \n"
     "and str.index().")},
    {"removeprefix", (PyCFunction)MoraStr_removeprefix,
     METH_O, PyDoc_STR(
     "removeprefix($self, prefix, /)\n"
     "--\n\n"
     "If the MoraStr object starts with the prefix, returns \n"
     "morastr[len(prefix):]. Otherwise, a copy of the original object \n"
     "is returned.")},
    {"removesuffix", (PyCFunction)MoraStr_removesuffix,
     METH_O, PyDoc_STR(
     "removesuffix($self, suffix, /)\n"
     "--\n\n"
     "If the MoraStr object ends with the suffix, returns \n"
     "morastr[:len(morastr)-len(suffix)]. Otherwise, a copy of the \n"
     "original object is returned.")},
    {"replace", (PyCFunction)MoraStr_replace,
     METH_FASTCALL, PyDoc_STR(
     "replace($self, old, new, maxcount=-1, /)\n"
     "--\n\n"
     "Returns a copy of the MoraStr object with all occurrences of \n"
     "submora-string 'old' replaced by 'new'. If 'maxcount' option is \n"
     "set to a non-negative number, replacements are performed only \n"
     "'maxcount' times starting from the left.")},
    {"rfind", (PyCFunction)MoraStr_rfind,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "rfind($self, sub_morastr, start=0, end=sys.maxsize, / , *, charwise=False)\n"
     "--\n\n"
     "Returns the last mora index in the MoraStr object where sub_morastr \n"
     "is found within the range of morastr[start:end]. The first argument \n"
     "sub_morastr must be a kana string or a MoraStr object. If start \n"
     "exceeds len(morastr) or sub_morastr is not found, -1 is returned. If \n"
     "keyword argument 'charwise' is set to True, the index count is \n"
     "calculated based on the number of kana characters instead of morae. \n"
     "The 'charwise' option is available only when start/end arguments are \n"
     "not specified.")},
    {"rindex", (PyCFunction)MoraStr_rindex,
     METH_VARARGS | METH_KEYWORDS, PyDoc_STR(
     "rindex($self, sub_morastr, start=0, end=sys.maxsize, / , *, charwise=False)\n"
     "--\n\n"
     "Same as MoraStr.find() except to raise an IndexError instead of \n"
     "returning -1 when sub_morastr is not found. The relationship of \n"
     "MoraStr.rfind() and MoraStr.rindex() is parallel to that of \n"
     "str.rfind() and str.rindex().")},
    {"startswith", (PyCFunction)MoraStr_startswith,
     METH_FASTCALL, PyDoc_STR(
     "startswith($self, prefix, start=0, end=sys.maxsize, /)\n"
     "--\n\n"
     "Like str.startswith(), but mora-wise.")},
    {"tostr", (PyCFunction)MoraStr_tostr,
     METH_NOARGS, PyDoc_STR(
     "tostr($self, /)\n"
     "--\n\n"
     "Returns the internal string representation of the MoraStr object.\n"
     "\n"
     "Note: In the current implementation, this method just returns the \n"
     "value of the instance's 'string' member. It is intended to use it \n"
     "with functions that require a callback as one of their arguments.\n")},
    {"fromstrs", (PyCFunction)MoraStr_fromstrs,
     METH_VARARGS | METH_KEYWORDS | METH_CLASS, PyDoc_STR(
     "fromstrs($cls, *iterables, **kwargs)\n"
     "--\n\n"
     "Alternate constructor for MoraStr(). Returns a new MoraStr object \n"
     "from multiple strings. This method takes an arbitrary number of \n"
     "positional arguments, each of which must be an iterable of str \n"
     "objects. Keyword arguments are treated in the same way as MoraStr(). \n"
     "Roughly equivalent to:\n"
     "\n"
     "@classmethod\n"
     "def fromstrs(cls, *iterables, **kwargs):\n"
     "    return cls(''.join(''.join(it) for it in iterables), **kwargs)")},
    {"count_all", (PyCFunction)MoraStr_count_all,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     morastr_count_all_docstring},
    {NULL, NULL}
};


static PyMemberDef MoraStr_members[] = {
    {"length", T_PYSSIZET, offsetof(PyVarObject, ob_size), READONLY, PyDoc_STR(
     "morastr.length <==> len(morastr)")},
    {"string", T_OBJECT_EX, offsetof(MoraStrObject, string), READONLY, PyDoc_STR(
     "Underlying katakana representation of the MoraStr object as a plain \n"
     "str object. All characters are guaranteed to be full-width (zenkaku) \n"
     "and to not contain spaces.")},
    {NULL}
};

static PyObject *MoraStr_iter(PyObject *);

static PyTypeObject MoraStrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "morastrja.MoraStr",
    .tp_basicsize = sizeof(MoraStrObject),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)MoraStr_dealloc,
    .tp_repr = (reprfunc)MoraStr_repr,
    .tp_as_sequence = &morastr_as_sequence,
    .tp_as_mapping = &morastr_as_mapping,
    .tp_hash = (hashfunc)MoraStr_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = PyDoc_STR(
     "MoraStr(kana_string: str | MoraStr = '',\n" \
     "        /, *,\n" \
     "        ignore: bool = False) -> MoraStr\n" \
     "\n" \
     "Divides kana_string into fractions each of which corresponds to a \n"
     "Japanese mora. kana_string must be a MoraStr object or a string \n"
     "composed of Japanese kana syllabaries. Otherwise, the constructor \n"
     "raises a ValueError unless 'ignore' parameter is set to True, in \n"
     "which case, invalid characters are just skipped. All elements in \n"
     "MoraStr objects are guaranteed to be full-width (zenkaku) katakana. \n"
     "Hiragana and half-width (hankaku) katakana in the input string are \n"
     "converted to proper forms.\n"
     ""),
    .tp_richcompare = (richcmpfunc)MoraStr_richcompare,
    .tp_iter = MoraStr_iter,
    .tp_methods = MoraStr_methods,
    .tp_members = MoraStr_members,
    .tp_new = (newfunc)MoraStr_new,
};


/*********************** MoraStr Iterator **************************/
typedef struct {
    PyObject_HEAD
    Py_ssize_t it_index;
    MoraStrObject *it_seq;
} MoraStrIterObject;


static void
MoraStrIter_dealloc(MoraStrIterObject *it) {
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->it_seq);
    PyObject_GC_Del(it);
}


static int
MoraStrIter_traverse(MoraStrIterObject *it, visitproc visit, void *arg) {
    Py_VISIT(it->it_seq);
    return 0;
}


static PyObject *
MoraStrIter_next(MoraStrIterObject *it) {
    MoraStrObject *morastr = it->it_seq;
    if (!morastr) {return NULL;}

    Py_ssize_t index = it->it_index;
    if (index < Py_SIZE(morastr)) {
        PyObject *string = MoraStr_STRING(morastr), *item;
        MINDEX_T *indices = MoraStr_INDICES(morastr);
        if (!indices) {
            Katakana k = KATAKANA_STR_READ(string, index);
            item = PyUnicode_FromOrdinal(k);
        } else {
            Py_ssize_t s_start, s_end;
            s_start = index ? indices[index-1] : 0LL;
            s_end = indices[index];
            const Katakana *buf = KatakanaArray_from_str(string);
            item = PyUnicode_FromKindAndData(
                KATAKANA_KIND, (const void*)(buf+s_start), s_end - s_start);
        }
        if (item) {++it->it_index;}
        return item;
    }
    it->it_seq = NULL;
    Py_DECREF(morastr);
    return NULL;
}


static PyObject *
MoraStrIter_len(MoraStrIterObject *it) {
    MoraStrObject *obj = it->it_seq;
    Py_ssize_t len = obj ? Py_SIZE(obj) - it->it_index : 0;
    return PyLong_FromSsize_t(len);
}


static PyObject *
MoraStrIter_reduce(MoraStrIterObject *it, PyObject *Py_UNUSED(ignored)) {
#if defined(_Py_IDENTIFIER)
    _Py_IDENTIFIER(iter);
    if (!it->it_seq) {
        return Py_BuildValue("N(())", _PyEval_GetBuiltinId(&PyId_iter));
    }
    return Py_BuildValue("N(O)n",
        _PyEval_GetBuiltinId(&PyId_iter), it->it_seq, it->it_index);
#else
    PyObject *builtins_dict = PyEval_GetBuiltins();
    if (!builtins_dict) {return NULL;}
    PyObject *iter_func = PyDict_GetItemString(builtins_dict, "iter");
    if (!iter_func) {
        PyErr_SetString(PyExc_AttributeError, "iter");
        return NULL;
    }
    if (!it->it_seq) {
        return Py_BuildValue("O(())", iter_func);
    }
    return Py_BuildValue("O(O)n", iter_func, it->it_seq, it->it_index);
#endif
}


static PyObject *
MoraStrIter_setstate(MoraStrIterObject *it, PyObject *state) {
    Py_ssize_t index = PyLong_AsSsize_t(state);
    if (index == -1 && PyErr_Occurred()) {return NULL;}
    if (it->it_seq) {
        if (index < 0) {index = 0;}
        it->it_index = index;
    }
    return Py_NewRef(Py_None);
}


static PyMethodDef MoraStrIter_methods[] = {
    {"__length_hint__", (PyCFunction)MoraStrIter_len, METH_NOARGS, PyDoc_STR(
     "Private method returning an estimate of len(list(it)).")},
    {"__reduce__", (PyCFunction)MoraStrIter_reduce, METH_NOARGS, PyDoc_STR(
     "Return state information for pickling.")},
    {"__setstate__", (PyCFunction)MoraStrIter_setstate, METH_O, PyDoc_STR(
     "Set state information for unpickling.")},
    {NULL, NULL}
};


PyTypeObject MoraStrIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "morastrja.MoraStrIterator",
    .tp_basicsize = sizeof(MoraStrIterObject),
    .tp_dealloc = (destructor)MoraStrIter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MoraStrIter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)MoraStrIter_next,
    .tp_methods = MoraStrIter_methods,
};


static PyObject *
MoraStr_iter(PyObject *obj) {
    if (!MoraStr_Check(obj)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    MoraStrObject *seq = (MoraStrObject *)obj;
    MoraStrIterObject *it;
    it = PyObject_GC_New(MoraStrIterObject, &MoraStrIterType);
    if (!it) {return NULL;}
    it->it_index = 0;
    Py_INCREF(seq);
    it->it_seq = (MoraStrObject *)seq;
    PyObject_GC_Track(it);
    return (PyObject *)it;
}


/*********************** MoraStr FindIter **************************/
enum {FINDITER_POOL_LIMIT = 32};

typedef struct {
    PyObject_HEAD
    MINDEX_T *ptr;
    PyObject *morastr;
    PyObject *substr;
    void *needle_cache;
    MINDEX_T submora_cnt;
    union {
        MINDEX_T pos;
        MINDEX_T pool[FINDITER_POOL_LIMIT+1];
    } state;
} MoraStrFindIterObject;


#define MoraStrFindIter_needle_DEL(needle) do { \
    two_way_cache_dealloc(needle); \
    (needle) = NULL; \
} while (0)


static void
MoraStrFindIter_dealloc(MoraStrFindIterObject *it) {
    PyObject_GC_UnTrack(it);
    Py_CLEAR(it->morastr);
    Py_CLEAR(it->substr);
    MoraStrFindIter_needle_DEL(it->needle_cache);
    PyObject_GC_Del(it);
}


static int
MoraStrFindIter_traverse(
    MoraStrFindIterObject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->morastr);
    Py_VISIT(it->substr);
    return 0;
}


static MINDEX_T
MoraStrFindIter_two_way(
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t start, MoraStrFindIterObject *it,
    const MINDEX_T *indices, bool charwise)
{
    PyObject *morastr = it->morastr;
    Py_ssize_t submora_cnt = ~(it->submora_cnt);
    MINDEX_T *ptr = it->state.pool;

    Py_ssize_t cap, end, mora_cnt = Py_SIZE(morastr);
    cap = (Py_ssize_t)Py_MIN(
        (uint64_t)submora_cnt * 2 + start + 2048,
        (uint64_t)mora_cnt);
    end = mora_cnt;
    if (!indices) {
        do {
            start = two_way_search(
                s, end, p, p_len, start, -1);
            *ptr++ = MINDEX(start);
            if (start == -1) {
                start = (end == mora_cnt) ? \
                    (Py_ssize_t)MINDEX_MAX : end - submora_cnt + 1;
                break;
            }
            start += submora_cnt;
            end = cap;
        } while (it->state.pool + FINDITER_POOL_LIMIT != ptr);
    } else {
        do {
            start = two_way_mora_search(
                s, indices[end-1], p, p_len, start,
                submora_cnt, indices, -1);
            if (charwise) {
                *ptr++ = 0 < start ? indices[start-1] : MINDEX(start);
            } else {
                *ptr++ = MINDEX(start);
            }
            if (start == -1) {
                start = (end == mora_cnt) ? \
                    (Py_ssize_t)MINDEX_MAX : end - submora_cnt + 1;
                break;
            }
            start += submora_cnt;
            end = cap;
        } while (it->state.pool + FINDITER_POOL_LIMIT != ptr);
    }

    two_way_set_needle(NULL);
    MINDEX_T result = it->state.pool[0];
    MINDEX_T pos = MINDEX(start);
    it->state.pos = charwise ? ~pos : pos;
    return result;
}


static MINDEX_T
MoraStrFindIter_with_cache(MoraStrFindIterObject *it)
{
    MINDEX_T pos = it->state.pos;
    BoolPred charwise = false;
    if (pos < 0) {
        charwise = true; pos = ~pos;
    }
    if (pos == MINDEX_MAX) {return -1;}

    PyObject *morastr = it->morastr;
    PyObject *substr = it->substr;
    MINDEX_T *ptr = it->state.pool + 1;
    Py_ssize_t substr_len = PyUnicode_GET_LENGTH(substr);

    PyObject *string = MoraStr_STRING(morastr);
    MINDEX_T *indices = MoraStr_INDICES(morastr);
    Py_ssize_t len = PyUnicode_GET_LENGTH(string);
    if (it->ptr != ptr) {
        memset((void*)ptr, -1, sizeof(MINDEX_T)*(FINDITER_POOL_LIMIT));
    }
    it->ptr = --ptr;

    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);

    two_way_set_needle(it->needle_cache);
    return MoraStrFindIter_two_way(
        s, len, p, substr_len, (Py_ssize_t)pos, it, indices, charwise);
}


static MINDEX_T
MoraStrFindIter_find_push(MoraStrFindIterObject *it) {
    MINDEX_T pos = it->state.pos;
    BoolPred charwise = false;
    if (pos < 0) {
        charwise = true; pos = ~pos;
    }
    if (pos == MINDEX_MAX) {return -1;}

    PyObject *morastr = it->morastr;
    PyObject *substr = it->substr;
    Py_ssize_t submora_cnt = it->submora_cnt;
    MINDEX_T *ptr = it->state.pool + 1;
    Py_ssize_t substr_len = PyUnicode_GET_LENGTH(substr);

    PyObject *string = MoraStr_STRING(morastr);
    MINDEX_T *indices = MoraStr_INDICES(morastr);
    Py_ssize_t len = PyUnicode_GET_LENGTH(string);
    if (it->ptr != ptr) {
        memset((void*)ptr, -1, sizeof(MINDEX_T)*(FINDITER_POOL_LIMIT));
    }
    it->ptr = --ptr;

    const Katakana *s = KatakanaArray_from_str(string);
    const Katakana *p = KatakanaArray_from_str(substr);

    
    if (!pos) {
        if (!indices && submora_cnt != substr_len) {return -1;}
        MoraStr_assert(substr_len <= MINDEX_MAX);
        int status = search_algorithm_prepare(
            s, len, p, substr_len, 0, submora_cnt, indices);
        if (status == -1) {
            search_algorithm_disable_cache();
            return -2;
        }
        if (status == SEARCH_TWOWAY) {
            it->submora_cnt = ~(it->submora_cnt);
            MINDEX_T result = MoraStrFindIter_two_way(
                s, len, p, substr_len, (Py_ssize_t)pos,
                it, indices, charwise);
            pos = charwise ? ~(it->state.pos) : it->state.pos;
            if (pos != MINDEX_MAX) {
                void *needle_cache = two_way_cache_new();
                if (!needle_cache) {
                    PyErr_NoMemory();
                    return -2;
                }
                it->needle_cache = needle_cache;
            }
            return result;
        }
    }

    Py_ssize_t start = (Py_ssize_t)pos, cap, end;
    Py_ssize_t mora_cnt = Py_SIZE(morastr);
    cap = (Py_ssize_t)Py_MIN(
        (uint64_t)submora_cnt * 2 + start + 2048,
        (uint64_t)mora_cnt);
    end = mora_cnt;
    if (!indices) {
        do {
            start = generic_katakana_search(
                s, end, p, substr_len, start, -1);
            *ptr++ = MINDEX(start);
            if (start == -1) {
                start = (end == mora_cnt) ? \
                    (Py_ssize_t)MINDEX_MAX : end - submora_cnt + 1;
                break;
            }
            start += submora_cnt;
            end = cap;
        } while (it->state.pool + FINDITER_POOL_LIMIT != ptr);
    } else {
        do {
            start = generic_mora_search(
                s, indices[end-1], p, substr_len, start,
                submora_cnt, indices, -1);
            if (charwise) {
                *ptr++ = 0 < start ? indices[start-1] : MINDEX(start);
            } else {
                *ptr++ = MINDEX(start);
            }
            if (start == -1) {
                start = (end == mora_cnt) ? \
                    (Py_ssize_t)MINDEX_MAX : end - submora_cnt + 1;
                break;
            }
            start += submora_cnt;
            end = cap;
        } while (it->state.pool + FINDITER_POOL_LIMIT != ptr);
    }

    MINDEX_T result = it->state.pool[0];
    pos = MINDEX(start);
    it->state.pos = charwise ? ~pos : pos;
    return result;
}


static PyObject *
MoraStrFindIter_empty_needle_next(MoraStrFindIterObject *it) {
    PyObject *morastr = it->morastr;
    MINDEX_T pos = it->state.pos;
    size_t limit = (size_t)Py_SIZE(morastr);
    size_t val = (uint32_t)it->state.pool[2] + 1;
    if (val > limit) {
        it->ptr = NULL;
        Py_CLEAR(it->morastr);
        Py_CLEAR(it->substr);
        MoraStrFindIter_needle_DEL(it->needle_cache);
        return NULL;
    }
    BoolPred charwise = (pos < 0);
    it->state.pool[2]++;
    if (charwise) {
        MINDEX_T *indices = MoraStr_INDICES(morastr);
        if (val && indices) {val = (size_t)indices[val-1];}
    }
    return PyLong_FromLong((long)val);
}


static PyObject *
MoraStrFindIter_next(MoraStrFindIterObject *it) {
    MINDEX_T *ptr = it->ptr;
    if (!ptr) {return NULL;}

    MINDEX_T val = *ptr;
    MINDEX_T submora_cnt = it->submora_cnt;
    if (val != -1) {
        it->ptr++;
        return PyLong_FromLong(val);
    }

    if (!submora_cnt) {
        return MoraStrFindIter_empty_needle_next(it);
    } else if (submora_cnt < 0) {
        val = MoraStrFindIter_with_cache(it);
    } else {
        val = MoraStrFindIter_find_push(it);
    }
    if (val < 0) {
        it->ptr = NULL;
        Py_CLEAR(it->morastr);
        Py_CLEAR(it->substr);
        MoraStrFindIter_needle_DEL(it->needle_cache);
        return NULL;
    }
    it->ptr++;
    return PyLong_FromLong(val);
}


PyTypeObject MoraStrFindIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "morastrja.morastr_finditerator",
    .tp_basicsize = sizeof(MoraStrFindIterObject),
    .tp_dealloc = (destructor)MoraStrFindIter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MoraStrFindIter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)MoraStrFindIter_next,
};


static PyObject *
MoraStr_finditer(PyObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"", "charwise", NULL};

    PyObject *submora;
    BoolPred charwise = false;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|$p", kwlist, &submora, &charwise)) {
        return NULL;
    }

    if (PyUnicode_Check(submora)) {
        submora = MoraStr_from_unicode_(submora, true);
        if (!submora) {return NULL;}
    } else if (MoraStr_Check(submora)) {
        Py_INCREF(submora);
    } else {
        PyErr_Format(PyExc_TypeError,
            "argument 1 must be a kana string "
            "or a MoraStr object, not '%.200s'",
            Py_TYPE(submora)->tp_name);
        return NULL;
    }
    PyObject *morastr, *substr;
    Py_ssize_t mora_cnt, submora_cnt;
    mora_cnt = Py_SIZE(self);
    submora_cnt = Py_SIZE(submora);

    if (mora_cnt < submora_cnt) {
        morastr = submora;
        substr = MoraStr_STRING(self);
        submora_cnt = mora_cnt;
        Py_INCREF(substr);
    } else {
        morastr = self;
        Py_INCREF(morastr);
        substr = MoraStr_STRING(submora);
        Py_INCREF(substr);
        Py_DECREF(submora);
    }

{ /* got ownership */
    MoraStrFindIterObject *it;
    it = PyObject_GC_New(MoraStrFindIterObject, &MoraStrFindIterType);
    if (!it) {goto error;}

    it->ptr = it->state.pool + 1;
    it->morastr = morastr;
    it->substr = substr;
    it->needle_cache = NULL;
    it->submora_cnt = MINDEX(submora_cnt);
    it->state.pos = charwise ? -1 : 0;
    memset((void*)(it->state.pool+1), -1,
        sizeof(MINDEX_T)*(FINDITER_POOL_LIMIT));

    PyObject_GC_Track(it);
    return (PyObject *)it;
}

error:
    Py_DECREF(morastr);
    Py_DECREF(substr);
    return NULL;
}


static PyObject *
Empty_MoraStr_make(void) {
    static PyTypeObject *type = &MoraStrType;

    PyObject *string = PyUnicode_New(0, 0);
    if (!string) {return NULL;}
    MoraStrObject *self = (MoraStrObject *) type->tp_alloc(type, 0);
    if (!self) {
        Py_DECREF(string);
        return NULL;
    }
    Py_SET_SIZE(self, 0);
    self->string = string;
    self->indices = NULL;
    empty_morastr = self;
    Py_INCREF(self);
    return (PyObject *) self;
}


static inline PyObject *
Empty_MoraStr(void) {
    if (empty_morastr) {
        Py_INCREF(empty_morastr);
        return (PyObject *) empty_morastr;
    }
    return Empty_MoraStr_make();
}


static inline bool
IS_MORASTR_TYPE(PyTypeObject *type) {
    return type == &MoraStrType;
}

static inline bool
MoraStr_Check_(PyObject *self) {
    return PyType_IsSubtype(Py_TYPE(self), &MoraStrType);
}

static PyMethodDef morastr_methods[] = {
    {"_register", (PyCFunction)morastr__register,
     METH_O, PyDoc_STR(
     "set a conversion table")},
    {"_set_converter", (PyCFunction)morastr__set_converter,
     METH_O, PyDoc_STR(
     "set a conversion function")},
    {"count_all", (PyCFunction)MoraStr_count_all,
     METH_VARARGS | METH_KEYWORDS,
     morastr_count_all_docstring},
    {"vowel_to_choon", (PyCFunction)MoraStr_vowel_to_choon,
     METH_VARARGS | METH_KEYWORDS,
     "vowel_to_choon(kana_string, /, maxrep=1, *, \n"
     "               clean=False, ou=False, ei=False, nn=True)\n"
     "--\n\n"
     "Returns a string or a MoraStr object with consecutive same vowels \n"
     "replaced by choonpu (i.e. prolonged sound marks). The input string is \n"
     "assumed to be composed of zenkaku (full-width) katakana. The return \n"
     "type will be str if an instance of str or its subtype is passed to \n"
     "this function. If it receives a MoraStr object or an instance of a \n"
     "class inheriting from MoraStr, the return value will be also a \n"
     "MoraStr object. The 'maxrep' option, which is default to 1, restricts \n"
     "repetion of choonpu in succession for the output. if 'maxrep' is set \n"
     "to -1, the maximum repetion of choonpu gets no restriction. With \n"
     "turning on the 'clean' option, you can eliminate all characters other \n"
     "than zenkaku katakana from the string. The keywords 'ou' and 'ei', \n"
     "whose defaults are False, control whether to convert vowel sequences \n"
     "(corresponding to the keyword names) into transcriptions with \n"
     "choonpu. If the keyword 'nn' is set to False, consecutive moraic \n"
     "nasals will not be replaced by choonpu."},
    {"choon_to_vowel", (PyCFunction)MoraStr_choon_to_vowel,
     METH_VARARGS | METH_KEYWORDS,
     "choon_to_vowel(kana_string, /, *, strict=True, clean=False)\n"
     "--\n\n"
     "Returns a string or a MoraStr object with choonpu (prolonged sound \n"
     "marks) replaced by vowel characters.  The input string is assumed to \n"
     "be composed of zenkaku (full-width) katakana. Roughly speaking, this \n"
     "function performs the inverse of vowel_to_choon(), although the \n"
     "result of vowel_to_choon(choon_to_vowel(kana_string)) is not \n"
     "necessarily equivalent to the original kana_string. The return type \n"
     "will be str if an instance of str or its subtype is passed to this \n"
     "function. If it receives a MoraStr object or an instance of a class \n"
     "inheriting from MoraStr, the return type will be also MoraStr. \n"
     "ValueError is raised when choonpu is used improperly or appears \n"
     "immediately after a non-katakana character. If the keyword 'strict' \n"
     "is set to False, inappropriate choonpu are silently ignored and \n"
     "left untouched for the output. If the keyword 'clean' is set to True, \n"
     "all characters but zenkaku katakana will be eliminated and thus you \n"
     "can safely pass the return value to MoraStr(). Note that the 'clean' \n"
     "option per se doesn't suppress ValueError as the cleaning process is \n"
     "performed after error checking and replacement of choonpu."},
    {NULL, NULL, 0, NULL}
};


static struct PyModuleDef morastrmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_morastr",
    .m_size = -1,
    .m_methods = morastr_methods
};

PyMODINIT_FUNC
PyInit__morastr(void) {
    PyObject *m;
    if (PyType_Ready(&MoraStrType) < 0) {return NULL;}

    if (PyType_Ready(&MoraStrIterType) < 0) {return NULL;}

    m = PyModule_Create(&morastrmodule);
    if (m == NULL) {return NULL;}
    Py_INCREF(&MoraStrType);
{ /* got ownership */
    if (PyModule_AddObject(m, "MoraStr", (PyObject *) &MoraStrType) < 0) {
        goto error;
    }

    hankaku_pair_map = PyDict_New();
    if (!hankaku_pair_map) {goto error;}
    init_katakana_table();

    return m;
}

error:
    Py_DECREF(m);
    Py_DECREF(&MoraStrType);
    Py_CLEAR(hankaku_pair_map);
    return NULL;
}

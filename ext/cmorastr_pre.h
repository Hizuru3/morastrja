#ifndef CMORASTR_PRE_H_
#define CMORASTR_PRE_H_

#ifdef __cplusplus
extern "C" {
#endif


#if defined(MORASTR_DEBUG_MODE) && MORASTR_DEBUG_MODE != 0
  #undef NDEBUG
#endif

#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <stdbool.h>
#include "structmember.h"


/* Token Concatenation */

#define JOIN_(x, y) x ## y
#define JOIN(x, y) JOIN_(x, y)


/* Backports of Newer CPython APIs */

#ifndef Py_XSETREF
  #define Py_XSETREF(op, op2) \
do { \
    PyObject *_py_tmp = (PyObject *)(op); \
    (op) = (op2); \
    Py_XDECREF(_py_tmp); \
} while (0)
#endif


#ifndef Py_NewRef
static inline PyObject* _Py_NewRef(PyObject *obj)
{
    Py_INCREF(obj);
    return obj;
}
  #define Py_NewRef(obj) _Py_NewRef((PyObject*)(obj))
#endif


#ifndef Py_SET_SIZE
  #define Py_SET_SIZE(o, size) (Py_SIZE(o) = (size))
#endif


// deprecated
#ifndef PyUnicode_READY
  #define PyUnicode_READY(op) (0)
#endif


/* Memory Allocation */

#if defined(MORASTR_USING_PYMEM_MALLOC)
  #define MoraStr_Malloc PyMem_Malloc
  #define MoraStr_Free PyMem_Free
  #define MoraStr_RESIZE PyMem_Resize
#else
  #define MoraStr_Malloc malloc
  #define MoraStr_Free free
  #define MoraStr_RESIZE(p, TYPE, n) \
  ( (p) = ((size_t)(n) > PY_SSIZE_T_MAX / sizeof(TYPE)) ? NULL : \
        (TYPE *) realloc((p), (n) * sizeof(TYPE)) )
#endif


/* Optimization and Debug */

#ifndef RESTRICT
  #define RESTRICT __restrict
#endif

#if !defined(NDEBUG)
  #define MoraStr_assert(expr) assert(expr)
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
  #define MoraStr_assert(expr) if (expr) {} else __builtin_unreachable()
#elif defined(__clang__)
  #define MoraStr_assert(expr) __builtin_assume(expr)
#elif defined(_MSC_VER) || defined(__INTEL_COMPILER)
  #define MoraStr_assert(expr) __assume(expr)
#else
  #define MoraStr_assert(expr) assert(expr)
#endif


#define DEBUG_PRINT(fmt, ...) do { \
    PyObject *_obj = PyUnicode_FromFormat(fmt "\n", __VA_ARGS__); \
    PyObject_Print(_obj, stdout, Py_PRINT_RAW); \
    Py_XDECREF(_obj); \
} while (0)


/* Bit Operation */

static inline unsigned int
TZCNT32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? (unsigned int)__builtin_ctz(x) : 0U;
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, x);
    return index;
#elif defined(__has_builtin)
  #if __has_builtin(__builtin_ctz)
    return x ? (unsigned int)__builtin_ctz(x) : 0U;
  #endif
#endif
    static const uint32_t debruijn = 0x077CB531U;
    static const unsigned char dtable[] = {
        0,  1,  28, 2,  29, 14, 24, 3,  30, 22, 20, 15, 25, 17, 4,  8,
        31, 27, 13, 23, 21, 19, 16, 7,  26, 12, 18, 6,  11, 5,  10, 9
    };

    return x ? dtable[((x & (0u-x)) * debruijn) >> 27] : 0U;
}

static inline unsigned int
TZCNT64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? (unsigned int)__builtin_ctzll(x) : 0U;
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, x);
    return index;
#elif defined(__has_builtin)
  #if __has_builtin(__builtin_ctzll)
    return x ? (unsigned int)__builtin_ctzll(x) : 0U;
  #endif
#endif
    static const uint64_t debruijn = 0x03F566ED27179461ULL;
    static const unsigned char dtable[] = {
        0,  1,  59, 2,  60, 40, 54, 3,  61, 32, 49, 41, 55, 19, 35, 4,
        62, 52, 30, 33, 50, 12, 14, 42, 56, 16, 27, 20, 36, 23, 44, 5,
        63, 58, 39, 53, 31, 48, 18, 34, 51, 29, 11, 13, 15, 26, 22, 43,
        57, 38, 47, 17, 28, 10, 25, 21, 37, 46, 9,  24, 45, 8,  7,  6
    };

    return x ? dtable[((x & (0u-x)) * debruijn) >> 58] : 0U;
}

#define TZCNT(x) \
    (sizeof(x) <= 4 ? TZCNT32((uint32_t)(x)) : TZCNT64((uint64_t)(x)))


/* Generic Unicode String */

#define MoraStr_Unicode_WRITE(kind, data, index, value) \
do { \
    int _kind = (kind); \
    if (_kind == PyUnicode_2BYTE_KIND) { \
        ((Py_UCS2 *)(data))[(index)] = (Py_UCS2)(value); \
    } else if (_kind == PyUnicode_1BYTE_KIND) { \
        ((Py_UCS1 *)(data))[(index)] = (Py_UCS1)(value); \
    } else { \
        MoraStr_assert(_kind == PyUnicode_4BYTE_KIND); \
        ((Py_UCS4 *)(data))[(index)] = (Py_UCS4)(value); \
    } \
} while (0)


#define MoraStr_Unicode_READ(KIND, data, index) \
    ((Py_UCS4) \
    ((KIND) == PyUnicode_2BYTE_KIND ? \
        ((const Py_UCS2 *)(data))[(index)] : \
        ((KIND) == PyUnicode_1BYTE_KIND ? \
            ((const Py_UCS1 *)(data))[(index)] : \
            ((const Py_UCS4 *)(data))[(index)] \
        ) \
    ))


#define UCSX_KIND(var) JOIN_(var, __KIND)
#define UCSX_DATA(var) JOIN_(var, __DATA)
#define DEF_TAGGED_UCS(var, pystr) \
    int UCSX_KIND(var) = PyUnicode_KIND(pystr); \
    void *UCSX_DATA(var) = PyUnicode_DATA(pystr)
#define UCSX_WRITE(var, i, x) \
    MoraStr_Unicode_WRITE(UCSX_KIND(var), UCSX_DATA(var), (i), (x))
#define UCSX_READ(var, i) \
    MoraStr_Unicode_READ(UCSX_KIND(var), UCSX_DATA(var), (i))



/* Other Utilities */

#define ALIGN_DOWN(p, a) ((void *)((uintptr_t)(p) & ~(uintptr_t)((a) - 1)))

static inline const char *
get_type_name(PyObject *self) {
    const char *raw_name = Py_TYPE(self)->tp_name;
    const char *name = strrchr(raw_name, '.');
    return (name == NULL) ? raw_name : name + 1;
}


/* Types and Parameters */

typedef int BoolPred;

typedef int32_t MINDEX_T;
enum {MINDEX_MAX = INT32_MAX};

#define MINDEX(x) (MINDEX_T)(x)

typedef Py_UCS2 Katakana;
#define KATAKANA_KIND PyUnicode_2BYTE_KIND
enum {
    KATAKANA_OFF = 0x30a0,
    KATAKANA_RNG = 96
};

#define KANA_ID(c) ((ptrdiff_t)(c) - KATAKANA_OFF)


/* Requirements */

#if !(defined(__STDC_ISO_10646__) || defined(MS_WINDOWS))
  #error "wchar_t type does not support unicode"
#endif
#if !(~0 == -1)
  #error "negative numbers are not implemented with 2's complement"
#endif
#if !((-1 >> 1) == -1)
  #error "signed integers do not support arithmetic shift"
#endif


#ifdef __cplusplus
}
#endif

#endif
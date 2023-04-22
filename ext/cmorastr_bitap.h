#include "cmorastr_pre.h"

// size_t CHAR_INDEX(Katakana ch)
// size_t BITAP_TABLE_SIZE
// type BITAP_UINT_T
// type BITAP_VAR_UINT_T


#define MORASTR_SEARCH(name) JOIN(name, BITAP_UINT_T)

static Py_ssize_t
MORASTR_SEARCH(bitap_search_) (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count);

static Py_ssize_t
MORASTR_SEARCH(bitap_mora_search_) (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices, Py_ssize_t count);


static BITAP_UINT_T MORASTR_SEARCH(bitap_table_)[BITAP_TABLE_SIZE];
#define BITAP_TABLE MORASTR_SEARCH(bitap_table_)

#define BITAP_NEXT_STATE(state, start_bit, c) \
    ( BITAP_TABLE[CHAR_INDEX(c)] & (((state) >> 1) | (start_bit)) )


#define RESET_BITAP_TABLE() do { \
    for (Py_ssize_t i = 0; i < BITAP_TABLE_SIZE;) { \
        BITAP_TABLE[i++] = 0; \
    } \
} while (0)


static Py_ssize_t
MORASTR_SEARCH(bitap_search_) (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count)
{
    MoraStr_assert(0 < p_len && count != 0);

    s += mora_off; s_len -= mora_off;
    Py_ssize_t limit = s_len - p_len + 1;
    if (limit < 1) {return count;}

    uint32_t last_idx = (uint32_t)p_len - 1;
    BITAP_UINT_T bit_state;
    Py_ssize_t gap;
#define START_BIT() ((BITAP_UINT_T)1 << last_idx)
    for (uint32_t i = 0; i < last_idx; ++i) {
        Katakana k = p[i];
        BITAP_TABLE[CHAR_INDEX(k)] |= START_BIT() >> i;
    }
    Katakana last = p[last_idx];
    bit_state = BITAP_TABLE[CHAR_INDEX(last)];
    BITAP_TABLE[CHAR_INDEX(last)] |= 1;
    gap = bit_state ? TZCNT(bit_state) : p_len;

    Py_ssize_t i = 0, k = -1;
    while (i < limit) {
        BITAP_UINT_T bits;
        Katakana kana = s[i+last_idx];

        if (kana != last) {
            bits = BITAP_TABLE[CHAR_INDEX(kana)];
            do {
                i += bits ? TZCNT(bits) : last_idx + 1;
                if (i >= limit) {goto post_process;}
                Katakana prev = kana;
                kana = s[i+last_idx];
                if (prev == kana) {continue;}
                bits = BITAP_TABLE[CHAR_INDEX(kana)];
            } while (kana != last);
        }

        uint32_t j;
        if (i > k) {
            if (last_idx && p[0] != s[i]) {goto nomatch;}
            bit_state = START_BIT();
            j = 1;
        } else {
            j = (uint32_t)(k - i) + 1;
        }
        bits = START_BIT();
        for (; j < last_idx; ++j) {
            bit_state = BITAP_NEXT_STATE(bit_state, START_BIT(), s[i+j]);
            if (!((bits >> j) & bit_state)) {goto partial_match;}
        }
        if (count == -1) {
            count = mora_off + i;
            goto post_process;
        }
        if (!(--count)) {break;}
        i += last_idx + 1;
        goto loop_end;

    nomatch:
        if (i + 1 >= limit) {break;}
        kana = s[i+last_idx+1];
        bits = BITAP_TABLE[CHAR_INDEX(kana)];
        i += bits ? gap : last_idx + 2;
        goto loop_end;

    partial_match:
        bits = bit_state >> (last_idx - j);
        if (bits) {
            k = (Py_ssize_t)(i + j);
            if (gap > j) {
                i += gap;
            } else {
                i += TZCNT(bits);
                last = p[j];
                while (i < limit && last != s[i+j]) {++i;}
                last = p[last_idx];
                if (i >= limit) {break;}
            }
        } else {
            i += (Py_ssize_t)Py_MAX(gap, j + 1);
        }
        if (i < limit) {continue;}

    loop_end:
        continue;
    }

#undef START_BIT

post_process:
    RESET_BITAP_TABLE();
    return count;
}


static Py_ssize_t
MORASTR_SEARCH(bitap_mora_search_) (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices, Py_ssize_t count)
{
    MoraStr_assert(0 < p_len && count != 0);

    Py_ssize_t i = mora_off ? indices[mora_off-1] : 0LL;
    Py_ssize_t limit = s_len - p_len + 1;
    if (i >= limit) {return count;}

    uint32_t last_idx = (uint32_t)p_len - 1;
    BITAP_UINT_T bit_state;
    Py_ssize_t gap;
#define START_BIT() ((BITAP_UINT_T)1 << last_idx)
    for (uint32_t i = 0; i < last_idx; ++i) {
        Katakana k = p[i];
        BITAP_TABLE[CHAR_INDEX(k)] |= START_BIT() >> i;
    }
    Katakana last = p[last_idx];
    bit_state = BITAP_TABLE[CHAR_INDEX(last)];
    BITAP_TABLE[CHAR_INDEX(last)] |= 1;
    gap = bit_state ? TZCNT(bit_state) : p_len;

    const MINDEX_T *next_ptr = indices + mora_off;
    Py_ssize_t k = -1;
    while (i < limit) {
        BITAP_UINT_T bits;
        Katakana kana = s[i+last_idx];

        if (kana != last) {
            bits = BITAP_TABLE[CHAR_INDEX(kana)];
            do {
                i += bits ? TZCNT(bits) : last_idx + 1;
                if (i >= limit) {goto post_process;}
                Katakana prev = kana;
                kana = s[i+last_idx];
                if (prev == kana) {continue;}
                bits = BITAP_TABLE[CHAR_INDEX(kana)];
            } while (kana != last);
            Py_ssize_t imax = i;
            do {i = *next_ptr++;} while (i < imax);
            if (i != imax) {continue;}
        }

        uint32_t j;
        if (i > k) {
            if (last_idx && p[0] != s[i]) {goto nomatch;}
            bit_state = START_BIT();
            j = 1;
        } else {
            j = (uint32_t)(k - i) + 1;
        }
        if (next_ptr[p_moracnt-1] != MINDEX(i + last_idx + 1)) {
            i = *next_ptr++;
            continue;
        }
        bits = START_BIT();
        for (; j < last_idx; ++j) {
            bit_state = BITAP_NEXT_STATE(bit_state, START_BIT(), s[i+j]);
            if (!((bits >> j) & bit_state)) {goto partial_match;}
        }
        if (count == -1) {
            count = next_ptr - indices;
            goto post_process;
        }
        if (!(--count)) {break;}
        i += last_idx + 1;
        goto loop_end;

    nomatch:
        if (i + 1 >= limit) {break;}
        kana = s[i+last_idx+1];
        bits = BITAP_TABLE[CHAR_INDEX(kana)];
        i += bits ? gap : last_idx + 2;
        goto loop_end;

    partial_match:
        bits = bit_state >> (last_idx - j);
        if (bits) {
            k = (Py_ssize_t)(i + j);
            if (gap > j) {
                i += gap;
            } else {
                i += TZCNT(bits);
                last = p[j];
                while (i < limit && last != s[i+j]) {++i;}
                last = p[last_idx];
                if (i >= limit) {break;}
            }
        } else {
            i += (Py_ssize_t)Py_MAX(gap, j + 1);
        }
        if (i < limit) {
            Py_ssize_t imax = i;
            do {i = *next_ptr++;} while (i < imax);
        }
        if (i < limit) {continue;}

    loop_end:
        if (i >= limit) {break;}
        Py_ssize_t imax = i;
        do {i = *next_ptr++;} while (i < imax);
    }

#undef START_BIT

post_process:
    RESET_BITAP_TABLE();
    return count;
}

#undef BITAP_TABLE
#undef BITAP_NEXT_STATE
#undef RESET_BITAP_TABLE

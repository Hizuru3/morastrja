#include "cmorastr_twoway.h"

// size_t TWOWAY_TABLE_SIZE
// size_t CHAR_INDEX(Katakana ch)
// type TWOWAY_SSIZE_T
// TWOWAY_SSIZE_T TWOWAY_SSIZE_MAX


struct TwoWayNeedle {
    uint64_t cache_state;
    const Katakana *buffer;
    TWOWAY_SSIZE_T len;
    TWOWAY_SSIZE_T mora_cnt;
    Py_ssize_t suffix;
    Py_ssize_t period;
    Py_ssize_t gap;
    TWOWAY_SSIZE_T table[TWOWAY_TABLE_SIZE];
} two_way_needle;


static inline Py_ssize_t
critical_factorization(
    const Katakana *needle, Py_ssize_t length,
    Py_ssize_t *p, bool direction)
{
    Py_ssize_t suffix = 0, period = 1;
    Py_ssize_t j = 1, k = 0;
    while (j + k < length) {
        Katakana a = needle[j+k];
        Katakana b = needle[suffix+k];
        if (direction ? (b < a) : (a < b)) {
            j += k + 1;
            k = 0;
            period = j - suffix;
        } else if (a == b) {
            if (k + 1 != period) {
                ++k;
            } else {
                j += period;
                k = 0;
            }
        } else {
            suffix = j++;
            k = 0;
            period = 1;
        }
    }
    *p = period;
    return suffix;
}

static void
two_way_prepare_x(
    const Katakana *needle, Py_ssize_t length, Py_ssize_t mora_cnt)
{
    MoraStr_assert(length <= TWOWAY_SSIZE_MAX);
    if (two_way_needle.cache_state) {return;}

    Py_ssize_t suffix, period, suffix_r, period_r;
    suffix = critical_factorization(needle, length, &period, 0);
    suffix_r = critical_factorization(needle, length, &period_r, 1);

    if (suffix <= suffix_r) {
        suffix = suffix_r;
        period = period_r;
    }
    bool is_periodic = \
        !memcmp(needle, needle+period, sizeof(Katakana)*suffix);

    TWOWAY_SSIZE_T *table = two_way_needle.table;
    TWOWAY_SSIZE_T len = (TWOWAY_SSIZE_T)length, i;
    for (i = 0; i < TWOWAY_TABLE_SIZE; ++i) {
        table[i] = len;
    }
    TWOWAY_SSIZE_T gap;
    if (is_periodic) {
        for (i = 0; i < len; ++i) {
            Katakana k = needle[i];
            table[CHAR_INDEX(k)] = len - (i + 1);
        }
        gap = -1;
    } else {
        Katakana last = needle[len-1];
        table[CHAR_INDEX(last)] = 0;
        gap = len;
        for (i = 0; i + 1 < len; ++i) {
            Katakana k = needle[i];
            TWOWAY_SSIZE_T diff = len - (i + 1);
            table[CHAR_INDEX(k)] = diff;
            if (k == last) {gap = diff;}
        }
        period = Py_MAX(suffix, length - suffix) + 1;
    }
    two_way_needle.buffer = needle;
    two_way_needle.len = len;
    two_way_needle.mora_cnt = (TWOWAY_SSIZE_T)mora_cnt;
    two_way_needle.suffix = suffix;
    two_way_needle.period = period;
    two_way_needle.gap = gap;
}


static Py_ssize_t
two_way_repetition(
    const Katakana *s, Py_ssize_t s_len,
    Py_ssize_t mora_off, const MINDEX_T *indices)
{
    const Katakana *p = two_way_needle.buffer;
    Py_ssize_t p_len = two_way_needle.len;

    MoraStr_assert(p_len >= 2);
    Katakana h1, h2, t1, t2;
    Py_ssize_t last2_idx = p_len - 2;
    h1 = p[0]; h2 = p[1];
    if (p_len & 1) {
        t1 = h2; t2 = h1;
    } else {
        t1 = h1; t2 = h2;
    }

    Py_ssize_t limit = s_len - p_len + 1;
    if (!indices) {
        Py_ssize_t i = mora_off;
        while (i < limit) {
            if (t1 == s[i+last2_idx] && t2 == s[i+last2_idx+1]) {
                Py_ssize_t j = i, last = i + last2_idx;
                for (; i < last; i+=2) {
                    if (h1 == s[i] && h2 == s[i+1]) {continue;}
                    i += 1;
                    goto kana_loop_end;
                }
                return j;
            } else {
                i += last2_idx + 1;
            }
        kana_loop_end:
            continue;
        }
    } else {
        Py_ssize_t p_moracnt = two_way_needle.mora_cnt;
        Py_ssize_t i = mora_off ? indices[mora_off-1] : 0LL;
        const MINDEX_T *next_ptr = indices + mora_off;
        while (i < limit) {
            if (t1 == s[i+last2_idx] && t2 == s[i+last2_idx+1]) {
                if (next_ptr[p_moracnt-1] != MINDEX(i + p_len)) {
                    i = *next_ptr++;
                    continue;
                }
                Py_ssize_t last = i + last2_idx;
                for (; i < last; i+=2) {
                    if (h1 == s[i] && h2 == s[i+1]) {continue;}
                    i += 1;
                    goto mora_loop_end;
                }
                return next_ptr - indices;
            } else {
                i += last2_idx + 1;
            }
        mora_loop_end:
            if (i >= limit) {break;}
            Py_ssize_t imax = i;
            do {i = *next_ptr++;} while (i < imax);
        }
    }
    return -1;
}


static Py_ssize_t
two_way_katakana_findindex(
    const Katakana *s, Py_ssize_t s_len, Py_ssize_t mora_off)
{
    const Katakana *p = two_way_needle.buffer;
    Py_ssize_t suffix = two_way_needle.suffix;
    Py_ssize_t period = two_way_needle.period;
    if (suffix == 1 && period == 2) {
        return two_way_repetition(s, s_len, mora_off, NULL);
    }

    Py_ssize_t p_len = two_way_needle.len;
    Py_ssize_t gap = two_way_needle.gap;
    TWOWAY_SSIZE_T *table = two_way_needle.table;

    Py_ssize_t i, j = mora_off, limit = s_len - p_len + 1;
    TWOWAY_SSIZE_T shift;
    if (gap == -1) {
        Py_ssize_t memory;
        while (j < limit) {
            shift = table[CHAR_INDEX(s[j+p_len-1])];
            j += shift;
            if (shift) {goto periodic_loop_end;}
            memory = 0;
        noshift:
            i = Py_MAX(suffix, memory);
            for (; i < p_len; ++i) {
                if (s[i+j] != p[i]) {
                    j += i + 1 - suffix; //
                    goto periodic_loop_end;
                }
            }
            for (i = memory; i < suffix; ++i) {
                if (s[i+j] != p[i]) {
                    j += period;
                    if (j >= limit) {return -1;}
                    memory = p_len - period;
                    shift = table[CHAR_INDEX(p[j+p_len-1])];
                    if (shift) {
                        gap = Py_MAX(suffix, memory) - suffix + 1;
                        j += Py_MAX(shift, gap);
                        goto periodic_loop_end;
                    }
                    goto noshift;
                }
            }
            return j;

        periodic_loop_end:
            continue;
        }
    } else {
        period = Py_MAX(gap, period);
        Py_ssize_t mid = Py_MIN(p_len, suffix + gap);
        while (j < limit) {
            shift = table[CHAR_INDEX(s[j+p_len-1])];
            j += shift;
            if (shift) {goto nonperiodic_loop_end;}
            for (i = suffix; i < mid; ++i) {
                if (s[i+j] != p[i]) {
                    j += gap;
                    goto nonperiodic_loop_end;
                }
            }
            for (i = mid; i < p_len; ++i) {
                if (s[i+j] != p[i]) {
                    j += i + 1 - suffix; //
                    goto nonperiodic_loop_end;
                }
            }
            for (i = 0; i < suffix; ++i) {
                if (s[i+j] != p[i]) {
                    j += period;
                    goto nonperiodic_loop_end;
                }
            }
            return j;

        nonperiodic_loop_end:
            continue;
        }
    }
    return -1;
}

static Py_ssize_t
two_way_mora_findindex(
    const Katakana *s, Py_ssize_t s_len,
    Py_ssize_t mora_off, const MINDEX_T *indices)
{
    const Katakana *p = two_way_needle.buffer;
    Py_ssize_t suffix = two_way_needle.suffix;
    Py_ssize_t period = two_way_needle.period;
    if (suffix == 1 && period == 2) {
        return two_way_repetition(s, s_len, mora_off, indices);
    }

    Py_ssize_t p_len = two_way_needle.len;
    Py_ssize_t p_moracnt = two_way_needle.mora_cnt;
    Py_ssize_t gap = two_way_needle.gap;
    TWOWAY_SSIZE_T *table = two_way_needle.table;

    Py_ssize_t i, j = mora_off ? indices[mora_off-1] : 0LL;
    Py_ssize_t limit = s_len - p_len + 1;
    const MINDEX_T *next_ptr = indices + mora_off;
    TWOWAY_SSIZE_T shift;
    if (gap == -1) {
        Py_ssize_t memory;
    periodic_loop:
        while (j < limit) {
            shift = table[CHAR_INDEX(s[j+p_len-1])];
            j += shift;
            if (shift) {
                if (j >= limit) {break;}
                Py_ssize_t jmax = j;
                do {j = *next_ptr++;} while (j < jmax);
                if (j >= limit) {break;}
                continue;
            }
            memory = 0;
        noshift:
            if (next_ptr[p_moracnt-1] != MINDEX(j + p_len)) {
                j = *next_ptr++;
                continue;
            }

            i = Py_MAX(suffix, memory);
            for (; i < p_len; ++i) {
                if (s[i+j] != p[i]) {
                    j += i + 1 - suffix; //
                    goto periodic_loop_end;
                }
            }
            for (i = memory; i < suffix; ++i) {
                if (s[i+j] != p[i]) {
                    j += period;
                    if (j >= limit) {return -1;}
                    memory = p_len - period;
                    shift = table[CHAR_INDEX(p[j+p_len-1])];
                    if (shift) {
                        gap = Py_MAX(suffix, memory) - suffix + 1;
                        j += Py_MAX(shift, gap);
                        goto periodic_loop_end;
                    }
                    Py_ssize_t jmax = j;
                    do {j = *next_ptr++;} while (j < jmax);
                    if (j == jmax) {goto noshift;}
                    goto periodic_loop;
                }
            }
            return next_ptr - indices;

        periodic_loop_end:
            if (j >= limit) {break;}
            Py_ssize_t jmax = j;
            do {j = *next_ptr++;} while (j < jmax);
        }
    } else {
        period = Py_MAX(gap, period);
        Py_ssize_t mid = Py_MIN(p_len, suffix + gap);
        while (j < limit) {
            shift = table[CHAR_INDEX(s[j+p_len-1])];
            j += shift;
            if (shift) {
                if (j >= limit) {break;}
                Py_ssize_t jmax = j;
                do {j = *next_ptr++;} while (j < jmax);
                if (j >= limit) {break;}
                continue;
            }
            if (next_ptr[p_moracnt-1] != MINDEX(j + p_len)) {
                j = *next_ptr++;
                continue;
            }
            for (i = suffix; i < mid; ++i) {
                if (s[i+j] != p[i]) {
                    j += gap;
                    goto nonperiodic_loop_end;
                }
            }
            for (i = mid; i < p_len; ++i) {
                if (s[i+j] != p[i]) {
                    j += i + 1 - suffix; //
                    goto nonperiodic_loop_end;
                }
            }
            for (i = 0; i < suffix; ++i) {
                if (s[i+j] != p[i]) {
                    j += period;
                    goto nonperiodic_loop_end;
                }
            }
            return next_ptr - indices;

        nonperiodic_loop_end:
            if (j >= limit) {break;}
            Py_ssize_t jmax = j;
            do {j = *next_ptr++;} while (j < jmax);
        }
    }
    return -1;
}


static Py_ssize_t
two_way_search (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len,
    Py_ssize_t mora_off, Py_ssize_t count)
{
    MoraStr_assert(p_len);

    two_way_prepare_x(p, p_len, p_len);
    if (count == -1) {
        return two_way_katakana_findindex(s, s_len, mora_off);
    }
    while (count) {
        mora_off = two_way_katakana_findindex(s, s_len, mora_off);
        if (mora_off == -1) {break;}
        mora_off += p_len;
        --count;
    }
    return count;
}


static Py_ssize_t
two_way_mora_search (
    const Katakana *s, Py_ssize_t s_len,
    const Katakana *p, Py_ssize_t p_len, Py_ssize_t mora_off,
    Py_ssize_t p_moracnt, const MINDEX_T *indices, Py_ssize_t count)
{
    MoraStr_assert(p_len && p_moracnt);

    two_way_prepare_x(p, p_len, p_moracnt);
    if (count == -1) {
        return two_way_mora_findindex(s, s_len, mora_off, indices);
    }
    while (count) {
        mora_off = two_way_mora_findindex(s, s_len, mora_off, indices);
        if (mora_off == -1) {break;}
        mora_off += p_moracnt;
        --count;
    }
    return count;
}


static bool
two_way_prepare(
    const Katakana *needle, Py_ssize_t length, Py_ssize_t mora_cnt)
{
    if (length > TWOWAY_SSIZE_MAX) {return false;}
    two_way_prepare_x(needle, length, mora_cnt);
    two_way_needle.cache_state = 1;
    return true;
}


static void
two_way_set_needle(void *needle) {
    if (!needle) {
        two_way_needle.cache_state = 0;
        return;
    }
    two_way_needle = *((struct TwoWayNeedle *)needle);
    two_way_needle.cache_state = 1;
}


static void *
two_way_cache_new(void) {
    const size_t SIZE = sizeof(struct TwoWayNeedle);

    void *result;
    result = PyMem_Malloc(SIZE);
    if (!result) {return NULL;}
    memcpy(result, &two_way_needle, SIZE);
    return result;
}


static void
two_way_cache_dealloc(void *needle) {
    PyMem_Free(needle);
}
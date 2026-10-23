/*
 * ccr_search - finding text in C and C++ files, and replacing it.
 *
 * Text, not blocks: every place a file says it, with what kind of place
 * that is (code, a string, a comment), so "Hello" can be changed in the
 * strings only and not in a function called hello_world. No window, no
 * clang: tests drive it.
 */
#ifndef CCR_SEARCH_H
#define CCR_SEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where a match is. */
#define CCR_IN_CODE    1u   /* names, numbers, keywords, #include <x> */
#define CCR_IN_STRING  2u   /* "...", '.', R"(...)" */
#define CCR_IN_COMMENT 4u   /* // and */
#define CCR_IN_ALL     7u

typedef struct ccr_search_opts {
    bool     match_case;    /* else any case: Latin, Cyrillic */
    bool     whole_word;    /* not inside a longer name */
    unsigned where;         /* CCR_IN_* that count; 0: all */
} ccr_search_opts;

typedef struct ccr_search_hit {
    uint32_t offset, len;   /* bytes */
    int      line;          /* 1-based */
    unsigned where;         /* CCR_IN_* of its first byte */
} ccr_search_hit;

/* Every match of `query` in `text`, in order, none overlapping. How many
 * there are; the first `cap` go into `out`. */
int ccr_search_text(const char *text, size_t len, const char *query, const ccr_search_opts *o,
                    ccr_search_hit *out, int cap);

/* What `with` becomes in place of hit `h`:
 *   - `preserve_case`: HELLO -> HI, Hello -> Hi, hello -> hi;
 *   - in a string or a character, " ' \ and new lines are escaped, so the
 *     literal still ends where it did. */
void ccr_search_replacement(const char *text, const ccr_search_hit *h, const char *with, bool preserve_case,
                            char *out, size_t cap);

/* The text with every hit (in order, as ccr_search_text gave them)
 * replaced (malloc'd, NUL-terminated; its length in *out_len). */
char *ccr_search_replace(const char *text, size_t len, const ccr_search_hit *hits, int n, const char *with,
                         bool preserve_case, size_t *out_len);

/* The line the byte at `offset` is on, its leading blanks dropped and cut
 * to `cap`; *at gets where the offset is in `out` (-1: cut away). */
void ccr_search_line(const char *text, size_t len, uint32_t offset, char *out, size_t cap, int *at);

/* What every byte of `text` is (CCR_IN_*): `kinds` has `len` bytes. */
void ccr_search_regions(const char *text, size_t len, unsigned char *kinds);

#endif /* CCR_SEARCH_H */

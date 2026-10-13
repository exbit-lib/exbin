/* EXBIN internal: small backtracking regex engine (byte oriented).
 *
 * Syntax: literals . [set] [^set] ranges, \d \w \s \D \W \S \b \B, \n \t \r
 * \f \v \0 \xHH, escapes of metacharacters, groups (..) and (?:..),
 * alternation |, quantifiers * + ? {m} {m,} {m,n} with lazy '?' suffix,
 * anchors ^ $.  `.` matches any byte except '\n'.  \w includes every byte
 * >= 0x80, so UTF-8 identifiers (Arabic etc.) match \w+ and [\x80-\xff]+.
 */
#ifndef EXBIN_REGEX_H
#define EXBIN_REGEX_H

#include <stddef.h>

#define REGEX_MAX_GROUPS 32
#define REGEX_FLAG_ICASE 1

typedef struct Regex Regex;

Regex* regex_compile(const char* pat, size_t len, int flags, char* err, size_t errsz);
void regex_free(Regex* re);
int regex_group_count(const Regex* re); /* capture groups, excluding group 0 */

/* Anchored match at text[pos]. Returns match length or -1. caps receives
 * 2*(groups+1) offsets (start,end) or -1 when unset; maxcaps bounds writes. */
int regex_match_at(const Regex* re, const char* text, size_t n, size_t pos, int* caps, int maxcaps);
/* First match at or after `from`; returns start (or -1), length in *len. */
int regex_search(const Regex* re, const char* text, size_t n, size_t from, int* len, int* caps, int maxcaps);

#endif

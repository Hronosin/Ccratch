/*
 * ccr_yaml - the bit of YAML block packs need, and nothing more.
 *
 * Mappings and sequences, in blocks (indentation) and in flow style
 * ([a, b], {k: v}); plain, 'single' and "double" quoted scalars; | and >
 * text blocks; # comments. No anchors, tags, multiple documents or
 * implicit typing: every value is a string, the reader decides what it
 * means. Tabs for indentation are an error, as in YAML.
 */
#ifndef CCR_YAML_H
#define CCR_YAML_H

#include <stdbool.h>

typedef enum ccr_yaml_kind {
    CCR_YAML_NULL,      /* `key:` with nothing after or under it */
    CCR_YAML_SCALAR,
    CCR_YAML_MAP,
    CCR_YAML_SEQ
} ccr_yaml_kind;

typedef struct ccr_yaml_node {
    ccr_yaml_kind         kind;
    char                 *key;      /* in a mapping: the key; else NULL */
    char                 *value;    /* scalars */
    int                   line;     /* 1-based, for messages */
    struct ccr_yaml_node *first;    /* children of a mapping or sequence */
    struct ccr_yaml_node *next;
} ccr_yaml_node;

typedef struct ccr_yaml {
    ccr_yaml_node *root;
    char           error[256];   /* "line 12: ..." when parsing failed */
} ccr_yaml;

bool ccr_yaml_parse(const char *text, ccr_yaml *out);
bool ccr_yaml_load(const char *path, ccr_yaml *out);
void ccr_yaml_free(ccr_yaml *y);

/* A mapping's value for `key`, or NULL. */
const ccr_yaml_node *ccr_yaml_get(const ccr_yaml_node *map, const char *key);
/* Its scalar text, or `fallback` (also for a non-scalar or missing key). */
const char *ccr_yaml_str(const ccr_yaml_node *map, const char *key, const char *fallback);

#endif /* CCR_YAML_H */

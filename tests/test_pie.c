/* The pie menu picks by direction: no window needed. */
#include "ccr_pie.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

static void open_all(ccr_pie *pie)
{
    bool en[CCR_ACTION_COUNT];
    for (int i = 0; i < CCR_ACTION_COUNT; ++i)
        en[i] = true;
    ccr_pie_open(pie, (ccr_vec2){ 100.0f, 100.0f }, en, false);
}

int main(void)
{
    ccr_pie pie = { 0 };
    open_all(&pie);

    static const struct { float dx, dy; ccr_action want; } aims[] = {
        {   0, -60, CCR_ACTION_SAVE },      {  45, -45, CCR_ACTION_COMMENT },
        {  60,   0, CCR_ACTION_REDO },      {  45,  45, CCR_ACTION_EDIT_TEXT },
        {   0,  60, CCR_ACTION_DELETE },    { -45,  45, CCR_ACTION_FIT_VIEW },
        { -60,   0, CCR_ACTION_UNDO },      { -45, -45, CCR_ACTION_DUPLICATE },
        { 400, -30, CCR_ACTION_REDO },      /* far away still counts */
        {  10, -10, CCR_ACTION_NONE },      /* dead zone */
    };
    for (unsigned i = 0; i < sizeof aims / sizeof aims[0]; ++i) {
        ccr_action got = ccr_pie_aim(&pie, (ccr_vec2){ 100.0f + aims[i].dx, 100.0f + aims[i].dy });
        CHECK(got == aims[i].want, "aim %u: got %d, want %d", i, got, aims[i].want);
    }

    /* a click picks and closes */
    ccr_vec2 west = { 20.0f, 100.0f };
    CHECK(ccr_pie_update(&pie, west, false, false) == CCR_ACTION_NONE && pie.open, "moved = picked");
    CHECK(pie.hot == CCR_ACTION_UNDO, "west is not lit up");
    CHECK(ccr_pie_update(&pie, west, true, false) == CCR_ACTION_UNDO && !pie.open, "click did not pick");

    /* cancel */
    open_all(&pie);
    CHECK(ccr_pie_update(&pie, west, true, true) == CCR_ACTION_NONE && !pie.open, "cancel picked");

    /* a click in the middle closes without doing anything */
    open_all(&pie);
    CHECK(ccr_pie_update(&pie, (ccr_vec2){ 101.0f, 99.0f }, true, false) == CCR_ACTION_NONE &&
          !pie.open, "a click in the dead zone did something");

    /* disabled: lit, but not picked */
    bool en[CCR_ACTION_COUNT] = { false };
    ccr_pie_open(&pie, (ccr_vec2){ 100.0f, 100.0f }, en, true);
    CHECK(ccr_pie_update(&pie, (ccr_vec2){ 100.0f, 180.0f }, true, false) == CCR_ACTION_NONE,
          "a disabled slice was picked");
    CHECK(ccr_action_label(CCR_ACTION_COMMENT, true)[0] == 'U', "commented block: no Uncomment");

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("pie: all checks passed\n");
    return g_failures ? 1 : 0;
}

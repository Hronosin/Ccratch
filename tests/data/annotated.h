#ifndef ANNOTATED_H
#define ANNOTATED_H
/* A library that picks its blocks itself, like Qt's signals. */

typedef enum { BEEP_SHORT, BEEP_LONG } beep_kind;

/// @block beep {times} times
/// @block.ru пискнуть {times} раз
void beeper_beep(int times);

/** @block set pitch to {hz} Hz */
void beeper_pitch(int hz);

/// @block beep a {kind} one
void beeper_kind(beep_kind kind);

/// @block the beeper is busy
_Bool beeper_busy(void);

/* not marked: no block */
int beeper_internal_thing(void);
int _beeper_hidden(void);

static inline int beeper_twice(int x) { return 2 * x; }
#endif

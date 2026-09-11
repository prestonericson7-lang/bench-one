/* The struct lives here rather than in the .ino because the Arduino preprocessor inserts its
 * generated function prototypes immediately after the last #include, which puts them ABOVE
 * anything declared in the sketch body. A function returning a type defined later in the same
 * file then fails to compile with a misleading "does not name a type". Anything in a header is
 * already visible by then. */
#ifndef SPREAD_H
#define SPREAD_H
#include <stdint.h>

/* A distribution, not an average. An average hides the interruption that makes a node miss its
 * deadline, and the interruption is the only part that matters here. */
typedef struct {
    double   mean_us;
    double   p50_us;
    double   p99_us;
    double   worst_us;
    uint32_t over_2x;      /* compares that took more than twice the median */
} Spread;

#endif

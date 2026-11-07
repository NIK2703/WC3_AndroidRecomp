#ifndef ELEMENT_TYPE_H
#define ELEMENT_TYPE_H

#define MAX_BINDINGS_PER_ELEMENT 4
#define MAX_PETALS 4

typedef enum {
    BUTTON, D_PAD, RANGE_BUTTON, STICK, TRACKPAD
} ElementType;

typedef enum {
    CIRCLE, RECT
} ElementShape;

#endif

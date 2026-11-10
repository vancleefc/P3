#ifndef ARRAYLIST_H
#define ARRAYLIST_H

/* Growable array of strings. Owns (and frees) the strings pushed into it. */
typedef struct {
    char **data;
    unsigned length;
    unsigned capacity;
} arraylist_t;

void al_init(arraylist_t *L, unsigned capacity);
void al_push(arraylist_t *L, char *item);   /* takes ownership; item may be NULL */
void al_destroy(arraylist_t *L);            /* frees every item and the array */

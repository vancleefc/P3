#include <stdio.h>
#include <stdlib.h>
#include "arraylist.h"

void al_init(arraylist_t *L, unsigned capacity)
{
    if (capacity == 0) capacity = 8;
    L->data = malloc(capacity * sizeof(char *));
    if (!L->data) { perror("malloc"); exit(EXIT_FAILURE); }
    L->length = 0;
    L->capacity = capacity;
}

void al_push(arraylist_t *L, char *item)
{
    if (L->length == L->capacity) {
        L->capacity *= 2;
        char **p = realloc(L->data, L->capacity * sizeof(char *));
        if (!p) { perror("realloc"); exit(EXIT_FAILURE); }
        L->data = p;
    }
    L->data[L->length++] = item;
}

void al_destroy(arraylist_t *L)
{
    for (unsigned i = 0; i < L->length; i++) free(L->data[i]);
    free(L->data);
    L->data = NULL;
    L->length = L->capacity = 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "queue.h"
#include <stdlib.h>
#include <string.h>

void pq_free(pq_t *q){
    if(q->order != q->base) free(q->order);
    free(q->base);
    memset(q, 0, sizeof *q);
    q->cur = -1;
}

int pq_set(pq_t *q, int *base, int n, int pos){
    pq_free(q);
    if(n <= 0 || !base){ free(base); return n <= 0 ? 0 : -1; }
    q->base = q->order = base; q->n = n;
    q->cur = (pos >= 0 && pos < n) ? pos : 0;
    return 0;
}

int pq_shuffle(pq_t *q, int on, unsigned (*rnd)(unsigned k)){
    if(q->n <= 0){ q->shuffled = on; return 0; }
    int id = pq_id(q, q->cur);
    if(!on){
        if(q->order != q->base) free(q->order);
        q->order = q->base; q->shuffled = 0;
        q->cur = pq_find(q, id);
        if(q->cur < 0) q->cur = 0;
        return 0;
    }
    int *o = malloc((size_t)q->n * sizeof *o);
    if(!o) return -1;
    memcpy(o, q->base, (size_t)q->n * sizeof *o);
    /* the current song first, the rest a Fisher-Yates permutation */
    for(int i = 0; i < q->n; i++) if(o[i] == id){ o[i] = o[0]; o[0] = id; break; }
    for(int i = q->n - 1; i > 1; i--){
        int j = 1 + (int)rnd((unsigned)i);      /* 1..i */
        int t = o[i]; o[i] = o[j]; o[j] = t;
    }
    if(q->order != q->base) free(q->order);
    q->order = o; q->shuffled = 1; q->cur = 0;
    return 0;
}

static int cmp_int(const void *a, const void *b){ int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

int pq_adopt_order(pq_t *q, int *order, int n){
    if(!order || n != q->n || n <= 0){ free(order); return -1; }
    int *s = malloc((size_t)n * sizeof *s);
    if(!s){ free(order); return -1; }
    memcpy(s, order, (size_t)n * sizeof *s);
    qsort(s, (size_t)n, sizeof *s, cmp_int);
    int ok = 1;
    for(int i = 0; i < n && ok; i++) if(s[i] != q->base[i]) ok = 0;   /* base[] is in ID order, so sorted */
    free(s);
    if(!ok){ free(order); return -1; }
    int id = pq_id(q, q->cur);
    if(q->order != q->base) free(q->order);
    q->order = order; q->shuffled = 1;
    q->cur = pq_find(q, id);
    if(q->cur < 0) q->cur = 0;
    return 0;
}

int pq_find(const pq_t *q, int id){
    for(int i = 0; i < q->n; i++) if(q->order[i] == id) return i;
    return -1;
}

int pq_from_base(const pq_t *q, int pos){
    if(pos < 0 || pos >= q->n) return -1;
    return q->shuffled ? pq_find(q, q->base[pos]) : pos;
}

int pq_next(const pq_t *q, int mode, int end_of_song){
    if(q->n <= 0 || q->cur < 0) return -1;
    if(end_of_song){
        if(mode == PQ_REPEAT_ONE) return q->cur;
        if(mode == PQ_SINGLE) return -1;
        if(mode == PQ_IN_ORDER) return q->cur + 1 < q->n ? q->cur + 1 : -1;
    }
    return (q->cur + 1) % q->n;
}

int pq_prev(const pq_t *q){
    if(q->n <= 0) return -1;
    return q->cur <= 0 ? q->n - 1 : q->cur - 1;
}

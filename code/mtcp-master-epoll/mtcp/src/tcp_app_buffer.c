/*
 * TCP APP Queue- tcp_app_buffer.c/h
 *
 * Pijush Chakraborty
 *
 * Part of this code borrows Click's simple queue implementation
 *
 * ============================== Click License =============================
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "tcp_app_buffer.h"

#ifndef _INDEX_TYPE_
#define _INDEX_TYPE_
typedef uint32_t index_type;
typedef int32_t signed_index_type;
#endif
/*---------------------------------------------------------------------------*/
struct app_queue
{
	index_type _capacity;
	volatile index_type _head;
	volatile index_type _tail;

	app_info * volatile * q;
};
/*---------------------------------------------------------------------------*/
static inline index_type
NextIndex(app_queue_t sq, index_type i)
{
	return (i != sq->_capacity ? i + 1: 0);
}
/*---------------------------------------------------------------------------*/
int
AppQueueIsEmpty(app_queue_t sq)
{
	return (sq->_head == sq->_tail);
}
/*---------------------------------------------------------------------------*/
static inline void
AppMemoryBarrier(app_info * volatile app, volatile index_type index)
{
	__asm__ volatile("" : : "m" (app), "m" (index));
}
/*---------------------------------------------------------------------------*/
app_queue_t
CreateAppQueue(int capacity)
{
	app_queue_t sq;

	sq = (app_queue_t)calloc(1, sizeof(struct app_queue));
	if (!sq)
		return NULL;

	sq->q = (app_info **)calloc(capacity + 1, sizeof(app_info *));
	if (!sq->q) {
		free(sq);
		return NULL;
	}

	sq->_capacity = capacity;
	sq->_head = sq->_tail = 0;

	return sq;
}
/*---------------------------------------------------------------------------*/
void
DestroyAppQueue(app_queue_t sq)
{
	if (!sq)
		return;

	if (sq->q) {
		free((void *)sq->q);
		sq->q = NULL;
	}

	free(sq);
}
/*---------------------------------------------------------------------------*/
int
AppEnqueue(app_queue_t sq, app_info* app)
{
	index_type h = sq->_head;
	index_type t = sq->_tail;
	index_type nt = NextIndex(sq, t);

	if (nt != h) {
		sq->q[t] = app;
		AppMemoryBarrier(sq->q[t], sq->_tail);
		sq->_tail = nt;
		return 0;
	}

	return -1;
}
/*---------------------------------------------------------------------------*/
app_info *
AppDequeue(app_queue_t sq)
{
	index_type h = sq->_head;
	index_type t = sq->_tail;

	if (h != t) {
		app_info *app = sq->q[h];
		AppMemoryBarrier(sq->q[h], sq->_head);
		sq->_head = NextIndex(sq, h);
		assert(app);
		return app;
	}

	return NULL;
}
/*---------------------------------------------------------------------------*/

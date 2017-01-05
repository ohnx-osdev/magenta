// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

#include <list.h>
#include <kernel/thread.h>

/* scheduler routines */
void sched_init_early(void);
thread_t *sched_get_top_thread(int cpu);

void sched_block(void);
void sched_unblock(thread_t *t, bool resched);
void sched_unblock_list(struct list_node *list, bool resched);

void sched_yield(void);
void sched_preempt(void);

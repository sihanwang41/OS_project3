#ifndef _LINUX_ACCELERATION_H
#define _LINUX_ACCELERATION_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/wait.h>

#define WINDOW 20
#define WINDOW_POW2 32
#define NOISE 2

struct dev_acceleration {
	int x;
	int y;
	int z;
};

struct acc_motion {
	unsigned int dlt_x;
	unsigned int dlt_y;
	unsigned int dlt_z;

	unsigned int frq;
};

struct acc_delta {
	unsigned int dlt_x;
	unsigned int dlt_y;
	unsigned int dlt_z;

	bool noise;
};

/*
 * state == 0: event did not occur
 * state == 1: event did occur
 * state == -1: event destroyed
 * nr: number of processes on wq (state 0)
 *     or number of processes yet to wake up (state != 0)
 */
struct acc_event {
	int event_id;
	atomic_t state;
	atomic_t nr;
	struct acc_motion acc;
	wait_queue_head_t wq;
	struct list_head list;
};

#endif /* _LINUX_ACCELERATION_H */

/*  linux/kernel/acceleration.c
 *
 *  System calls to receive acceleration information from a user daemon
 *  and allow processes to create, wait on, and destroy acceleration
 *  events
 *
 */


#include <linux/acceleration.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <asm/uaccess.h>
#include <linux/kfifo.h>

static struct dev_acceleration dev_acc;
static DEFINE_SPINLOCK(dev_acc_lock);

static DEFINE_SPINLOCK(acc_buffer_lock);
static DEFINE_KFIFO(acc_buffer, struct dev_acceleration, WINDOW_POW2);

static atomic_t last_event_id = ATOMIC_INIT(0);
static LIST_HEAD(event_list);
static DEFINE_SPINLOCK(event_list_lock);

static inline int abs_diff(int x, int y)
{
	return (x > y ? x - y : y - x);
}

/*
 * Returns an event from the event list matching the
 * given id, or NULL if none is found
 * Caller must be holding event_list_lock
 */
static struct acc_event *find_event(int event_id)
{
	struct acc_event *acc_evt = NULL;
	struct acc_event *acc_evt_p;

	list_for_each_entry(acc_evt_p, &event_list, list) {
		if (acc_evt_p->event_id == event_id) {
			acc_evt = acc_evt_p;
			break;
		}
	}

	return acc_evt;
}

/*
 * Calculates the deltas in the acceleration data
 * received from user space.  Operates on a copy of the
 * acceleration data.
 */
static void calc_deltas(struct dev_acceleration acc_data[],
			struct acc_delta deltas[], int len)
{
	int i = 0;

	for (i = 0; i < len - 1; i++) {
		deltas[i].dlt_x = abs_diff(acc_data[i].x, acc_data[i + 1].x);
		deltas[i].dlt_y = abs_diff(acc_data[i].y, acc_data[i + 1].y);
		deltas[i].dlt_z = abs_diff(acc_data[i].z, acc_data[i + 1].z);

		if ((deltas[i].dlt_x + deltas[i].dlt_y + deltas[i].dlt_z)
				> NOISE * 100)
			deltas[i].noise = true;
		else
			deltas[i].noise = false;
	}
}

/*
 * Return 1 if an event has been trigggered and 0 otherwise
 */
static int test_event(struct acc_event *acc_evt,
			struct acc_delta deltas[], int len)
{
	int i;
	int frq = 0;

	for (i = 0; i < len - 1; i++) {
		if (deltas[i].noise == true &&
				deltas[i].dlt_x >= acc_evt->acc.dlt_x &&
				deltas[i].dlt_y >= acc_evt->acc.dlt_y &&
				deltas[i].dlt_z >= acc_evt->acc.dlt_z)
			frq++;
	}

	if (frq >= acc_evt->acc.frq)
		return 1;

	return 0;
}

/**
 * sys_set_acceleration - sets device acceleration info in the kernel
 */
SYSCALL_DEFINE1(set_acceleration,
		struct dev_acceleration __user *, acceleration)
{
	struct dev_acceleration new_acc;

	if (current->cred->uid != 0)
		return -EACCES;

	if (acceleration == NULL)
		return -EINVAL;

	if (copy_from_user(&new_acc, acceleration,
			sizeof(struct dev_acceleration)))
		return -EFAULT;

	spin_lock(&dev_acc_lock);
	dev_acc.x = new_acc.x;
	dev_acc.y = new_acc.y;
	dev_acc.z = new_acc.z;
	spin_unlock(&dev_acc_lock);

	return 0;
}

/*
 * sys_accevt_create - Create an event based on a motion.
 * Returns an event_id on success and appropriate error on failure.
 */
SYSCALL_DEFINE1(accevt_create,
		struct acc_motion __user *, acceleration)
{
	struct acc_motion user_acc;
	struct acc_event *acc_evt;
	int event_id;

	if (acceleration == NULL)
		return -EINVAL;

	if (copy_from_user(&user_acc, acceleration,
			sizeof(struct acc_motion)))
		return -EFAULT;

	acc_evt = kmalloc(sizeof(*acc_evt), GFP_KERNEL);
	if (!acc_evt)
		return -ENOMEM;

	event_id = atomic_inc_return(&last_event_id);
	if (find_event(event_id))
		return -ENOMEM;

	acc_evt->event_id = event_id;
	atomic_set(&acc_evt->state, 0);
	atomic_set(&acc_evt->nr, 0);
	acc_evt->acc = user_acc;
	acc_evt->acc.frq = acc_evt->acc.frq > WINDOW ?
				 WINDOW : acc_evt->acc.frq;
	init_waitqueue_head(&acc_evt->wq);

	spin_lock(&event_list_lock);
	list_add(&acc_evt->list, &event_list);
	spin_unlock(&event_list_lock);

	return acc_evt->event_id;
}

/*
 * sys_accevt_wait - Blocks a process on a motion event.
 * Returns 0 on success, -EAGAIN if the process is woken up
 * due to a signal or because the event was destroyed,
 * and appropriate error on failure.
 */
SYSCALL_DEFINE1(accevt_wait, int __user, event_id)
{
	int res;
	struct acc_event *acc_evt;

	spin_lock(&event_list_lock);

	acc_evt = find_event(event_id);
	if (!acc_evt) {
		spin_unlock(&event_list_lock);
		return -EINVAL;
	}

	spin_unlock(&event_list_lock);

	atomic_inc(&acc_evt->nr);
	res = wait_event_interruptible(acc_evt->wq,
			atomic_read(&acc_evt->state));

	if (atomic_dec_and_test(&acc_evt->nr))
		wake_up(&acc_evt->wq);

	if (atomic_read(&acc_evt->state) < 0)
		return -EAGAIN;

	if (res < 0)
		return -EAGAIN;

	return 0;
}

/*
 * sys_accevt_signal - Stores acceleration data from the user in the kernel.
 * Calculates deltas and wakes up processes waiting on triggered events.
 * Returns 0 on success and appropriate error on failure.
 */
SYSCALL_DEFINE1(accevt_signal,
		struct dev_acceleration __user *, acceleration)
{
	struct dev_acceleration kacceleration;
	struct dev_acceleration acc_data[WINDOW + 1];
	struct acc_delta deltas[WINDOW];
	struct acc_event *acc_evt = NULL;
	int len;

	if (acceleration == NULL)
		return -EINVAL;

	if (copy_from_user(&kacceleration, acceleration,
				sizeof(struct dev_acceleration)))
		return -EFAULT;

	spin_lock(&acc_buffer_lock);

	len = kfifo_len(&acc_buffer);

	if (len == WINDOW + 1)
		kfifo_skip(&acc_buffer);

	kfifo_put(&acc_buffer, &kacceleration);

	if (!kfifo_out_peek(&acc_buffer, acc_data, WINDOW + 1)) {
		spin_unlock(&acc_buffer_lock);
		return -EIO;
	}

	spin_unlock(&acc_buffer_lock);

	if (len > 1)
		calc_deltas(acc_data, deltas, len);

	spin_lock(&event_list_lock);
	list_for_each_entry(acc_evt, &event_list, list) {
		if (test_event(acc_evt, deltas, len)) {
			atomic_set(&acc_evt->state, 1);
			wake_up_all(&acc_evt->wq);
		} else {
			atomic_set(&acc_evt->state, 0);
		}
	}
	spin_unlock(&event_list_lock);

	return 0;
}

/*
 * sys_accevt_destroy - Destroy an event given the event_id
 * Returns 0 on success, -EAGAIN if the process is woken up
 * due to a signal, and appropriate error on failure.
 */
SYSCALL_DEFINE1(accevt_destroy, int, event_id)
{
	int res;
	struct acc_event *acc_evt;

	spin_lock(&event_list_lock);

	acc_evt = find_event(event_id);
	if (!acc_evt) {
		spin_unlock(&event_list_lock);
		return -EINVAL;
	}

	list_del(&acc_evt->list);
	spin_unlock(&event_list_lock);

	atomic_set(&acc_evt->state, -1);
	wake_up_all(&acc_evt->wq);

	/* Last process woken up from event will wake me to free memory */
	res = wait_event_interruptible(acc_evt->wq,
			atomic_read(&acc_evt->nr) == 0);

	if (res < 0)
		return -EAGAIN;

	kfree(acc_evt);

	return 0;
}

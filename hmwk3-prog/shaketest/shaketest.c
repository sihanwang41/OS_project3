#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/syscall.h"
#include "unistd.h"

#include "acceleration.h"

#define NUM_TESTS 3
#define TERMINATE_DELAY 60

struct acc_test {
	int event_id;
	char *msg;
	struct acc_motion acc;
};

static struct acc_test create_acc_test(int dlt_x, int dlt_y, int dlt_z,
					int frq, char *msg)
{
	struct acc_test test;

	test.acc.dlt_x = dlt_x;
	test.acc.dlt_y = dlt_y;
	test.acc.dlt_z = dlt_z;
	test.acc.frq = frq;
	test.msg = msg;

	return test;
}

int main(int argc, char **argv)
{
	int i;
	struct acc_test acc_tests[NUM_TESTS];
	pid_t pid;

	acc_tests[0] = create_acc_test(400, 0, 0, 4, "horizontal shake");
	acc_tests[1] = create_acc_test(0, 400, 0, 4, "vertical shake");
	acc_tests[2] = create_acc_test(400, 400, 0, 4, "shake");

	for (i = 0; i < NUM_TESTS; i++) {
		acc_tests[i].event_id = syscall(__NR_accevt_create,
						&acc_tests[i].acc);

		if (acc_tests[i].event_id < 0) {
			perror("error: Failed to create event");
			exit(1);
		}

		pid = fork();
		if (pid < 0) {
			perror("error: Fork failed");
			exit(1);
		} else if (pid == 0) {
			printf("Process %d waiting on event %d for a %s\n",
					getpid(), acc_tests[i].event_id,
					acc_tests[i].msg);

			if (syscall(__NR_accevt_wait,
					acc_tests[i].event_id) == 0)
				printf("%d detected a %s\n",
						getpid(),
						acc_tests[i].msg);

			exit(0);
		}
	}

	usleep(TERMINATE_DELAY * 1000000);

	for (i = 0; i < NUM_TESTS; i++) {
		printf("Destroying event %d\n", acc_tests[i].event_id);
		syscall(__NR_accevt_destroy, acc_tests[i].event_id);
	}

	return 0;
}

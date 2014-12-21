#ifndef _ACCELERATION_H
#define _ACCELERATION_H

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

#endif

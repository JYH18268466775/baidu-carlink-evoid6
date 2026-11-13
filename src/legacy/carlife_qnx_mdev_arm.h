#ifndef CARLIFE_QNX_MDEV_ARM_H
#define CARLIFE_QNX_MDEV_ARM_H

#include <sys/uio.h>

int carlink_mdev_arm_open(void);
int carlink_mdev_arm_close(int coid);
int carlink_mdev_arm_sendv(int coid, const iov_t *iov, int parts);

#endif

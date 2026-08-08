/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ECONET_XPON_BOARD_H_
#define _ECONET_XPON_BOARD_H_

#include <linux/types.h>

int xpon_board_register(void);
void xpon_board_unregister(void);
bool xpon_board_tx_disable_ready(void);
int xpon_board_set_tx_disable(bool disable);

#endif /* _ECONET_XPON_BOARD_H_ */

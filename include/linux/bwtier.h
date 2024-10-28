// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/types.h>

bool bwtier_enabled(void);
int bwtier_enable(void);
int bwtier_disable(void);
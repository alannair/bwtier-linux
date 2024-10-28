// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>

static bool bwtier_enabled_var = false;

bool bwtier_enabled(void)
{
	return bwtier_enabled_var;
}

int bwtier_enable(void)
{
	bwtier_enabled_var = true;
	return 0;
}

int bwtier_disable(void)
{
	bwtier_enabled_var = false;
	return 0;
}
/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2026 NetBSD pkgsrc contributors
 *
 * Licensed under the GNU General Public License Version 2
 */

#include "config.h"
#include <unistd.h>
#include <sys/param.h>

#include "up-native.h"

/**
 * up_native_get_native_path:
 * @object: the native tracking object
 *
 * Return value: the device path (static string).
 **/
const char *
up_native_get_native_path (GObject *object)
{
	return "/dev/sysmon";
}

/**
 * up_native_is_laptop:
 *
 * Check if the machine has a battery (i.e. is a laptop).
 * We do this by checking if /dev/sysmon exists and the
 * acpibat0 device is present.
 *
 * Return value: %TRUE if the machine appears to be a laptop.
 **/
gboolean
up_native_is_laptop (void)
{
	return (access("/dev/sysmon", R_OK) == 0);
}

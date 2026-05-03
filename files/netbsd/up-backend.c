/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2026 NetBSD pkgsrc contributors
 *
 * Licensed under the GNU General Public License Version 2
 *
 * NetBSD UPower backend using envsys(4) via proplib(3).
 * Reads battery and AC adapter state from /dev/sysmon using
 * the ENVSYS_GETDICTIONARY ioctl.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/envsys.h>
#include <paths.h>
#include <prop/proplib.h>

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "up-backend.h"
#include "up-backend-bsd-private.h"
#include "up-config.h"
#include "up-daemon.h"
#include "up-device.h"

#define UP_BACKEND_REFRESH_TIMEOUT	30	/* seconds */

static void	up_backend_class_init	(UpBackendClass	*klass);
static void	up_backend_init		(UpBackend	*backend);
static void	up_backend_finalize	(GObject	*object);

static gboolean	up_backend_update_battery	(UpDevice *device,
						 UpRefreshReason reason);
static gboolean	up_backend_update_ac		(UpDevice *device,
						 UpRefreshReason reason);

struct UpBackendPrivate
{
	UpDaemon	*daemon;
	UpDevice	*ac;
	UpDevice	*battery;
	UpConfig	*config;
	GDBusProxy	*seat_manager_proxy;
	guint		 poll_timer_id;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (UpBackend, up_backend, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/*  envsys helper: read the full sensor dictionary from /dev/sysmon    */
/* ------------------------------------------------------------------ */

static prop_dictionary_t
envsys_get_dictionary (void)
{
	prop_dictionary_t dict = NULL;
	int fd;

	fd = open (_PATH_SYSMON, O_RDONLY);
	if (fd < 0) {
		g_warning ("cannot open %s: %s",
			   _PATH_SYSMON, g_strerror (errno));
		return (NULL);
	}

	if (prop_dictionary_recv_ioctl (fd, ENVSYS_GETDICTIONARY,
	    &dict) != 0) {
		g_warning ("ENVSYS_GETDICTIONARY failed: %s",
			   g_strerror (errno));
		close (fd);
		return (NULL);
	}

	close (fd);
	return (dict);
}

/*
 * Find a sensor in a device array by its "description" string.
 * Returns the sensor's prop_dictionary, or NULL.
 */
static prop_dictionary_t
envsys_find_sensor (prop_array_t array, const char *description)
{
	prop_object_iterator_t iter;
	prop_object_t obj;
	prop_dictionary_t found = NULL;

	iter = prop_array_iterator (array);
	if (iter == NULL)
		return (NULL);

	while ((obj = prop_object_iterator_next (iter)) != NULL) {
		const char *desc;

		if (prop_object_type (obj) != PROP_TYPE_DICTIONARY)
			continue;
		if (!prop_dictionary_get_string (obj,
		    "description", &desc))
			continue;
		if (strcmp (desc, description) == 0) {
			found = obj;
			break;
		}
	}
	prop_object_iterator_release (iter);
	return (found);
}

/*
 * Read an integer value from a sensor dictionary's "cur-value" key.
 * Returns 0 if not found.
 */
static int64_t
envsys_sensor_value (prop_dictionary_t sensor)
{
	int64_t val = 0;

	if (sensor == NULL)
		return (0);
	prop_dictionary_get_int64 (sensor, "cur-value", &val);
	return (val);
}

/*
 * Read the "state" string from a sensor dictionary.
 */
static const char *
envsys_sensor_state (prop_dictionary_t sensor)
{
	const char *state = "invalid";

	if (sensor != NULL)
		prop_dictionary_get_string (sensor, "state", &state);
	return (state);
}

/* ------------------------------------------------------------------ */
/*  Read battery state from acpibat0 via envsys                        */
/* ------------------------------------------------------------------ */

static gboolean
up_backend_read_battery (UpDevice *device)
{
	prop_dictionary_t dict, sensor;
	prop_array_t bat_array;
	int64_t design_voltage, voltage;
	int64_t design_cap, last_full_cap, charge;
	int64_t charge_rate, discharge_rate;
	int64_t charging;
	gdouble energy, energy_full, energy_full_design;
	gdouble energy_rate, percentage, volts;
	UpDeviceState state;
	gboolean present;

	dict = envsys_get_dictionary ();
	if (dict == NULL)
		return (FALSE);

	bat_array = prop_dictionary_get (dict, "acpibat0");
	if (bat_array == NULL || prop_object_type (bat_array) !=
	    PROP_TYPE_ARRAY) {
		prop_object_release (dict);
		return (FALSE);
	}

	/* read sensor values (envsys units: µV, µAh, µA) */
	present = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "present")) != 0;

	design_voltage = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "design voltage"));
	voltage = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "voltage"));
	design_cap = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "design cap"));
	last_full_cap = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "last full cap"));
	charge = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "charge"));
	charge_rate = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "charge rate"));
	discharge_rate = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "discharge rate"));
	charging = envsys_sensor_value (
	    envsys_find_sensor (bat_array, "charging"));

	/* convert µV to V */
	volts = voltage / 1000000.0;
	if (volts <= 0.0)
		volts = design_voltage / 1000000.0;

	/* convert µAh * V = Wh */
	energy = (charge / 1000000.0) * volts;
	energy_full = (last_full_cap / 1000000.0) * volts;
	energy_full_design = (design_cap / 1000000.0) * volts;

	/* convert µA * V = W */
	if (charge_rate > 0)
		energy_rate = (charge_rate / 1000000.0) * volts;
	else if (discharge_rate > 0)
		energy_rate = (discharge_rate / 1000000.0) * volts;
	else
		energy_rate = 0.0;

	/* percentage */
	if (energy_full > 0.0)
		percentage = (energy / energy_full) * 100.0;
	else
		percentage = 0.0;
	if (percentage > 100.0)
		percentage = 100.0;
	if (percentage < 0.0)
		percentage = 0.0;

	/* determine state */
	if (!present) {
		state = UP_DEVICE_STATE_EMPTY;
	} else if (charging) {
		state = UP_DEVICE_STATE_CHARGING;
	} else if (discharge_rate > 0) {
		state = UP_DEVICE_STATE_DISCHARGING;
	} else {
		/* not charging, not discharging -> fully charged or idle */
		if (percentage >= 99.0)
			state = UP_DEVICE_STATE_FULLY_CHARGED;
		else
			state = UP_DEVICE_STATE_PENDING_CHARGE;
	}

	/* compute time-to-empty / time-to-full in seconds */
	gint64 time_to_empty = 0;
	gint64 time_to_full = 0;

	if (state == UP_DEVICE_STATE_DISCHARGING && energy_rate > 0.001)
		time_to_empty = (gint64)(3600.0 * energy / energy_rate);
	if (state == UP_DEVICE_STATE_CHARGING && energy_rate > 0.001)
		time_to_full = (gint64)(3600.0 *
		    (energy_full - energy) / energy_rate);

	g_object_set (device,
		      "is-present", present,
		      "is-rechargeable", TRUE,
		      "state", state,
		      "energy", energy,
		      "energy-empty", 0.0,
		      "energy-full", energy_full,
		      "energy-full-design", energy_full_design,
		      "energy-rate", energy_rate,
		      "voltage", volts,
		      "percentage", percentage,
		      "time-to-empty", time_to_empty,
		      "time-to-full", time_to_full,
		      "update-time",
		      (guint64)g_get_real_time () / G_USEC_PER_SEC,
		      (void *) NULL);

	prop_object_release (dict);
	return (TRUE);
}

/* ------------------------------------------------------------------ */
/*  Read AC adapter state from acpiacad0 via envsys                    */
/* ------------------------------------------------------------------ */

static gboolean
up_backend_read_ac (UpDevice *device)
{
	prop_dictionary_t dict, sensor;
	prop_array_t ac_array;
	gboolean online;

	dict = envsys_get_dictionary ();
	if (dict == NULL)
		return (FALSE);

	ac_array = prop_dictionary_get (dict, "acpiacad0");
	if (ac_array == NULL || prop_object_type (ac_array) !=
	    PROP_TYPE_ARRAY) {
		prop_object_release (dict);
		return (FALSE);
	}

	sensor = envsys_find_sensor (ac_array, "connected");
	online = (envsys_sensor_value (sensor) != 0);

	g_object_set (device,
		      "online", online,
		      "update-time",
		      (guint64)g_get_real_time () / G_USEC_PER_SEC,
		      (void *) NULL);

	prop_object_release (dict);
	return (TRUE);
}

/* ------------------------------------------------------------------ */
/*  Device refresh callbacks                                           */
/* ------------------------------------------------------------------ */

static gboolean
up_backend_update_battery (UpDevice *device, UpRefreshReason reason)
{
	return (up_backend_read_battery (device));
}

static gboolean
up_backend_update_ac (UpDevice *device, UpRefreshReason reason)
{
	return (up_backend_read_ac (device));
}

/* ------------------------------------------------------------------ */
/*  Poll timer                                                         */
/* ------------------------------------------------------------------ */

static gboolean
up_backend_poll_cb (gpointer data)
{
	UpBackend *backend = UP_BACKEND (data);

	if (backend->priv->battery != NULL)
		up_device_refresh_internal (backend->priv->battery,
		    UP_REFRESH_POLL);
	if (backend->priv->ac != NULL)
		up_device_refresh_internal (backend->priv->ac,
		    UP_REFRESH_POLL);
	return (G_SOURCE_CONTINUE);
}

/* ------------------------------------------------------------------ */
/*  Coldplug / unplug                                                  */
/* ------------------------------------------------------------------ */

gboolean
up_backend_coldplug (UpBackend *backend, UpDaemon *daemon)
{
	prop_dictionary_t dict;
	prop_array_t bat_array, ac_array;
	UpDeviceClass *device_class;
	GObject *native_bat, *native_ac;

	backend->priv->daemon = g_object_ref (daemon);

	/* check if we have battery/ac in envsys */
	dict = envsys_get_dictionary ();
	if (dict == NULL) {
		g_debug ("no envsys dictionary, no devices");
		return (TRUE);
	}

	bat_array = prop_dictionary_get (dict, "acpibat0");
	ac_array = prop_dictionary_get (dict, "acpiacad0");

	/* set up AC device */
	if (ac_array != NULL) {
		native_ac = g_object_new (G_TYPE_OBJECT, NULL);
		backend->priv->ac = UP_DEVICE (
		    up_device_new (daemon, native_ac));
		g_object_unref (native_ac);

		device_class = UP_DEVICE_GET_CLASS (backend->priv->ac);
		device_class->refresh = up_backend_update_ac;

		g_object_set (backend->priv->ac,
			      "type", UP_DEVICE_KIND_LINE_POWER,
			      "power-supply", TRUE,
			      "online", TRUE,
			      (void *) NULL);

		up_backend_read_ac (backend->priv->ac);

		if (!g_initable_init (G_INITABLE (backend->priv->ac),
		    NULL, NULL))
			g_warning ("failed to coldplug ac");
		else
			g_signal_emit (backend,
			    signals[SIGNAL_DEVICE_ADDED], 0,
			    backend->priv->ac);
	}

	/* set up battery device */
	if (bat_array != NULL) {
		native_bat = g_object_new (G_TYPE_OBJECT, NULL);
		backend->priv->battery = UP_DEVICE (
		    up_device_new (daemon, native_bat));
		g_object_unref (native_bat);

		device_class = UP_DEVICE_GET_CLASS (
		    backend->priv->battery);
		device_class->refresh = up_backend_update_battery;

		g_object_set (backend->priv->battery,
			      "type", UP_DEVICE_KIND_BATTERY,
			      "power-supply", TRUE,
			      "is-present", TRUE,
			      "is-rechargeable", TRUE,
			      "has-history", TRUE,
			      "has-statistics", TRUE,
			      (void *) NULL);

		up_backend_read_battery (backend->priv->battery);

		if (!g_initable_init (
		    G_INITABLE (backend->priv->battery), NULL, NULL))
			g_warning ("failed to coldplug battery");
		else
			g_signal_emit (backend,
			    signals[SIGNAL_DEVICE_ADDED], 0,
			    backend->priv->battery);
	}

	prop_object_release (dict);

	/* start polling */
	backend->priv->poll_timer_id = g_timeout_add_seconds (
	    UP_BACKEND_REFRESH_TIMEOUT, up_backend_poll_cb, backend);

	return (TRUE);
}

void
up_backend_unplug (UpBackend *backend)
{
	if (backend->priv->poll_timer_id > 0) {
		g_source_remove (backend->priv->poll_timer_id);
		backend->priv->poll_timer_id = 0;
	}
	if (backend->priv->daemon != NULL) {
		g_object_unref (backend->priv->daemon);
		backend->priv->daemon = NULL;
	}
}

/* ------------------------------------------------------------------ */
/*  Seat manager proxy accessors (for bsd/up-backend-common.c)         */
/* ------------------------------------------------------------------ */

GDBusProxy *
up_backend_get_seat_manager_proxy (UpBackend *backend)
{
	g_return_val_if_fail (UP_IS_BACKEND (backend), NULL);
	return (backend->priv->seat_manager_proxy);
}

UpConfig *
up_backend_get_config (UpBackend *backend)
{
	g_return_val_if_fail (UP_IS_BACKEND (backend), NULL);
	return (backend->priv->config);
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

static void
up_backend_class_init (UpBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_backend_finalize;

	signals[SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
		    G_TYPE_FROM_CLASS (object_class),
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (UpBackendClass, device_added),
		    NULL, NULL, NULL,
		    G_TYPE_NONE, 1, UP_TYPE_DEVICE);
	signals[SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
		    G_TYPE_FROM_CLASS (object_class),
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (UpBackendClass, device_removed),
		    NULL, NULL, NULL,
		    G_TYPE_NONE, 1, UP_TYPE_DEVICE);
}

static void
up_backend_init (UpBackend *backend)
{
	backend->priv = up_backend_get_instance_private (backend);
	backend->priv->config = up_config_new ();
	backend->priv->seat_manager_proxy =
	    g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, NULL,
		CONSOLEKIT2_DBUS_NAME,
		CONSOLEKIT2_DBUS_PATH,
		CONSOLEKIT2_DBUS_INTERFACE,
		NULL, NULL);
}

static void
up_backend_finalize (GObject *object)
{
	UpBackend *backend;

	g_return_if_fail (UP_IS_BACKEND (object));
	backend = UP_BACKEND (object);

	if (backend->priv->poll_timer_id > 0)
		g_source_remove (backend->priv->poll_timer_id);
	g_object_unref (backend->priv->config);
	if (backend->priv->daemon != NULL)
		g_object_unref (backend->priv->daemon);
	if (backend->priv->battery != NULL)
		g_object_unref (backend->priv->battery);
	if (backend->priv->ac != NULL)
		g_object_unref (backend->priv->ac);
	g_clear_object (&backend->priv->seat_manager_proxy);

	G_OBJECT_CLASS (up_backend_parent_class)->finalize (object);
}

UpBackend *
up_backend_new (void)
{
	return (g_object_new (UP_TYPE_BACKEND, NULL));
}

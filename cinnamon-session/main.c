/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Novell, Inc.
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Suite 500, Boston, MA
 * 02110-1335, USA.
 */

#include <config.h>

#include <libintl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <glib/gi18n.h>
#include <glib-unix.h>
#include <glib.h>
#include <gio/gio.h>

#include "mdm-log.h"

#include "csm-util.h"
#include "csm-manager.h"
#include "csm-session-fill.h"
#include "csm-store.h"
#include "csm-system.h"
#include <systemd/sd-journal.h>

#define CSM_DBUS_NAME "org.gnome.SessionManager"

static gboolean failsafe = FALSE;
static gboolean show_version = FALSE;
static gboolean debug = FALSE;
static gboolean please_fail = FALSE;
static const char *session_name = NULL;


static CsmManager *manager = NULL;

static GMainLoop *loop;

void
csm_quit (void)
{
        if (loop != NULL)
                g_main_loop_quit (loop);
        else
                exit(0);
}

static void
csm_main (void)
{
        if (loop == NULL)
                loop = g_main_loop_new (NULL, TRUE);

        g_main_loop_run (loop);
}


static void
on_name_lost (GDBusConnection *connection,
              const char *name,
              gpointer    data)
{
        if (connection == NULL) {
                csm_util_init_error (TRUE, "Lost name on bus: org.gnome.SessionManager");
        } else {
                g_debug ("Calling name lost callback function");

                /*
                 * When the signal handler gets a shutdown signal, it calls
                 * this function to inform CsmManager to not restart
                 * applications in the off chance a handler is already queued
                 * to dispatch following the below call to csm_quit.
                 */
                csm_manager_set_phase (manager, CSM_MANAGER_PHASE_EXIT);

                csm_quit ();
        }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char *name,
                  gpointer data)
{
        g_debug ("main: Name acquired");

        csm_manager_start (manager);
}

static gboolean
term_or_int_signal_cb (gpointer data)
{
        CsmManager *manager = (CsmManager *)data;

        /* let the fatal signals interrupt us */
        g_debug ("Caught SIGINT/SIGTERM, shutting down normally.");

        csm_manager_logout (manager, CSM_MANAGER_LOGOUT_MODE_FORCE, NULL);

        return FALSE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char *name,
                 gpointer data)
{
        CsmStore *client_store;

        g_debug ("main: Bus acquired");

        client_store = csm_store_new ();

        manager = csm_manager_new (client_store, failsafe);

        g_object_unref (client_store);

        g_unix_signal_add (SIGTERM, term_or_int_signal_cb, manager);
        g_unix_signal_add (SIGINT, term_or_int_signal_cb, manager);

        if (IS_STRING_EMPTY (session_name))
                session_name = _csm_manager_get_default_session (manager);

        if (!csm_session_fill (manager, session_name)) {
                csm_util_init_error (TRUE, "Failed to load session \"%s\"", session_name ? session_name : "(null)");
        }
}

static gboolean
acquire_name (void)
{
        return g_bus_own_name (G_BUS_TYPE_SESSION,
                               CSM_DBUS_NAME,
                               G_BUS_NAME_OWNER_FLAGS_NONE,
                               on_bus_acquired,
                               on_name_acquired,
                               on_name_lost,
                               NULL, NULL);
}

static gboolean
require_dbus_session (int      argc,
                      char   **argv,
                      GError **error)
{
        char **new_argv;
        int    i;

        if (g_getenv ("DBUS_SESSION_BUS_ADDRESS"))
                return TRUE;

        /* Just a sanity check to prevent infinite recursion if
         * dbus-launch fails to set DBUS_SESSION_BUS_ADDRESS 
         */
        g_return_val_if_fail (!g_str_has_prefix (argv[0], "dbus-launch"),
                              TRUE);

        /* +2 for our new arguments, +1 for NULL */
        new_argv = g_malloc ((argc + 3) * sizeof (*argv));

        new_argv[0] = "dbus-launch";
        new_argv[1] = "--exit-with-session";
        for (i = 0; i < argc; i++) {
                new_argv[i + 2] = argv[i];
	}
        new_argv[i + 2] = NULL;
        
        if (!execvp ("dbus-launch", new_argv)) {
                g_set_error (error, 
                             G_SPAWN_ERROR,
                             G_SPAWN_ERROR_FAILED,
                             "No session bus and could not exec dbus-launch: %s",
                             g_strerror (errno));
                return FALSE;
        }

        /* Should not be reached */
        return TRUE;
}

int
main (int argc, char **argv)
{
        struct sigaction  sa;
        GError           *error;
        guint             name_owner_id;
        static char     **override_autostart_dirs = NULL;
        GOptionContext   *options;
        static GOptionEntry entries[] = {
                { "autostart", 'a', 0, G_OPTION_ARG_STRING_ARRAY, &override_autostart_dirs, N_("Override standard autostart directories"), N_("AUTOSTART_DIR") },
                { "session", 0, 0, G_OPTION_ARG_STRING, &session_name, N_("Session to use"), N_("SESSION_NAME") },
                { "debug", 0, 0, G_OPTION_ARG_NONE, &debug, N_("Enable debugging code"), NULL },
                { "failsafe", 'f', 0, G_OPTION_ARG_NONE, &failsafe, N_("Do not load user-specified applications"), NULL },
                { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Version of this application"), NULL },
                /* Translators: the 'fail whale' is the black dialog we show when something goes seriously wrong */
                { "whale", 0, 0, G_OPTION_ARG_NONE, &please_fail, N_("Show the fail whale dialog for testing"), NULL },
                { NULL, 0, 0, 0, NULL, NULL, NULL }
        };

        GSettings *settings;
        settings = g_settings_new ("org.cinnamon.SessionManager");

        if (g_settings_get_boolean (settings, "x-sync")) {
            csm_util_setenv ("GDK_SYNCHRONIZE", "1");
        }

        /* Make sure that we have a session bus */
        if (!require_dbus_session (argc, argv, &error)) {
                csm_util_init_error (TRUE, "%s", error->message);
        }

        bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
        textdomain (GETTEXT_PACKAGE);

        if (g_settings_get_boolean (settings, "debug")) {
            debug = TRUE;
        }
        g_clear_object (&settings);

        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;
        sigemptyset (&sa.sa_mask);
        sigaction (SIGPIPE, &sa, 0);

        error = NULL;
        options = g_option_context_new (_(" - the Cinnamon session manager"));
        g_option_context_add_main_entries (options, entries, GETTEXT_PACKAGE);
        g_option_context_parse (options, &argc, &argv, &error);

        if (error != NULL) {
                g_warning ("%s", error->message);
                exit (1);
        }

        g_option_context_free (options);

        if (show_version) {
                g_print ("%s %s\n", argv [0], VERSION);
                exit (1);
        }

        if (please_fail) {
                csm_util_init_error (TRUE, "Testing the fail whale");
        }

        csm_util_export_activation_environment (NULL);
        csm_util_export_user_environment (NULL);

        mdm_log_init ();
        mdm_log_set_debug (debug);

        /* Some third-party programs rely on GNOME_DESKTOP_SESSION_ID to
         * detect if GNOME is running. We keep this for compatibility reasons.
         */
        csm_util_setenv ("GNOME_DESKTOP_SESSION_ID", "this-is-deprecated");

        csm_util_set_autostart_dirs (override_autostart_dirs);

        /* Talk to logind before acquiring a name, since it does synchronous
         * calls at initialization time that invoke a main loop and if we
         * already owned a name, then we would service too early during
         * that main loop.
         */
        g_object_unref (csm_get_system ());

        name_owner_id = acquire_name ();

        csm_main ();

        if (manager != NULL) {
                g_debug ("Unreffing manager");
                g_object_unref (manager);
        }

        g_bus_unown_name (name_owner_id);

        mdm_log_shutdown ();

        return 0;
}

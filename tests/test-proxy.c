/*
 * Copyright © 2018 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "backport-autoptr.h"

#define DBUS_SERVICE_DBUS "org.freedesktop.DBus"
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"
#define DBUS_INTERFACE_DBUS "org.freedesktop.DBus"

#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1
#define DBUS_REQUEST_NAME_REPLY_IN_QUEUE 2
#define DBUS_REQUEST_NAME_REPLY_EXISTS 3
#define DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER 4

#define CANNOT_ACCESS_NAME "com.example.CannotAccess"
#define CAN_SEE_NAME "com.example.CanSee"
#define CAN_TALK_NAME "com.example.CanTalk"
#define CAN_OWN_NAME "com.example.CanOwn"
#define CAN_CALL_ANYTHING_NAME "com.example.CanCallAny"
#define CAN_CALL_SOME_NAME "com.example.CanCallSome"
#define CAN_RECEIVE_ANYTHING_NAME "com.example.CanReceiveAny"
#define CAN_RECEIVE_SOME_NAME "com.example.CanReceiveSome"

#define EXAMPLE_IFACE "net.example.AnyInterface"
#define EXAMPLE_METHOD "Echo"
#define EXAMPLE_SIGNAL "Shouted"
#define EXAMPLE_PATH "/path"
#define CAN_CALL_SOME_IFACE "org.example.CanCallThis"
#define CAN_CALL_SOME_METHOD "OnlyThisMethod"
#define CAN_CALL_SOME_PATH "/only/this/path"
#define CAN_RECEIVE_SOME_IFACE "org.example.CanReceiveThis"
#define CAN_RECEIVE_SOME_SIGNAL "JustThisSignal"
#define CAN_RECEIVE_SOME_PATH "/just/this/path"

typedef struct
{
  GDBusConnection *conn;
  const char *unique_name;
} Connection;

static void
connection_clear (Connection *self)
{
  if (self->conn != NULL)
    {
      g_autoptr(GError) error = NULL;

      g_dbus_connection_close_sync (self->conn, NULL, &error);

      if (error != NULL)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
    }

  self->unique_name = NULL;
  g_clear_object (&self->conn);
}

typedef struct
{
  Connection proxied;
  Connection cannot_access_conn;
  Connection can_see_conn;
  Connection can_talk_conn;
  Connection can_own_conn;
  Connection can_call_anything_conn;
  Connection can_call_some_conn;
  Connection can_receive_anything_conn;
  Connection can_receive_some_conn;
  GSubprocess *dbus_daemon;
  GSubprocess *monitor;
  GSubprocess *proxy;
  gchar *dbus_address;
  gchar *temp_directory;
  gchar *proxy_socket;
  gchar *proxy_address;
  const gchar *proxy_path;
  int sync_pipe;
} Fixture;

typedef struct
{
  int dummy;
} Config;

/*
 * Open a direct connection to the bus
 */
static void
fixture_connect (Fixture *f,
                 Connection *conn,
                 const char *name)
{
  g_autoptr(GError) error = NULL;

  g_return_if_fail (conn != NULL);
  g_return_if_fail (conn->conn == NULL);
  conn->conn = g_dbus_connection_new_for_address_sync (f->dbus_address,
                                                       (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                        | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                       NULL,    /* observer */
                                                       NULL,    /* cancellable */
                                                       &error);
  g_assert_no_error (error);
  g_assert_nonnull (conn->conn);
  conn->unique_name = g_dbus_connection_get_unique_name (conn->conn);

  if (name != NULL)
    {
      g_autoptr(GVariant) tuple = NULL;
      guint32 result = 0;

      tuple = g_dbus_connection_call_sync (conn->conn,
                                           DBUS_SERVICE_DBUS,
                                           DBUS_PATH_DBUS,
                                           DBUS_INTERFACE_DBUS,
                                           "RequestName",
                                           g_variant_new ("(su)",
                                                           name,
                                                           (G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT
                                                           | G_BUS_NAME_OWNER_FLAGS_REPLACE
                                                           | G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE)),
                                           G_VARIANT_TYPE ("(u)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           NULL,    /* cancellable */
                                           &error);
      g_assert_no_error (error);
      g_assert_nonnull (tuple);
      g_variant_get (tuple, "(u)", &result);
      g_assert_cmpuint (result, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    }
}

static void
setup (Fixture *f,
       gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  GInputStream *address_pipe;
  gchar address_buffer[4096] = { 0 };
  g_autofree gchar *escaped = NULL;
  char *newline;

  f->sync_pipe = -1;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  f->dbus_daemon = g_subprocess_launcher_spawn (launcher, &error,
                                                "dbus-daemon",
                                                "--session",
                                                "--print-address=1",
                                                "--nofork",
                                                NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->dbus_daemon);

  address_pipe = g_subprocess_get_stdout_pipe (f->dbus_daemon);

  /* Crash if it takes too long to get the address */
  alarm (30);

  while (strchr (address_buffer, '\n') == NULL)
    {
      if (strlen (address_buffer) >= sizeof (address_buffer) - 1)
        g_error ("Read %" G_GSIZE_FORMAT " bytes from dbus-daemon with "
                 "no newline",
                 sizeof (address_buffer) - 1);

      g_input_stream_read (address_pipe,
                           address_buffer + strlen (address_buffer),
                           sizeof (address_buffer) - strlen (address_buffer),
                           NULL, &error);
      g_assert_no_error (error);
    }

  /* Disable alarm */
  alarm (0);

  newline = strchr (address_buffer, '\n');
  g_assert_nonnull (newline);
  *newline = '\0';
  f->dbus_address = g_strdup (address_buffer);

  if (g_getenv ("TEST_DBUS_MONITOR") != NULL)
    {
      g_autoptr(GSubprocessLauncher) monitor_launcher = NULL;

      monitor_launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
      g_subprocess_launcher_take_stdout_fd (monitor_launcher, dup (STDERR_FILENO));
      f->monitor = g_subprocess_launcher_spawn (monitor_launcher, NULL,
                                                "dbus-monitor",
                                                "--address", f->dbus_address,
                                                NULL);
    }

  f->proxy_path = g_getenv ("DBUS_PROXY");

  if (f->proxy_path == NULL)
    f->proxy_path = BINDIR "/xdg-dbus-proxy";

  f->temp_directory = g_dir_make_tmp ("xdg-dbus-proxy-test.XXXXXX", &error);
  g_assert_no_error (error);
  f->proxy_socket = g_build_filename (f->temp_directory, "proxy", NULL);
  escaped = g_dbus_address_escape_value (f->proxy_socket);
  f->proxy_address = g_strdup_printf ("unix:path=%s", escaped);

  fixture_connect (f, &f->cannot_access_conn, CANNOT_ACCESS_NAME);
  fixture_connect (f, &f->can_see_conn, CAN_SEE_NAME);
  fixture_connect (f, &f->can_talk_conn, CAN_TALK_NAME);
  fixture_connect (f, &f->can_own_conn, CAN_OWN_NAME);
  fixture_connect (f, &f->can_call_anything_conn, CAN_CALL_ANYTHING_NAME);
  fixture_connect (f, &f->can_call_some_conn, CAN_CALL_SOME_NAME);
  fixture_connect (f, &f->can_receive_anything_conn, CAN_RECEIVE_ANYTHING_NAME);
  fixture_connect (f, &f->can_receive_some_conn, CAN_RECEIVE_SOME_NAME);
}

enum
{
  READ_END = 0,
  WRITE_END = 1,
  PIPE_FDS
};

static void
fixture_start_proxy (Fixture *f)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  int sync_pipe[PIPE_FDS];
  char buf;
  ssize_t bytes_read;

#if GLIB_CHECK_VERSION (2, 78, 0)
  g_unix_open_pipe (sync_pipe, O_CLOEXEC, &error);
#else
  g_unix_open_pipe (sync_pipe, FD_CLOEXEC, &error);
#endif
  g_assert_no_error (error);
  f->sync_pipe = sync_pipe[READ_END];

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_take_fd (launcher, dup (STDERR_FILENO), STDOUT_FILENO);
  g_subprocess_launcher_take_fd (launcher, sync_pipe[WRITE_END], 3);
  sync_pipe[WRITE_END] = -1;

  f->proxy = g_subprocess_launcher_spawn (launcher, &error,
                                          f->proxy_path,
                                          "--fd=3",
                                          f->dbus_address,
                                          f->proxy_socket,
                                          "--filter",
                                          "--log",
                                          "--see=" CAN_SEE_NAME,
                                          "--talk=" CAN_TALK_NAME,
                                          "--own=" CAN_OWN_NAME,
                                          "--call=" CAN_CALL_ANYTHING_NAME "=*",
                                          "--call=" CAN_CALL_SOME_NAME "=" CAN_CALL_SOME_IFACE "." CAN_CALL_SOME_METHOD "@" CAN_CALL_SOME_PATH,
                                          "--broadcast=" CAN_RECEIVE_ANYTHING_NAME "=*",
                                          "--broadcast=" CAN_RECEIVE_SOME_NAME "=" CAN_RECEIVE_SOME_IFACE "." CAN_RECEIVE_SOME_SIGNAL "@" CAN_RECEIVE_SOME_PATH,
                                          NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->proxy);

  /* Wait for the proxy to be ready */
  bytes_read = read (sync_pipe[READ_END], &buf, 1);
  g_assert_cmpint (bytes_read, ==, 1);

  f->proxied.conn = g_dbus_connection_new_for_address_sync (f->proxy_address,
                                                            (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                             | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                            NULL,    /* observer */
                                                            NULL,    /* cancellable */
                                                            &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->proxied.conn);
  f->proxied.unique_name = g_dbus_connection_get_unique_name (f->proxied.conn);
}

static void
test_basics (Fixture *f,
             gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) tuple = NULL;
  g_auto(GStrv) strv = NULL;
  gsize i;
  gboolean found;

  alarm (30);

  fixture_start_proxy (f);

  tuple = g_dbus_connection_call_sync (f->proxied.conn, DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                                       "ListNames", NULL,
                                       G_VARIANT_TYPE ("(as)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                       &error);
  g_assert_no_error (error);
  g_assert_nonnull (tuple);
  g_variant_get (tuple, "(^as)", &strv);
  found = FALSE;

  /* As a simple test of the proxying, assert that the array contains the
   * proxied connection itself */
  for (i = 0; strv[i] != NULL; i++)
    {
      g_test_message ("ListNames(): %s", strv[i]);

      if (g_strcmp0 (strv[i], f->proxied.unique_name) == 0)
        found = TRUE;
    }

  g_assert_true (found);
}

typedef struct
{
  const char *name;
  gboolean can_own;
  gboolean can_see;
} OwnTest;

static const OwnTest own_tests[] =
{
  { CANNOT_ACCESS_NAME, .can_own = FALSE, .can_see = FALSE },
  { CAN_SEE_NAME, .can_own = FALSE, .can_see = TRUE },
  { CAN_TALK_NAME, .can_own = FALSE, .can_see = TRUE },
  { CAN_OWN_NAME, .can_own = TRUE, .can_see = TRUE },
  { CAN_CALL_ANYTHING_NAME, .can_own = FALSE, .can_see = TRUE },
  { CAN_CALL_SOME_NAME, .can_own = FALSE, .can_see = TRUE },
  { CAN_RECEIVE_ANYTHING_NAME, .can_own = FALSE, .can_see = TRUE },
  { CAN_RECEIVE_SOME_NAME, .can_own = FALSE, .can_see = TRUE },
};

static void
test_own (Fixture *f,
          gconstpointer context G_GNUC_UNUSED)
{
  alarm (30);
  fixture_start_proxy (f);

  for (size_t i = 0; i < G_N_ELEMENTS (own_tests); i++)
    {
      const OwnTest *t = &own_tests[i];
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) tuple = NULL;
      const char *owner = NULL;

      g_test_message ("#%zu: sandboxed connection %s be allowed to own %s",
                      i, t->can_own ? "should" : "should not", t->name);

      tuple = g_dbus_connection_call_sync (f->proxied.conn,
                                           DBUS_SERVICE_DBUS,
                                           DBUS_PATH_DBUS,
                                           DBUS_INTERFACE_DBUS,
                                           "RequestName",
                                           g_variant_new ("(su)",
                                                           t->name,
                                                           (G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT
                                                           | G_BUS_NAME_OWNER_FLAGS_REPLACE
                                                           | G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE)),
                                           G_VARIANT_TYPE ("(u)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           NULL,    /* cancellable */
                                           &error);

      if (tuple != NULL)
        g_test_message ("-> Was allowed");
      else
        g_test_message ("-> Was not allowed: %s", error->message);

      if (t->can_own)
        {
          guint32 result = 0;

          g_assert_no_error (error);
          g_assert_nonnull (tuple);
          g_variant_get (tuple, "(u)", &result);
          g_assert_cmpuint (result, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
        }
      else if (t->can_see)
        {
          g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
        }
      else
        {
          g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN);
        }

      g_clear_error (&error);
      g_clear_pointer (&tuple, g_variant_unref);

      /* Use a different connection to check who actually owns the name */
      tuple = g_dbus_connection_call_sync (f->cannot_access_conn.conn,
                                           DBUS_SERVICE_DBUS,
                                           DBUS_PATH_DBUS,
                                           DBUS_INTERFACE_DBUS,
                                           "GetNameOwner",
                                           g_variant_new ("(s)", t->name),
                                           G_VARIANT_TYPE ("(s)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           NULL,    /* cancellable */
                                           NULL);

      if (tuple != NULL)
        g_variant_get (tuple, "(&s)", &owner);
      else
        owner = "";

      if (t->can_own)
        g_assert_cmpstr (owner, ==, f->proxied.unique_name);
      else
        g_assert_cmpstr (owner, !=, f->proxied.unique_name);
    }
}

static void
teardown (Fixture *f,
          gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;

  if (f->monitor != NULL)
    {
      g_subprocess_send_signal (f->monitor, SIGTERM);
      g_subprocess_wait (f->monitor, NULL, &error);
      g_assert_no_error (error);
    }

  if (f->dbus_daemon != NULL)
    {
      g_subprocess_send_signal (f->dbus_daemon, SIGTERM);
      g_subprocess_wait (f->dbus_daemon, NULL, &error);
      g_assert_no_error (error);
    }

  if (f->sync_pipe >= 0)
    {
      g_close (f->sync_pipe, &error);
      g_assert_no_error (error);
      f->sync_pipe = -1;
    }

  if (f->proxy != NULL)
    {
      /* It terminates in response to us closing the sync_pipe */
      g_subprocess_wait_check (f->proxy, NULL, &error);
      g_assert_no_error (error);
    }

  connection_clear (&f->proxied);

  if (f->proxy_socket != NULL)
    {
      if (g_remove (f->proxy_socket) != 0 && errno != ENOENT)
        g_warning ("remove %s: %s", f->proxy_socket, g_strerror (errno));

      g_free (f->proxy_socket);
    }

  if (f->temp_directory != NULL)
    {
      if (g_rmdir (f->temp_directory) != 0)
        g_warning ("rmdir %s: %s", f->temp_directory, g_strerror (errno));

      g_free (f->temp_directory);
    }

  g_clear_object (&f->monitor);
  g_clear_object (&f->dbus_daemon);
  g_clear_object (&f->proxy);
  g_free (f->dbus_address);
  g_free (f->proxy_address);
  alarm (0);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/basics", Fixture, NULL, setup, test_basics, teardown);
  g_test_add ("/own", Fixture, NULL, setup, test_own, teardown);

  return g_test_run ();
}

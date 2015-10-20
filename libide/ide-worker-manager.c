/* ide-worker-manager.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "ide-worker-manager"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "egg-counter.h"

#include "ide-worker-process.h"
#include "ide-worker-manager.h"

struct _IdeWorkerManager
{
  GObject      parent_instance;

  GMutex       mutex;

  gchar       *argv0;
  GHashTable  *workers_by_plugin_name;
  GDBusServer *dbus_server;
};

G_DEFINE_TYPE (IdeWorkerManager, ide_worker_manager, G_TYPE_OBJECT)

EGG_DEFINE_COUNTER (instances, "IdeWorkerManager", "Instances", "Number of IdeWorkerManager instances")

enum {
  PROP_0,
  PROP_ARGV0,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

static void
ide_worker_manager_set_argv0 (IdeWorkerManager *self,
                              const gchar      *argv0)
{
  g_return_if_fail (IDE_IS_WORKER_MANAGER (self));

  if (argv0 == NULL)
    argv0 = "gnome-builder";

  if (g_strcmp0 (argv0, self->argv0) != 0)
    {
      g_free (self->argv0);
      self->argv0 = g_strdup (argv0);
      g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_ARGV0]);
    }
}

static gboolean
ide_worker_manager_new_connection_cb (IdeWorkerManager *self,
                                      GDBusConnection  *connection,
                                      GDBusServer      *server)
{
  GCredentials *credentials;
  GHashTableIter iter;
  gpointer key, value;
  gboolean handled = FALSE;

  g_assert (IDE_IS_WORKER_MANAGER (self));
  g_assert (G_IS_DBUS_CONNECTION (connection));
  g_assert (G_IS_DBUS_SERVER (server));

  credentials = g_dbus_connection_get_peer_credentials (connection);
  if ((credentials == NULL) || !g_credentials_get_unix_pid (credentials, NULL))
    return FALSE;

  g_mutex_lock (&self->mutex);

  g_hash_table_iter_init (&iter, self->workers_by_plugin_name);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      IdeWorkerProcess *process = value;

      if (ide_worker_process_matches_credentials (process, credentials))
        {
          ide_worker_process_set_connection (process, connection);
          handled = TRUE;
        }
    }

  g_mutex_unlock (&self->mutex);

  return handled;
}

static void
ide_worker_manager_constructed (GObject *object)
{
  IdeWorkerManager *self = (IdeWorkerManager *)object;
  g_autofree gchar *guid = NULL;
  g_autofree gchar *address = NULL;
  g_autofree gchar *tmpdir = NULL;
  GError *error = NULL;

  g_return_if_fail (IDE_IS_WORKER_MANAGER (self));

  G_OBJECT_CLASS (ide_worker_manager_parent_class)->constructed (object);

  if (g_unix_socket_address_abstract_names_supported ())
    tmpdir = g_strdup_printf ("%s/gnome-builder-worker-%u",
                              g_get_tmp_dir (), getpid ());
  else
    tmpdir = g_dir_make_tmp ("gnome-builder-worker-XXXXXX", NULL);

  if (tmpdir == NULL)
    {
      g_error ("Failed to determine temporary directory for DBus.");
      exit (EXIT_FAILURE);
    }

  address = g_strdup_printf ("unix:tmpdir=%s", tmpdir);
  guid = g_dbus_generate_guid ();

  self->dbus_server = g_dbus_server_new_sync (address,
                                              G_DBUS_SERVER_FLAGS_NONE,
                                              guid,
                                              NULL,
                                              NULL,
                                              &error);

  g_signal_connect_object (self->dbus_server,
                           "new-connection",
                           G_CALLBACK (ide_worker_manager_new_connection_cb),
                           self,
                           G_CONNECT_SWAPPED);

  if (error != NULL)
    {
      g_printerr ("%s\n", error->message);
      exit (EXIT_FAILURE);
    }

  g_assert (self->dbus_server != NULL);
}

static void
ide_worker_manager_force_exit_worker (gpointer instance)
{
  IdeWorkerProcess *process = instance;

  g_assert (IDE_IS_WORKER_PROCESS (process));

  ide_worker_process_quit (process);
  g_object_unref (process);
}

static void
ide_worker_manager_finalize (GObject *object)
{
  IdeWorkerManager *self = (IdeWorkerManager *)object;

  g_clear_pointer (&self->workers_by_plugin_name, g_hash_table_unref);
  g_clear_object (&self->dbus_server);

  G_OBJECT_CLASS (ide_worker_manager_parent_class)->finalize (object);

  EGG_COUNTER_DEC (instances);
}

static void
ide_worker_manager_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  IdeWorkerManager *self = IDE_WORKER_MANAGER (object);

  switch (prop_id)
    {
    case PROP_ARGV0:
      ide_worker_manager_set_argv0 (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_worker_manager_class_init (IdeWorkerManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ide_worker_manager_constructed;
  object_class->finalize = ide_worker_manager_finalize;
  object_class->set_property = ide_worker_manager_set_property;

  gParamSpecs [PROP_ARGV0] =
    g_param_spec_string ("argv0",
                         _("Argv0"),
                         _("The path to the process to spawn."),
                         "gnome-builder",
                         (G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, gParamSpecs);
}

static void
ide_worker_manager_init (IdeWorkerManager *self)
{
  EGG_COUNTER_INC (instances);

  g_mutex_init (&self->mutex);

  self->argv0 = g_strdup ("gnome-builder");

  self->workers_by_plugin_name =
    g_hash_table_new_full (g_str_hash,
                           g_str_equal,
                           g_free,
                           ide_worker_manager_force_exit_worker);
}

GDBusProxy *
ide_worker_manager_get_worker (IdeWorkerManager  *self,
                               const gchar       *plugin_name,
                               GError           **error)
{
  IdeWorkerProcess *worker;

  g_return_val_if_fail (IDE_IS_WORKER_MANAGER (self), NULL);
  g_return_val_if_fail (plugin_name != NULL, NULL);

  g_mutex_lock (&self->mutex);

  worker = g_hash_table_lookup (self->workers_by_plugin_name, plugin_name);

  if (worker == NULL)
    {
      worker = ide_worker_process_new (self->argv0, plugin_name,
                                       g_dbus_server_get_client_address (self->dbus_server));
      g_hash_table_insert (self->workers_by_plugin_name, g_strdup (plugin_name), worker);
      ide_worker_process_run (worker);
    }

  g_mutex_unlock (&self->mutex);

  return ide_worker_process_create_proxy (worker, error);
}

IdeWorkerManager *
ide_worker_manager_new (const gchar *argv0)
{
  g_return_val_if_fail (argv0 != NULL, NULL);

  return g_object_new (IDE_TYPE_WORKER_MANAGER,
                       "argv0", argv0,
                       NULL);
}

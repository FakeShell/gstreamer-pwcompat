/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "gstpipewirecore.h"

/* a list of global cores indexed by fd. */
G_LOCK_DEFINE_STATIC (cores_lock);
static GList *cores;

static gpointer
thread_func (gpointer user_data)
{
  GstPipeWireCore *core = user_data;

  GST_DEBUG ("core thread start");

  g_main_context_push_thread_default (core->context);
  g_main_loop_run (core->loop);
  g_main_context_pop_thread_default (core->context);

  GST_DEBUG ("core thread stop");

  return NULL;
}

static GstPipeWireCore *
make_core (int fd)
{
  GstPipeWireCore *core;

  core = g_new0 (GstPipeWireCore, 1);
  core->refcount = 1;
  core->fd = fd;
  core->context = g_main_context_new ();
  core->loop = g_main_loop_new (core->context, FALSE);
  g_mutex_init (&core->lock);
  g_cond_init (&core->cond);
  core->last_seq = -1;
  core->last_error = 0;

  GST_DEBUG ("created core %p context %p", core, core->context);

  core->running = TRUE;
  core->thread = g_thread_new ("pipewire-core", thread_func, core);

  return core;
}

typedef struct {
  int fd;
} FindData;

static gint
core_find (GstPipeWireCore *core, FindData *data)
{
  /* fd's must match */
  if (core->fd == data->fd)
    return 0;
  return 1;
}

GstPipeWireCore *
gst_pipewire_core_get (int fd)
{
  GstPipeWireCore *core;
  FindData data;
  GList *found;

  data.fd = fd;

  G_LOCK (cores_lock);
  found = g_list_find_custom (cores, &data, (GCompareFunc) core_find);
  if (found != NULL) {
    core = (GstPipeWireCore *) found->data;
    core->refcount++;
    GST_DEBUG ("found core %p", core);
  } else {
    core = make_core (fd);
    if (core != NULL) {
      GST_DEBUG ("created core %p", core);
      /* add to list on success */
      cores = g_list_prepend (cores, core);
    } else {
      GST_WARNING ("could not create core");
    }
  }
  G_UNLOCK (cores_lock);

  return core;
}

void
gst_pipewire_core_release (GstPipeWireCore *core)
{
  gboolean zero;

  G_LOCK (cores_lock);
  core->refcount--;
  if ((zero = (core->refcount == 0))) {
    GST_DEBUG ("closing core %p", core);
    /* remove from list */
    cores = g_list_remove (cores, core);
  }
  G_UNLOCK (cores_lock);

  if (zero) {
    g_mutex_lock (&core->lock);
    core->running = FALSE;
    g_main_loop_quit (core->loop);
    g_mutex_unlock (&core->lock);

    g_thread_join (core->thread);

    g_main_loop_unref (core->loop);
    g_main_context_unref (core->context);
    g_mutex_clear (&core->lock);
    g_cond_clear (&core->cond);

    g_free (core);
  }
}

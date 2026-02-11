/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "gstpipewirestream.h"

#include "gstpipewirepool.h"
#include "gstpipewireclock.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_stream_debug);
#define GST_CAT_DEFAULT pipewire_stream_debug

G_DEFINE_TYPE (GstPipeWireStream, gst_pipewire_stream, GST_TYPE_OBJECT)

static void
gst_pipewire_stream_init (GstPipeWireStream *self)
{
  self->fd = -1;
  self->client_name = g_strdup (g_get_application_name ());
  g_mutex_init (&self->state_lock);
  self->state = GST_PIPEWIRE_STREAM_STATE_UNCONNECTED;
}

static void
gst_pipewire_stream_finalize (GObject *object)
{
  GstPipeWireStream *self = GST_PIPEWIRE_STREAM (object);

  g_clear_object (&self->pool);
  g_free (self->path);
  g_free (self->target_object);
  g_free (self->client_name);
  g_free (self->error_message);
  g_mutex_clear (&self->state_lock);
  gst_clear_structure (&self->client_properties);
  gst_clear_structure (&self->stream_properties);

  G_OBJECT_CLASS (gst_pipewire_stream_parent_class)->finalize (object);
}

void
gst_pipewire_stream_class_init (GstPipeWireStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_pipewire_stream_finalize;

  GST_DEBUG_CATEGORY_INIT (pipewire_stream_debug, "pipewirestream", 0, "PipeWire Stream");
}

GstPipeWireStream *
gst_pipewire_stream_new (GstElement *element)
{
  GstPipeWireStream *stream;

  stream = g_object_new (GST_TYPE_PIPEWIRE_STREAM, NULL);
  stream->element = element;

  return stream;
}

gboolean
gst_pipewire_stream_open (GstPipeWireStream *self,
                          const GstPipeWireStreamEvents *events, void *data)
{
  g_return_val_if_fail (self->core == NULL, FALSE);

  GST_DEBUG_OBJECT (self, "open");

  self->core = gst_pipewire_core_get (self->fd);
  if (self->core == NULL) {
    GST_ELEMENT_ERROR (self->element, RESOURCE, FAILED,
                       ("Failed to connect"), (NULL));
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "connected to core");

  self->events = *events;
  self->events_data = data;

  self->pool = gst_pipewire_pool_new (self);

  self->clock = gst_pipewire_clock_new (self, 0);

  return TRUE;
}

void
gst_pipewire_stream_close (GstPipeWireStream *self)
{
  GST_DEBUG_OBJECT (self, "close");

  gst_element_post_message (GST_ELEMENT (self->element),
                            gst_message_new_clock_lost (GST_OBJECT_CAST (self->element), self->clock));
  g_weak_ref_set (&GST_PIPEWIRE_CLOCK (self->clock)->stream, NULL);
  g_clear_object (&self->clock);

  g_clear_pointer (&self->core, gst_pipewire_core_release);
}

GstPipeWireStreamState
gst_pipewire_stream_get_state (GstPipeWireStream *self, const char **error)
{
  GstPipeWireStreamState state;

  g_mutex_lock (&self->state_lock);
  state = self->state;
  if (error && self->error_message)
    *error = self->error_message;
  g_mutex_unlock (&self->state_lock);

  return state;
}

void
gst_pipewire_stream_set_active (GstPipeWireStream *self, gboolean active)
{
  GST_DEBUG_OBJECT (self, "set active: %d", active);
  self->active = active;

  if (active && self->state == GST_PIPEWIRE_STREAM_STATE_PAUSED) {
    g_mutex_lock (&self->state_lock);
    GstPipeWireStreamState old_state = self->state;
    self->state = GST_PIPEWIRE_STREAM_STATE_STREAMING;
    g_mutex_unlock (&self->state_lock);

    if (self->events.state_changed)
      self->events.state_changed (self->events_data, old_state,
                                  GST_PIPEWIRE_STREAM_STATE_STREAMING, NULL);
    if (self->events.process)
      self->events.process (self->events_data);
  } else if (!active && self->state == GST_PIPEWIRE_STREAM_STATE_STREAMING) {
    g_mutex_lock (&self->state_lock);
    GstPipeWireStreamState old_state = self->state;
    self->state = GST_PIPEWIRE_STREAM_STATE_PAUSED;
    g_mutex_unlock (&self->state_lock);
    if (self->events.state_changed)
      self->events.state_changed (self->events_data, old_state,
                                  GST_PIPEWIRE_STREAM_STATE_PAUSED, NULL);
  }
}

void
gst_pipewire_stream_set_error (GstPipeWireStream *self, int res, const char *error, ...)
{
  va_list args;
  gchar *message;

  va_start (args, error);
  message = g_strdup_vprintf (error, args);
  va_end (args);

  g_mutex_lock (&self->state_lock);
  GstPipeWireStreamState old_state = self->state;
  self->state = GST_PIPEWIRE_STREAM_STATE_ERROR;
  g_free (self->error_message);
  self->error_message = message;
  g_mutex_unlock (&self->state_lock);

  GST_ERROR_OBJECT (self, "error: %s (code: %d)", message, res);

  if (self->events.state_changed)
    self->events.state_changed (self->events_data, old_state,
                                GST_PIPEWIRE_STREAM_STATE_ERROR, message);
}

gboolean
gst_pipewire_stream_connect (GstPipeWireStream *self, uint32_t target_id,
                             uint32_t flags, GstCaps *caps)
{
  GST_DEBUG_OBJECT (self, "connect, target_id: %u, flags: %u", target_id, flags);

  g_mutex_lock (&self->state_lock);
  if (self->state != GST_PIPEWIRE_STREAM_STATE_UNCONNECTED) {
    g_mutex_unlock (&self->state_lock);
    return FALSE;
  }

  GstPipeWireStreamState old_state = self->state;
  self->state = GST_PIPEWIRE_STREAM_STATE_CONNECTING;
  g_mutex_unlock (&self->state_lock);

  if (self->events.state_changed)
    self->events.state_changed (self->events_data, old_state,
                                GST_PIPEWIRE_STREAM_STATE_CONNECTING, NULL);

  GST_DEBUG_OBJECT (self, "connecting with target %u", target_id);

  g_mutex_lock (&self->state_lock);
  old_state = self->state;
  self->state = GST_PIPEWIRE_STREAM_STATE_PAUSED;
  g_mutex_unlock (&self->state_lock);

  if (self->events.param_changed && caps)
    self->events.param_changed (self->events_data, 0, caps);

  if (self->events.state_changed)
    self->events.state_changed (self->events_data, old_state,
                                GST_PIPEWIRE_STREAM_STATE_PAUSED, NULL);

  return TRUE;
}

void
gst_pipewire_stream_disconnect (GstPipeWireStream *self)
{
  GST_DEBUG_OBJECT (self, "disconnect");

  g_mutex_lock (&self->state_lock);
  if (self->state == GST_PIPEWIRE_STREAM_STATE_UNCONNECTED) {
    g_mutex_unlock (&self->state_lock);
    return;
  }

  GstPipeWireStreamState old_state = self->state;
  self->state = GST_PIPEWIRE_STREAM_STATE_UNCONNECTED;
  g_mutex_unlock (&self->state_lock);

  GST_DEBUG_OBJECT (self, "disconnecting");

  if (self->events.state_changed)
    self->events.state_changed (self->events_data, old_state,
                                GST_PIPEWIRE_STREAM_STATE_UNCONNECTED, NULL);
}

void
gst_pipewire_stream_update_params (GstPipeWireStream *self, GstCaps *caps)
{
  GST_DEBUG_OBJECT (self, "update params with caps: %" GST_PTR_FORMAT, caps);

  if (self->events.param_changed && caps)
    self->events.param_changed (self->events_data, 0, caps);
}

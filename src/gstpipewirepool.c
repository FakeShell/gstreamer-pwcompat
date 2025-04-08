/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <unistd.h>

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>

#include "gstpipewirepool.h"

GST_DEBUG_CATEGORY_STATIC (gst_pipewire_pool_debug_category);
#define GST_CAT_DEFAULT gst_pipewire_pool_debug_category

G_DEFINE_TYPE (GstPipeWirePool, gst_pipewire_pool, GST_TYPE_BUFFER_POOL);

enum
{
  ACTIVATED,
  LAST_SIGNAL
};

static guint pool_signals[LAST_SIGNAL] = { 0 };

static GQuark pool_data_quark;

GstPipeWirePool *
gst_pipewire_pool_new (GstPipeWireStream *stream)
{
  GstPipeWirePool *pool;

  pool = g_object_new (GST_TYPE_PIPEWIRE_POOL, NULL);
  g_weak_ref_set (&pool->stream, stream);

  return pool;
}

static void
pool_data_destroy (gpointer user_data)
{
  GstPipeWirePoolData *data = user_data;

  gst_object_unref (data->pool);
  g_slice_free (GstPipeWirePoolData, data);
}

void
gst_pipewire_pool_add_buffer (GstPipeWirePool *pool, uint32_t id)
{
  GstBuffer *buf;
  GstPipeWirePoolData *data;

  GST_DEBUG_OBJECT (pool, "add buffer with id %u", id);

  data = g_slice_new0 (GstPipeWirePoolData);

  buf = gst_buffer_new ();

  /* just create a dummy buffer for now */
  GstMemory *mem = gst_allocator_alloc (NULL, 4096, NULL);
  gst_buffer_insert_memory (buf, 0, mem);

  data->pool = gst_object_ref (pool);
  data->owner = NULL;
  data->id = id;
  data->flags = GST_BUFFER_FLAGS (buf);
  data->buf = buf;
  data->queued = TRUE;

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             pool_data_quark,
                             data,
                             pool_data_destroy);

  pool->n_buffers++;
}

void
gst_pipewire_pool_remove_buffer (GstPipeWirePool *pool, uint32_t id)
{
  GST_DEBUG_OBJECT (pool, "remove buffer with id %u", id);
  /* nothing to remove in this implementation */
  pool->n_buffers--;
}

GstPipeWirePoolData *
gst_pipewire_pool_get_data (GstBuffer *buffer)
{
  return gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer), pool_data_quark);
}

static GstFlowReturn
acquire_buffer (GstBufferPool *pool, GstBuffer **buffer,
                GstBufferPoolAcquireParams * params)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&p->stream);
  GstBuffer *buf;

  if (G_UNLIKELY (!s))
    return GST_FLOW_ERROR;

  GST_OBJECT_LOCK (pool);
  while (TRUE) {
    if (G_UNLIKELY (GST_BUFFER_POOL_IS_FLUSHING (pool)))
      goto flushing;

    GST_DEBUG_OBJECT (pool, "attempting to acquire buffer");
    /* there is no buffer to acquire in this implementation */
    goto no_more_buffers;

    if (params) {
      if (params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT)
        goto no_more_buffers;

      if ((params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_LAST) &&
	      p->paused)
        goto paused;
    }

    GST_WARNING_OBJECT (pool, "failed to dequeue buffer, waiting...");
    g_cond_wait (&p->cond, GST_OBJECT_GET_LOCK (pool));
  }

  GST_OBJECT_UNLOCK (pool);
  *buffer = buf;
  GST_LOG_OBJECT (pool, "acquired buffer %p", *buffer);

  return GST_FLOW_OK;

flushing:
  {
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_FLUSHING;
  }
paused:
  {
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_CUSTOM_ERROR_1;
  }
no_more_buffers:
  {
    GST_LOG_OBJECT (pool, "no more buffers available");
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_EOS;
  }
}

static const gchar **
get_options (GstBufferPool *pool G_GNUC_UNUSED)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  return options;
}

static gboolean
set_config (GstBufferPool *pool, GstStructure *config)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  GstCaps *caps;
  GstStructure *structure;
  guint size, min_buffers, max_buffers;
  gboolean has_video;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers)) {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }

  if (caps == NULL) {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }

  structure = gst_caps_get_structure (caps, 0);
  if (g_str_has_prefix (gst_structure_get_name (structure), "video/") ||
      g_str_has_prefix (gst_structure_get_name (structure), "image/")) {
    has_video = TRUE;
    gst_video_info_from_caps (&p->video_info, caps);
  } else {
    has_video = FALSE;
  }

  p->add_metavideo = has_video && gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (p->video_info.size != 0)
    size = p->video_info.size;

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (gst_pipewire_pool_parent_class)->set_config (pool, config);
}

void
gst_pipewire_pool_set_paused (GstPipeWirePool *pool, gboolean paused)
{
  GST_DEBUG_OBJECT (pool, "pause: %u", paused);
  GST_OBJECT_LOCK (pool);
  pool->paused = paused;
  g_cond_signal (&pool->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
flush_start (GstBufferPool *pool)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);

  GST_DEBUG_OBJECT (pool, "flush start");
  GST_OBJECT_LOCK (pool);
  g_cond_signal (&p->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
release_buffer (GstBufferPool *pool, GstBuffer *buffer)
{
  GST_LOG_OBJECT (pool, "release buffer %p", buffer);

  GstPipeWirePoolData *data = gst_pipewire_pool_get_data (buffer);

  GST_OBJECT_LOCK (pool);
  if (!data->queued) {
    GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
    g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&p->stream);

    GST_DEBUG_OBJECT (pool, "queue buffer %p with id %u back to pipewire", buffer, data->id);

    /* nothing to dequeue in this implementation */
    data->queued = TRUE;
  }
  GST_OBJECT_UNLOCK (pool);
}

static gboolean
do_start (GstBufferPool *pool)
{
  g_signal_emit (pool, pool_signals[ACTIVATED], 0, NULL);
  return TRUE;
}

static void
gst_pipewire_pool_finalize (GObject *object)
{
  GstPipeWirePool *pool = GST_PIPEWIRE_POOL (object);

  GST_DEBUG_OBJECT (pool, "finalize");
  g_weak_ref_set (&pool->stream, NULL);

  G_OBJECT_CLASS (gst_pipewire_pool_parent_class)->finalize (object);
}

static void
gst_pipewire_pool_class_init (GstPipeWirePoolClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = gst_pipewire_pool_finalize;

  bufferpool_class->get_options = get_options;
  bufferpool_class->set_config = set_config;
  bufferpool_class->start = do_start;
  bufferpool_class->flush_start = flush_start;
  bufferpool_class->acquire_buffer = acquire_buffer;
  bufferpool_class->release_buffer = release_buffer;

  pool_signals[ACTIVATED] =
      g_signal_new ("activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

  GST_DEBUG_CATEGORY_INIT (gst_pipewire_pool_debug_category, "pipewirepool", 0,
      "debug category for pipewirepool object");

  pool_data_quark = g_quark_from_static_string ("GstPipeWirePoolDataQuark");
}

static void
gst_pipewire_pool_init (GstPipeWirePool *pool)
{
  g_cond_init (&pool->cond);
}

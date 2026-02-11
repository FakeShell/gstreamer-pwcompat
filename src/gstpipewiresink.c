/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "gstpipewiresink.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

#include <gst/video/video.h>
#include <gst/base/gstbasesink.h>

#include "gstpipewireclock.h"
#include "gstpipewireformat.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_sink_debug);
#define GST_CAT_DEFAULT pipewire_sink_debug

#define DEFAULT_PROP_MODE GST_PIPEWIRE_SINK_MODE_DEFAULT
#define DEFAULT_PROP_SLAVE_METHOD GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE
#define DEFAULT_PROP_USE_BUFFERPOOL USE_BUFFERPOOL_AUTO

enum
{
  PROP_0,
  PROP_PATH,
  PROP_TARGET_OBJECT,
  PROP_CLIENT_NAME,
  PROP_CLIENT_PROPERTIES,
  PROP_STREAM_PROPERTIES,
  PROP_MODE,
  PROP_FD,
  PROP_SLAVE_METHOD,
  PROP_USE_BUFFERPOOL,
};

GType
gst_pipewire_sink_mode_get_type (void)
{
  static gsize mode_type = 0;
  static const GEnumValue mode[] = {
    {GST_PIPEWIRE_SINK_MODE_DEFAULT, "GST_PIPEWIRE_SINK_MODE_DEFAULT", "default"},
    {GST_PIPEWIRE_SINK_MODE_RENDER, "GST_PIPEWIRE_SINK_MODE_RENDER", "render"},
    {GST_PIPEWIRE_SINK_MODE_PROVIDE, "GST_PIPEWIRE_SINK_MODE_PROVIDE", "provide"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&mode_type)) {
    GType tmp =
        g_enum_register_static ("GstPipeWireSinkMode", mode);
    g_once_init_leave (&mode_type, tmp);
  }

  return (GType) mode_type;
}

GType
gst_pipewire_sink_slave_method_get_type (void)
{
  static gsize method_type = 0;
  static const GEnumValue method[] = {
    {GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE, "GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE", "none"},
    {GST_PIPEWIRE_SINK_SLAVE_METHOD_RESAMPLE, "GST_PIPEWIRE_SINK_SLAVE_METHOD_RESAMPLE", "resample"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&method_type)) {
    GType tmp =
        g_enum_register_static ("GstPipeWireSinkSlaveMethod", method);
    g_once_init_leave (&method_type, tmp);
  }

  return (GType) method_type;
}

static GstStaticPadTemplate gst_pipewire_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pipewire_sink_parent_class parent_class
G_DEFINE_TYPE (GstPipeWireSink, gst_pipewire_sink, GST_TYPE_BASE_SINK);


static void
gst_pipewire_sink_set_property (GObject *object, guint prop_id,
                                   const GValue *value, GParamSpec *pspec)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsink->stream->path);
      pwsink->stream->path = g_value_dup_string (value);
      break;
    case PROP_TARGET_OBJECT:
      g_free (pwsink->stream->target_object);
      pwsink->stream->target_object = g_value_dup_string (value);
      break;
    case PROP_CLIENT_NAME:
      g_free (pwsink->stream->client_name);
      pwsink->stream->client_name = g_value_dup_string (value);
      break;
    case PROP_CLIENT_PROPERTIES:
      if (pwsink->stream->client_properties)
        gst_structure_free (pwsink->stream->client_properties);
      pwsink->stream->client_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;
    case PROP_STREAM_PROPERTIES:
      if (pwsink->stream->stream_properties)
        gst_structure_free (pwsink->stream->stream_properties);
      pwsink->stream->stream_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;
    case PROP_MODE:
      pwsink->mode = g_value_get_enum (value);
      break;
    case PROP_FD:
      pwsink->stream->fd = g_value_get_int (value);
      break;
    case PROP_SLAVE_METHOD:
      pwsink->slave_method = g_value_get_enum (value);
      break;
    case PROP_USE_BUFFERPOOL:
      if (g_value_get_boolean (value))
        pwsink->use_bufferpool = USE_BUFFERPOOL_YES;
      else
        pwsink->use_bufferpool = USE_BUFFERPOOL_NO;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_sink_get_property (GObject *object, guint prop_id,
                                   GValue *value, GParamSpec *pspec)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsink->stream->path);
      break;
    case PROP_TARGET_OBJECT:
      g_value_set_string (value, pwsink->stream->target_object);
      break;
    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsink->stream->client_name);
      break;
    case PROP_CLIENT_PROPERTIES:
      gst_value_set_structure (value, pwsink->stream->client_properties);
      break;
    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsink->stream->stream_properties);
      break;
    case PROP_MODE:
      g_value_set_enum (value, pwsink->mode);
      break;
    case PROP_FD:
      g_value_set_int (value, pwsink->stream->fd);
      break;
    case PROP_SLAVE_METHOD:
      g_value_set_enum (value, pwsink->slave_method);
      break;
    case PROP_USE_BUFFERPOOL:
      g_value_set_boolean (value, !!pwsink->use_bufferpool);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_pipewire_sink_change_state (GstElement *element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK_CAST (element);

  GST_DEBUG_OBJECT (pwsink, "changing state: %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  /* just do the standard state change - skip all the pipewire specific stuff for now */
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  return ret;
}

static GstCaps *
gst_pipewire_sink_sink_fixate (GstBaseSink *bsink, GstCaps *caps)
{
  GstStructure *structure;
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (bsink);

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (structure, "video/x-raw")) {
    pwsink->is_video = true;
    gst_structure_fixate_field_nearest_int (structure, "width", 320);
    gst_structure_fixate_field_nearest_int (structure, "height", 240);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);

    if (gst_structure_has_field (structure, "pixel-aspect-ratio"))
      gst_structure_fixate_field_nearest_fraction (structure,
          "pixel-aspect-ratio", 1, 1);
    else
      gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);

    if (gst_structure_has_field (structure, "colorimetry"))
      gst_structure_fixate_field_string (structure, "colorimetry", "bt601");
    if (gst_structure_has_field (structure, "chroma-site"))
      gst_structure_fixate_field_string (structure, "chroma-site", "mpeg2");

    if (gst_structure_has_field (structure, "interlace-mode"))
      gst_structure_fixate_field_string (structure, "interlace-mode", "progressive");
    else
      gst_structure_set (structure, "interlace-mode", G_TYPE_STRING, "progressive", NULL);
  } else if (gst_structure_has_name (structure, "audio/x-raw")) {
    gst_structure_fixate_field_string (structure, "format", "S16LE");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  } else if (gst_structure_has_name (structure, "audio/mpeg")) {
    gst_structure_fixate_field_string (structure, "format", "Encoded");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  } else if (gst_structure_has_name (structure, "audio/x-flac")) {
    gst_structure_fixate_field_string (structure, "format", "Encoded");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  }

  caps = GST_BASE_SINK_CLASS (parent_class)->fixate (bsink, caps);

  return caps;
}

static GstFlowReturn
gst_pipewire_sink_render (GstBaseSink *psink, GstBuffer *buffer)
{
  GstPipeWireSink *pwsink;
  GstFlowReturn res = GST_FLOW_OK;
  const char *error = NULL;

  pwsink = GST_PIPEWIRE_SINK (psink);

  if (!pwsink->negotiated)
    goto not_negotiated;

  GST_DEBUG_OBJECT (pwsink, "rendering buffer %p", buffer);

  if (gst_pipewire_stream_get_state (pwsink->stream, &error) != GST_PIPEWIRE_STREAM_STATE_STREAMING)
    goto streaming_error;

  /* in a real implementation this would be doing something instead of sleep */
  g_usleep (10000);

  return res;

not_negotiated:
  return GST_FLOW_NOT_NEGOTIATED;
streaming_error:
  GST_DEBUG_OBJECT (pwsink, "streaming error: %s", error ? error : "unknown");
  return GST_FLOW_ERROR;
}

static gboolean gst_pipewire_sink_event (GstBaseSink *sink, GstEvent *event)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK(sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
    {
      GST_DEBUG_OBJECT (pwsink, "flush-start");

      /* nothing to flush in this implementation */
      GST_DEBUG_OBJECT (pwsink, "flushing buffers");

      gst_buffer_pool_set_flushing (GST_BUFFER_POOL_CAST (pwsink->stream->pool), TRUE);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      GST_DEBUG_OBJECT (pwsink, "flush-stop");

      /* nothing to flush in this implementation */
      GST_DEBUG_OBJECT (pwsink, "stop flushing");

      gst_buffer_pool_set_flushing (GST_BUFFER_POOL_CAST (pwsink->stream->pool), FALSE);
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}

static GstClock *
gst_pipewire_sink_provide_clock (GstElement *elem)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pwsink);
  if (!GST_OBJECT_FLAG_IS_SET (pwsink, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pwsink->stream->clock)
    clock = GST_CLOCK_CAST (gst_object_ref (pwsink->stream->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pwsink);

  return clock;

clock_disabled:
  GST_DEBUG_OBJECT (pwsink, "clock provide disabled");
  GST_OBJECT_UNLOCK (pwsink);
  return NULL;
}

static void
gst_pipewire_sink_finalize (GObject *object)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  gst_clear_object (&pwsink->stream);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_pipewire_sink_propose_allocation (GstBaseSink *bsink, GstQuery *query)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (bsink);

  if (pwsink->use_bufferpool != USE_BUFFERPOOL_NO)
    gst_query_add_allocation_pool (query, GST_BUFFER_POOL_CAST (pwsink->stream->pool), 0, 0, 0);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  return TRUE;
}

static gboolean
gst_pipewire_sink_setcaps (GstBaseSink *bsink, GstCaps *caps)
{
  GstPipeWireSink *pwsink;
  g_autoptr(GPtrArray) format_params = NULL;
  GstPipeWireStreamState state;
  const char *error = NULL;
  uint32_t target_id;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  GST_DEBUG_OBJECT (pwsink, "set caps %" GST_PTR_FORMAT, caps);

  state = gst_pipewire_stream_get_state (pwsink->stream, &error);
  if (state == GST_PIPEWIRE_STREAM_STATE_ERROR)
    goto start_error;

  if (state == GST_PIPEWIRE_STREAM_STATE_UNCONNECTED) {
    format_params = gst_pipewire_caps_to_format (caps);
    if (!format_params || format_params->len == 0)
      goto no_format;

    target_id = pwsink->stream->path ? (uint32_t)atoi (pwsink->stream->path) : 0;

    uint32_t flags = 0;
    if (pwsink->mode == GST_PIPEWIRE_SINK_MODE_PROVIDE)
      flags |= 0x1;

    GST_DEBUG_OBJECT (pwsink, "connecting with target_id %u", target_id);
    if (!gst_pipewire_stream_connect (pwsink->stream, target_id, flags, caps))
      goto connect_error;
  }

  gst_pipewire_clock_reset (GST_PIPEWIRE_CLOCK (pwsink->stream->clock), 0);

  pwsink->negotiated = TRUE;

  return TRUE;

start_error:
  GST_ERROR_OBJECT (pwsink, "could not start stream: %s", error);
  return FALSE;
no_format:
  GST_ERROR_OBJECT (pwsink, "could not convert caps to format");
  return FALSE;
connect_error:
  GST_ERROR_OBJECT (pwsink, "could not connect stream");
  return FALSE;
}

/* unused at the moment */
static gboolean
gst_pipewire_sink_stream_start (GstPipeWireSink *pwsink)
{
  const char *error = NULL;
  GstPipeWireStreamState state;

  GST_DEBUG_OBJECT (pwsink, "doing stream start");

  while (TRUE) {
    state = gst_pipewire_stream_get_state (pwsink->stream, &error);

    GST_DEBUG_OBJECT (pwsink, "waiting for STREAMING, now %d", state);
    if (state == GST_PIPEWIRE_STREAM_STATE_STREAMING)
      break;

    if (state == GST_PIPEWIRE_STREAM_STATE_ERROR)
      goto start_error;

    /* simulate success after connection */
    if (state == GST_PIPEWIRE_STREAM_STATE_PAUSED) {
      gst_pipewire_stream_set_active (pwsink->stream, TRUE);
      break;
    }

    if (state == GST_PIPEWIRE_STREAM_STATE_CONNECTING) {
      g_usleep (100000);
      continue;
    }

    break;
  }

  GST_DEBUG_OBJECT (pwsink, "stream started");
  return TRUE;

start_error:
  GST_DEBUG_OBJECT (pwsink, "error starting stream: %s", error);
  return FALSE;
}

static void
on_state_changed (void *data, GstPipeWireStreamState old,
                  GstPipeWireStreamState state, const char *error)
{
  GstPipeWireSink *pwsink = data;

  GST_DEBUG_OBJECT (pwsink, "got stream state change: %d -> %d", old, state);

  if (state == GST_PIPEWIRE_STREAM_STATE_ERROR) {
    if (gst_pipewire_stream_get_state (pwsink->stream, NULL) != GST_PIPEWIRE_STREAM_STATE_ERROR)
      gst_pipewire_stream_set_error (pwsink->stream, -EPIPE, "%s", error);
    else
      GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
                         ("stream error: %s", error), (NULL));
  }
}

static void
on_param_changed (void *data, uint32_t id G_GNUC_UNUSED, GstCaps *caps)
{
  GstPipeWireSink *pwsink = data;

  GST_DEBUG_OBJECT (pwsink, "got param changed with caps: %" GST_PTR_FORMAT, caps);
}

static void
on_process (void *data)
{
  GstPipeWireSink *pwsink = data;

  GST_DEBUG_OBJECT (pwsink, "got process signal");
}

/* unused at the moment */
static const GstPipeWireStreamEvents stream_events G_GNUC_UNUSED = {
  .state_changed = on_state_changed,
  .param_changed = on_param_changed,
  .process = on_process
};

static void
gst_pipewire_sink_class_init (GstPipeWireSinkClass *klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->finalize = gst_pipewire_sink_finalize;
  gobject_class->set_property = gst_pipewire_sink_set_property;
  gobject_class->get_property = gst_pipewire_sink_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The sink path to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_TARGET_OBJECT,
                                   g_param_spec_string ("target-object",
                                                        "Target object",
                                                        "The sink name/serial to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_NAME,
                                   g_param_spec_string ("client-name",
                                                        "Client Name",
                                                        "The client name to use (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_CLIENT_PROPERTIES,
                                    g_param_spec_boxed ("client-properties",
                                                        "Client properties",
                                                        "List of client properties",
                                                        GST_TYPE_STRUCTURE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_STREAM_PROPERTIES,
                                    g_param_spec_boxed ("stream-properties",
                                                        "Stream properties",
                                                        "List of stream properties",
                                                        GST_TYPE_STRUCTURE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_MODE,
                                    g_param_spec_enum ("mode",
                                                       "Mode",
                                                       "The mode to operate in",
                                                        GST_TYPE_PIPEWIRE_SINK_MODE,
                                                        DEFAULT_PROP_MODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_FD,
                                    g_param_spec_int ("fd",
                                                      "Fd",
                                                      "The fd to connect with",
                                                      -1, G_MAXINT, -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SLAVE_METHOD,
                                   g_param_spec_enum ("slave-method",
                                                      "Slave Method",
                                                      "Algorithm used to match the rate of the masterclock",
                                                      GST_TYPE_PIPEWIRE_SINK_SLAVE_METHOD,
                                                      DEFAULT_PROP_SLAVE_METHOD,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_USE_BUFFERPOOL,
                                   g_param_spec_boolean ("use-bufferpool",
                                                         "Use bufferpool",
                                                         "Use bufferpool (default: true for video, false for audio)",
                                                         DEFAULT_PROP_USE_BUFFERPOOL,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pipewire_sink_provide_clock;
  gstelement_class->change_state = gst_pipewire_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire sink", "Sink/Audio/Video",
      "Send audio/video to PipeWire", "Bardia Moshiri <bardia@furilabs.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_sink_template));

  gstbasesink_class->set_caps = gst_pipewire_sink_setcaps;
  gstbasesink_class->fixate = gst_pipewire_sink_sink_fixate;
  gstbasesink_class->propose_allocation = gst_pipewire_sink_propose_allocation;
  gstbasesink_class->render = gst_pipewire_sink_render;
  gstbasesink_class->event = gst_pipewire_sink_event;

  GST_DEBUG_CATEGORY_INIT (pipewire_sink_debug, "pipewiresink", 0, "PipeWire Sink");
}

static void
gst_pipewire_sink_init (GstPipeWireSink *sink)
{
  GST_BASE_SINK (sink)->segment.format = GST_FORMAT_TIME;

  gst_base_sink_set_sync (GST_BASE_SINK (sink), TRUE);
  gst_base_sink_set_async_enabled (GST_BASE_SINK (sink), TRUE);

  GST_OBJECT_FLAG_SET (sink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  sink->stream = gst_pipewire_stream_new (GST_ELEMENT (sink));

  sink->mode = DEFAULT_PROP_MODE;
  sink->use_bufferpool = DEFAULT_PROP_USE_BUFFERPOOL;
  sink->is_video = FALSE;
}

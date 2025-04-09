/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "gstpipewiresrc.h"
#include "gstpipewireformat.h"

#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include "gstpipewireclock.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_src_debug);
#define GST_CAT_DEFAULT pipewire_src_debug

#define DEFAULT_ALWAYS_COPY     false
#define DEFAULT_MIN_BUFFERS     8
#define DEFAULT_MAX_BUFFERS     INT32_MAX
#define DEFAULT_RESEND_LAST     false
#define DEFAULT_KEEPALIVE_TIME  0
#define DEFAULT_AUTOCONNECT     true
#define DEFAULT_USE_BUFFERPOOL  USE_BUFFERPOOL_AUTO
#define DEFAULT_USE_CAMERA      true
#define DEFAULT_CAMERA_ID       0

enum
{
  PROP_0,
  PROP_PATH,
  PROP_TARGET_OBJECT,
  PROP_CLIENT_NAME,
  PROP_CLIENT_PROPERTIES,
  PROP_STREAM_PROPERTIES,
  PROP_ALWAYS_COPY,
  PROP_MIN_BUFFERS,
  PROP_MAX_BUFFERS,
  PROP_FD,
  PROP_RESEND_LAST,
  PROP_KEEPALIVE_TIME,
  PROP_AUTOCONNECT,
  PROP_USE_BUFFERPOOL,
  PROP_USE_CAMERA,
  PROP_CAMERA_ID,
  PROP_ORIENTATION,
};

static GstStaticPadTemplate gst_pipewire_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pipewire_src_parent_class parent_class
G_DEFINE_TYPE (GstPipeWireSrc, gst_pipewire_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pipewire_src_change_state (GstElement *element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSrc *self = GST_PIPEWIRE_SRC_CAST (element);

  GST_DEBUG_OBJECT (self, "changing state: %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (self->use_camera) {
        /* get the camera ID from target-object if set */
        if (self->stream->target_object != NULL) {
          gint target_id = -1;
          if (sscanf (self->stream->target_object, "%d", &target_id) == 1 && target_id >= 0) {
            GST_DEBUG_OBJECT (self, "Using camera ID %d from target-object", target_id);
            self->camera_id = target_id;
          } else {
            GST_WARNING_OBJECT (self, "Invalid target-object '%s', using default camera ID %d",
                                self->stream->target_object, self->camera_id);
          }
        }

        if (!self->camera) {
          self->camera = gst_pipewire_camera_new (self);
          if (!self->camera) {
            GST_ERROR_OBJECT (self, "Failed to create camera object");
            return GST_STATE_CHANGE_FAILURE;
          }

          /* set the camera ID in the camera object */
          self->camera->camera_id = self->camera_id;
          GST_INFO_OBJECT (self, "Opening camera with ID: %d", self->camera_id);
        }

        if (!gst_pipewire_camera_open (self->camera)) {
          GST_ERROR_OBJECT (self, "Failed to open camera");
          return GST_STATE_CHANGE_FAILURE;
        }
      } else {
        self->negotiated = TRUE;
        self->is_video = TRUE;
      }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (self->use_camera) {
        if (!gst_pipewire_camera_start_streaming (self->camera)) {
          GST_ERROR_OBJECT (self, "Failed to start camera streaming");
          return GST_STATE_CHANGE_FAILURE;
        }
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->use_camera)
        gst_pipewire_camera_stop_streaming (self->camera);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (self->use_camera && self->camera) {
        gst_pipewire_camera_close (self->camera);
        g_clear_object (&self->camera);
      }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    default:
      break;
  }
  return ret;
}

static gboolean
gst_pipewire_src_send_event (GstElement *elem, GstEvent *event)
{
  GstPipeWireSrc *self = GST_PIPEWIRE_SRC_CAST (elem);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (self, "got EOS");
      self->eos = true;
      ret = TRUE;
      break;
    default:
      ret = GST_ELEMENT_CLASS (parent_class)->send_event (elem, event);
      break;
  }
  return ret;
}

static GstFlowReturn
gst_pipewire_src_create (GstPushSrc *psrc, GstBuffer **buffer)
{
  GstPipeWireSrc *pwsrc;
  GstBuffer *buf = NULL;
  GstClockTime timestamp;
  static GstBuffer *last_buffer = NULL;
  static GstClockTime previous_ts = 0;
  static GstClockTime last_push_time = 0;
  guint size = 0;

  pwsrc = GST_PIPEWIRE_SRC (psrc);

  GstClockTime now = gst_util_get_timestamp ();

  /* add a small delay between frames to allow gtk4paintablesink to catch up */
  if (last_push_time != 0) {
    GstClockTime elapsed = now - last_push_time;

    if (elapsed < 10000000) {
      g_usleep (10000);
      GST_LOG_OBJECT (pwsrc, "Adding small delay to help sink keep up");
    }
  }

  timestamp = gst_clock_get_time (GST_ELEMENT_CLOCK (GST_ELEMENT (psrc)));
  if (GST_CLOCK_TIME_IS_VALID (timestamp))
    timestamp -= gst_element_get_base_time (GST_ELEMENT (psrc));
  else
    timestamp = GST_CLOCK_TIME_NONE;

  if (pwsrc->use_camera && pwsrc->camera) {
    buf = gst_pipewire_camera_get_latest_frame (pwsrc->camera);

    if (buf) {
      if (last_buffer)
        gst_buffer_unref(last_buffer);
      last_buffer = gst_buffer_ref(buf);
    } else if (last_buffer) {
      /* no new frame available, use the last frame */
      buf = gst_buffer_ref (last_buffer);
      GST_LOG_OBJECT (pwsrc, "Reusing last valid frame");
    } else {
      /* no frame yet and no last frame, create a blank frame */
      gint width, height, rotation;
      gst_pipewire_camera_get_info (pwsrc->camera, &width, &height, &rotation);

      if (rotation == 90 || rotation == 270) {
        int temp = width;
        width = height;
        height = temp;
      }

      guint size = width * height * 3 / 2; /* YUV 4:2:0 format */
      buf = gst_buffer_new_allocate (NULL, size, NULL);
      if (!buf) {
        GST_ERROR_OBJECT (pwsrc, "Failed to allocate buffer");
        return GST_FLOW_ERROR;
      }

      GstMapInfo info;
      gst_buffer_map (buf, &info, GST_MAP_WRITE);

      /* Y=16, U=V=128 */
      memset (info.data, 16, width * height); /* Y plane */
      memset (info.data + width * height, 128, size - width * height); /* UV planes */

      gst_buffer_unmap (buf, &info);
    }

    if (buf) {
      GST_BUFFER_PTS (buf) = timestamp;
      GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;

      if (GST_CLOCK_TIME_IS_VALID (previous_ts) && previous_ts != 0)
        GST_BUFFER_DURATION (buf) = timestamp - previous_ts;
      else
        /* default to 30fps if we don't know */
        GST_BUFFER_DURATION (buf) = GST_SECOND / 30;

      /* keep track of when we are pushing a frame */
      previous_ts = timestamp;
      last_push_time = now;

      *buffer = buf;
      return GST_FLOW_OK;
    }
  } else {
    /* fallback code for audio */
    size = 1024;

    buf = gst_buffer_new_allocate (NULL, size, NULL);
    if (!buf) {
      GST_ERROR_OBJECT (pwsrc, "Failed to allocate buffer");
      return GST_FLOW_ERROR;
    }

    GstMapInfo info;
    gst_buffer_map(buf, &info, GST_MAP_WRITE);

    memset(info.data, 0, info.size);

    gst_buffer_unmap(buf, &info);

    GST_BUFFER_PTS (buf) = timestamp;
    GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;

    if (GST_CLOCK_TIME_IS_VALID (previous_ts) && previous_ts != 0)
      GST_BUFFER_DURATION (buf) = timestamp - previous_ts;
    else
      GST_BUFFER_DURATION (buf) = GST_SECOND / 30;

    previous_ts = timestamp;
    last_push_time = now;
  }

  *buffer = buf;
  return GST_FLOW_OK;
}

static gboolean
gst_pipewire_src_start (GstBaseSrc *basesrc G_GNUC_UNUSED)
{
  return TRUE;
}

static gboolean
gst_pipewire_src_stop (GstBaseSrc *basesrc)
{
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pwsrc->eos = false;
  gst_buffer_replace(&pwsrc->last_buffer, NULL);
  gst_caps_replace(&pwsrc->caps, NULL);

  return TRUE;
}

static gboolean
gst_pipewire_src_event (GstBaseSrc *src, GstEvent *event)
{
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
      if (gst_video_event_is_force_key_unit (event)) {
        GstClockTime running_time;
        gboolean all_headers;
        guint count;

        gst_video_event_parse_upstream_force_key_unit (event,
                &running_time, &all_headers, &count);

        /* not sure what to do here */
        GST_DEBUG_OBJECT (src, "force-key-unit event, requesting keyframe");

        res = TRUE;
      } else {
        res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      }
      break;
    case GST_EVENT_QOS:
      {
        gdouble proportion;
        GstClockTimeDiff diff;
        GstClockTime timestamp;

        gst_event_parse_qos (event, NULL, &proportion, &diff, &timestamp);

        if (diff < 0)
          /* we're running ahead, maybe slow down */
          GST_DEBUG_OBJECT (src, "QoS: we're ahead by %" GST_TIME_FORMAT, GST_TIME_ARGS (-diff));
        else
          GST_DEBUG_OBJECT (src, "QoS: we're behind by %" GST_TIME_FORMAT, GST_TIME_ARGS (diff));

        res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      }
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      break;
  }
  return res;
}

static gboolean
gst_pipewire_src_query (GstBaseSrc *src, GstQuery *query)
{
  gboolean res = FALSE;
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      GST_DEBUG_OBJECT (pwsrc, "latency query min: %" GST_TIME_FORMAT ", max: %" GST_TIME_FORMAT,
                        GST_TIME_ARGS (pwsrc->min_latency), GST_TIME_ARGS (pwsrc->max_latency));

      gst_query_set_latency (query, pwsrc->is_live, pwsrc->min_latency, pwsrc->max_latency);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static void
gst_pipewire_src_get_times (GstBaseSrc *basesrc, GstBuffer *buffer,
                               GstClockTime *start, GstClockTime *end)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  /* sync on the timestamp of the buffer */
  if (gst_base_src_is_live (basesrc)) {
    GstClockTime timestamp = GST_BUFFER_PTS (buffer);

    if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
      GstClockTime duration = GST_BUFFER_DURATION (buffer);

      if (GST_CLOCK_TIME_IS_VALID (duration))
        *end = timestamp + duration;
      *start = timestamp;
    }
  } else {
    *start = GST_CLOCK_TIME_NONE;
    *end = GST_CLOCK_TIME_NONE;
  }

  GST_LOG_OBJECT (pwsrc, "start %" GST_TIME_FORMAT " (%" G_GUINT64_FORMAT
      "), end %" GST_TIME_FORMAT " (%" G_GUINT64_FORMAT ")",
      GST_TIME_ARGS (*start), *start, GST_TIME_ARGS (*end), *end);
}

static void
gst_pipewire_src_set_property (GObject *object, guint prop_id,
                                  const GValue *value, GParamSpec *pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsrc->stream->path);
      pwsrc->stream->path = g_value_dup_string (value);
      break;
    case PROP_TARGET_OBJECT:
      g_free (pwsrc->stream->target_object);
      pwsrc->stream->target_object = g_value_dup_string (value);
      break;
    case PROP_CLIENT_NAME:
      g_free (pwsrc->stream->client_name);
      pwsrc->stream->client_name = g_value_dup_string (value);
      break;
    case PROP_CLIENT_PROPERTIES:
      if (pwsrc->stream->client_properties)
        gst_structure_free (pwsrc->stream->client_properties);
      pwsrc->stream->client_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;
    case PROP_STREAM_PROPERTIES:
      if (pwsrc->stream->stream_properties)
        gst_structure_free (pwsrc->stream->stream_properties);
      pwsrc->stream->stream_properties = gst_structure_copy (gst_value_get_structure (value));
      break;
    case PROP_ALWAYS_COPY:
      if (g_value_get_boolean (value))
        pwsrc->use_bufferpool = USE_BUFFERPOOL_NO;
      else
        pwsrc->use_bufferpool = USE_BUFFERPOOL_YES;
      break;
    case PROP_MIN_BUFFERS:
      pwsrc->min_buffers = g_value_get_int (value);
      break;
    case PROP_MAX_BUFFERS:
      pwsrc->max_buffers = g_value_get_int (value);
      break;
    case PROP_FD:
      pwsrc->stream->fd = g_value_get_int (value);
      break;
    case PROP_RESEND_LAST:
      pwsrc->resend_last = g_value_get_boolean (value);
      break;
    case PROP_KEEPALIVE_TIME:
      pwsrc->keepalive_time = g_value_get_int (value);
      break;
    case PROP_AUTOCONNECT:
      pwsrc->autoconnect = g_value_get_boolean (value);
      break;
    case PROP_USE_BUFFERPOOL:
      if (g_value_get_boolean (value))
        pwsrc->use_bufferpool = USE_BUFFERPOOL_YES;
      else
        pwsrc->use_bufferpool = USE_BUFFERPOOL_NO;
      break;
    case PROP_USE_CAMERA:
      pwsrc->use_camera = g_value_get_boolean (value);
      break;
    case PROP_CAMERA_ID:
      pwsrc->camera_id = g_value_get_int (value);
      break;
    case PROP_ORIENTATION:
      pwsrc->orientation = g_value_get_int (value);
      GST_DEBUG_OBJECT (pwsrc, "Setting orientation to %d degrees", pwsrc->orientation);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_src_get_property (GObject *object, guint prop_id,
                                  GValue *value, GParamSpec *pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsrc->stream->path);
      break;
    case PROP_TARGET_OBJECT:
      g_value_set_string (value, pwsrc->stream->target_object);
      break;
    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsrc->stream->client_name);
      break;
    case PROP_CLIENT_PROPERTIES:
      gst_value_set_structure (value, pwsrc->stream->client_properties);
      break;
    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsrc->stream->stream_properties);
      break;
    case PROP_ALWAYS_COPY:
      g_value_set_boolean (value, !pwsrc->use_bufferpool);
      break;
    case PROP_MIN_BUFFERS:
      g_value_set_int (value, pwsrc->min_buffers);
      break;
    case PROP_MAX_BUFFERS:
      g_value_set_int (value, pwsrc->max_buffers);
      break;
    case PROP_FD:
      g_value_set_int (value, pwsrc->stream->fd);
      break;
    case PROP_RESEND_LAST:
      g_value_set_boolean (value, pwsrc->resend_last);
      break;
    case PROP_KEEPALIVE_TIME:
      g_value_set_int (value, pwsrc->keepalive_time);
      break;
    case PROP_AUTOCONNECT:
      g_value_set_boolean (value, pwsrc->autoconnect);
      break;
    case PROP_USE_BUFFERPOOL:
      g_value_set_boolean (value, !!pwsrc->use_bufferpool);
      break;
    case PROP_USE_CAMERA:
      g_value_set_boolean (value, pwsrc->use_camera);
      break;
    case PROP_CAMERA_ID:
      g_value_set_int (value, pwsrc->camera_id);
      break;
    case PROP_ORIENTATION:
      g_value_set_int (value, pwsrc->orientation);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_pipewire_src_get_caps (GstBaseSrc *basesrc, GstCaps *filter)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);
  GstCaps *caps;

  GST_DEBUG_OBJECT (pwsrc, "Getting caps with filter %" GST_PTR_FORMAT, filter);

  caps = gst_caps_new_empty ();

  GstStructure *structure = gst_structure_new ("video/x-raw",
      "format", G_TYPE_STRING, "I420",
      "width", GST_TYPE_INT_RANGE, 160, 1920,
      "height", GST_TYPE_INT_RANGE, 120, 1920,
      "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 30, 1,
      "interlace-mode", G_TYPE_STRING, "progressive",
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
      "colorimetry", G_TYPE_STRING, "bt709",
      NULL);
  gst_caps_append_structure (caps, structure);

  structure = gst_structure_new ("video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", GST_TYPE_INT_RANGE, 160, 1920,
      "height", GST_TYPE_INT_RANGE, 120, 1920,
      "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 30, 1,
      "interlace-mode", G_TYPE_STRING, "progressive",
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
      "colorimetry", G_TYPE_STRING, "bt709",
      NULL);
  gst_caps_append_structure (caps, structure);

  GST_DEBUG_OBJECT (pwsrc, "Returning caps %" GST_PTR_FORMAT, caps);

  if (filter) {
    GstCaps *intersection;

    intersection = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;

    GST_DEBUG_OBJECT (pwsrc, "Filtered caps %" GST_PTR_FORMAT, caps);
  }

  return caps;
}

static GstClock *
gst_pipewire_src_provide_clock (GstElement *elem)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pwsrc);
  if (!GST_OBJECT_FLAG_IS_SET (pwsrc, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pwsrc->stream->clock && pwsrc->is_live)
    clock = GST_CLOCK_CAST (gst_object_ref (pwsrc->stream->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pwsrc);

  return clock;

clock_disabled:
  GST_DEBUG_OBJECT (pwsrc, "clock provide disabled");
  GST_OBJECT_UNLOCK (pwsrc);
  return NULL;
}

static void
gst_pipewire_src_finalize (GObject *object)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  if (pwsrc->camera) {
    gst_object_unref(pwsrc->camera);
    pwsrc->camera = NULL;
  }

  gst_clear_object (&pwsrc->stream);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
on_state_changed (void *data, GstPipeWireStreamState old,
                  GstPipeWireStreamState state, const char *error)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG_OBJECT (pwsrc, "got stream state change %d -> %d", old, state);

  switch (state) {
    case GST_PIPEWIRE_STREAM_STATE_UNCONNECTED:
    case GST_PIPEWIRE_STREAM_STATE_CONNECTING:
    case GST_PIPEWIRE_STREAM_STATE_PAUSED:
      break;
    case GST_PIPEWIRE_STREAM_STATE_STREAMING:
      if (pwsrc->stream->events.process)
        pwsrc->stream->events.process(pwsrc);
      break;
    case GST_PIPEWIRE_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED,
          ("stream error: %s", error), (NULL));
      break;
  }
}

static void
on_param_changed (void *data, uint32_t id G_GNUC_UNUSED, GstCaps *caps)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG_OBJECT (pwsrc, "got param changed with caps: %" GST_PTR_FORMAT, caps);

  if (id == 0) {
    gst_caps_replace (&pwsrc->caps, caps);

    if (caps) {
      GstStructure *s = gst_caps_get_structure (caps, 0);
      if (s && gst_structure_has_name (s, "video/x-raw")) {
        pwsrc->is_video = TRUE;
        gst_video_info_from_caps (&pwsrc->video_info, caps);
      } else {
        pwsrc->is_video = FALSE;
      }
    } else {
      pwsrc->is_video = FALSE;
    }

    pwsrc->negotiated = (caps != NULL);
  }
}

static void
on_process (void *data)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG_OBJECT (pwsrc, "got process signal");
}

/* unused for now */
static const GstPipeWireStreamEvents stream_events G_GNUC_UNUSED = {
  .state_changed = on_state_changed,
  .param_changed = on_param_changed,
  .process = on_process
};

static gboolean
gst_pipewire_src_negotiate (GstBaseSrc *basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);
  GstCaps *caps;

  if (pwsrc->use_camera && pwsrc->camera) {
    caps = gst_pipewire_camera_get_caps (pwsrc->camera);

    if (caps) {
      caps = gst_caps_make_writable (caps);
      GstStructure *s = gst_caps_get_structure (caps, 0);

      if (gst_structure_has_name (s, "video/x-raw")) {
        gst_structure_set (s,
                          "interlace-mode", G_TYPE_STRING, "progressive",
                          "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                          "colorimetry", G_TYPE_STRING, "bt709",
                          NULL);

        gint fps_n, fps_d;
        if (gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d)) {
          if (fps_n == 0)
            gst_structure_set (s, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
        } else {
          gst_structure_set (s, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
        }
      }

      GST_DEBUG_OBJECT (pwsrc, "Using camera caps (modified): %" GST_PTR_FORMAT, caps);
      gboolean result = gst_base_src_set_caps (basesrc, caps);
      gst_caps_unref (caps);
      return result;
    }
  }

  /* hardcoding this is not a great idea */
  if (pwsrc->is_video) {
    caps = gst_caps_new_simple ("video/x-raw",
                                "format", G_TYPE_STRING, "I420",
                                "width", G_TYPE_INT, 640,
                                "height", G_TYPE_INT, 480,
                                "framerate", GST_TYPE_FRACTION, 30, 1,
                                "interlace-mode", G_TYPE_STRING, "progressive",
                                "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                                "colorimetry", G_TYPE_STRING, "bt709",
                                NULL);
    gst_video_info_from_caps (&pwsrc->video_info, caps);
  } else {
    caps = gst_caps_new_simple ("audio/x-raw",
                                "format", G_TYPE_STRING, "S16LE",
                                "rate", G_TYPE_INT, 44100,
                                "channels", G_TYPE_INT, 2,
                                "layout", G_TYPE_STRING, "interleaved",
                                NULL);
  }

  GST_DEBUG_OBJECT (pwsrc, "negotiated caps %" GST_PTR_FORMAT, caps);
  gboolean result = gst_base_src_set_caps (basesrc, caps);
  gst_caps_unref (caps);

  return result;
}

static GstCaps *
gst_pipewire_src_fixate (GstBaseSrc *basesrc, GstCaps *caps)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);
  GstStructure *structure;
  GstCaps *fixated_caps;
  guint i;

  GST_DEBUG_OBJECT (pwsrc, "Fixating caps %" GST_PTR_FORMAT, caps);

  fixated_caps = gst_caps_make_writable (caps);

  for (i = 0; i < gst_caps_get_size (fixated_caps); i++) {
    structure = gst_caps_get_structure (fixated_caps, i);

    if (gst_structure_has_name (structure, "video/x-raw")) {
      if (!gst_structure_has_field (structure, "format"))
        gst_structure_set (structure, "format", G_TYPE_STRING, "I420", NULL);

      if (!gst_structure_has_field_typed (structure, "width", G_TYPE_INT))
        gst_structure_set (structure, "width", G_TYPE_INT, 640, NULL);
      else
        gst_structure_fixate_field_nearest_int (structure, "width", 640);

      if (!gst_structure_has_field_typed (structure, "height", G_TYPE_INT))
        gst_structure_set (structure, "height", G_TYPE_INT, 480, NULL);
      else
        gst_structure_fixate_field_nearest_int (structure, "height", 480);

      if (!gst_structure_has_field (structure, "framerate")) {
        gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
      } else {
        if (!gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1))
          gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, 30, 1, NULL);
      }

      if (!gst_structure_has_field (structure, "interlace-mode"))
        gst_structure_set(structure, "interlace-mode", G_TYPE_STRING, "progressive", NULL);

      if (!gst_structure_has_field (structure, "pixel-aspect-ratio"))
        gst_structure_set(structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);

      if (!gst_structure_has_field (structure, "colorimetry"))
        gst_structure_set(structure, "colorimetry", G_TYPE_STRING, "bt709", NULL);
    }
  }

  GstCaps *result = GST_BASE_SRC_CLASS (gst_pipewire_src_parent_class)->fixate (basesrc, fixated_caps);

  GST_DEBUG_OBJECT (pwsrc, "Fixated caps to %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_pipewire_src_unlock (GstBaseSrc *basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  GST_DEBUG_OBJECT (pwsrc, "setting flushing");
  pwsrc->flushing = TRUE;

  return TRUE;
}

static gboolean
gst_pipewire_src_unlock_stop (GstBaseSrc *basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  GST_DEBUG_OBJECT (pwsrc, "unsetting flushing");
  pwsrc->flushing = FALSE;

  return TRUE;
}

static void
gst_pipewire_src_class_init (GstPipeWireSrcClass *klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pipewire_src_finalize;
  gobject_class->set_property = gst_pipewire_src_set_property;
  gobject_class->get_property = gst_pipewire_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The source path to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_TARGET_OBJECT,
                                   g_param_spec_string ("target-object",
                                                        "Target object",
                                                        "The source name/serial to connect to (NULL = default)",
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
                                                       "client properties",
                                                       "list of client properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STREAM_PROPERTIES,
                                   g_param_spec_boxed ("stream-properties",
                                                       "stream properties",
                                                       "list of stream properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_ALWAYS_COPY,
                                   g_param_spec_boolean ("always-copy",
                                                         "Always copy",
                                                         "Always copy the buffer and data",
                                                         DEFAULT_ALWAYS_COPY,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS |
                                                         G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_MIN_BUFFERS,
                                   g_param_spec_int ("min-buffers",
                                                     "Min Buffers",
                                                     "Minimum number of buffers to negotiate with PipeWire",
                                                     1, G_MAXINT, DEFAULT_MIN_BUFFERS,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_MAX_BUFFERS,
                                   g_param_spec_int ("max-buffers",
                                                     "Max Buffers",
                                                     "Maximum number of buffers to negotiate with PipeWire",
                                                     1, G_MAXINT, DEFAULT_MAX_BUFFERS,
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
                                   PROP_RESEND_LAST,
                                   g_param_spec_boolean ("resend-last",
                                                         "Resend last",
                                                         "Resend last buffer on EOS",
                                                         DEFAULT_RESEND_LAST,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_KEEPALIVE_TIME,
                                   g_param_spec_int ("keepalive-time",
                                                     "Keepalive Time",
                                                     "Periodically send last buffer (in milliseconds, 0 = disabled)",
                                                     0, G_MAXINT, DEFAULT_KEEPALIVE_TIME,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_AUTOCONNECT,
                                   g_param_spec_boolean ("autoconnect",
                                                         "Connect automatically",
                                                         "Attempt to find a peer to connect to",
                                                         DEFAULT_AUTOCONNECT,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_USE_BUFFERPOOL,
                                   g_param_spec_boolean ("use-bufferpool",
                                                         "Use bufferpool",
                                                         "Use bufferpool (default: true for video, false for audio)",
                                                         DEFAULT_USE_BUFFERPOOL,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_ORIENTATION,
                                   g_param_spec_int ("orientation",
                                                     "Orientation",
                                                     "Camera sensor orientation in degrees (0, 90, 180, 270)",
                                                     0, 270, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));


  g_object_class_install_property (gobject_class,
                                   PROP_USE_CAMERA,
                                   g_param_spec_boolean ("use-camera",
                                                         "Use Camera",
                                                         "Use DroidMedia camera as source",
                                                         DEFAULT_USE_CAMERA,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CAMERA_ID,
                                   g_param_spec_int ("camera-id",
                                                     "Camera ID",
                                                     "DroidMedia camera ID to use (0=back, 1=front typically)",
                                                     0, G_MAXINT, DEFAULT_CAMERA_ID,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pipewire_src_provide_clock;
  gstelement_class->change_state = gst_pipewire_src_change_state;
  gstelement_class->send_event = gst_pipewire_src_send_event;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire source", "Source/Audio/Video",
      "Uses PipeWire create audio/video", "Bardia Moshiri <bardia@furilabs.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_src_template));

  gstbasesrc_class->get_caps = gst_pipewire_src_get_caps;
  gstbasesrc_class->negotiate = gst_pipewire_src_negotiate;
  gstbasesrc_class->fixate = gst_pipewire_src_fixate;
  gstbasesrc_class->unlock = gst_pipewire_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pipewire_src_unlock_stop;
  gstbasesrc_class->start = gst_pipewire_src_start;
  gstbasesrc_class->stop = gst_pipewire_src_stop;
  gstbasesrc_class->event = gst_pipewire_src_event;
  gstbasesrc_class->query = gst_pipewire_src_query;
  gstbasesrc_class->get_times = gst_pipewire_src_get_times;
  gstpushsrc_class->create = gst_pipewire_src_create;

  GST_DEBUG_CATEGORY_INIT (pipewire_src_debug, "pipewiresrc", 0, "PipeWire Source");
}

static void
gst_pipewire_src_init (GstPipeWireSrc *src)
{
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  src->stream = gst_pipewire_stream_new (GST_ELEMENT (src));

  src->use_bufferpool = DEFAULT_USE_BUFFERPOOL;
  src->min_buffers = DEFAULT_MIN_BUFFERS;
  src->max_buffers = DEFAULT_MAX_BUFFERS;
  src->resend_last = DEFAULT_RESEND_LAST;
  src->keepalive_time = DEFAULT_KEEPALIVE_TIME;
  src->autoconnect = DEFAULT_AUTOCONNECT;
  src->min_latency = 0;
  src->max_latency = GST_CLOCK_TIME_NONE;

  src->use_camera = DEFAULT_USE_CAMERA;
  src->camera_id = DEFAULT_CAMERA_ID;
  src->camera = NULL;
  src->orientation = 0;
  src->qos_delay = 0;

  gst_base_src_set_blocksize (GST_BASE_SRC (src), 0);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (src), TRUE);
}

/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <libyuv.h>
#include <libyuv/rotate.h>

#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstpipewirecamera.h"
#include "gstpipewiresrc.h"

static DroidMediaCameraConstants CAMERA_CONSTANTS;
static DroidMediaPixelFormatConstants PIXEL_FORMAT_CONSTANTS;
static DroidMediaColourFormatConstants COLOR_FORMAT_CONSTANTS;

GST_DEBUG_CATEGORY_STATIC (pipewire_camera_debug);
#define GST_CAT_DEFAULT pipewire_camera_debug

G_DEFINE_TYPE (GstPipeWireCamera, gst_pipewire_camera, G_TYPE_OBJECT)

/* comes from droidcam2v4l2 */
static size_t
get_parameter_value (const char *params, const char *parameter_name,
                     char *value_out, size_t value_len)
{
  char *original_params = strdup (params);
  char *handle = original_params;
  size_t value_size = 0;

  while (original_params && *original_params) {
    const char *key = original_params;
    char *value = strchr (original_params, '=');
    if (!value)
      break;

    *value = '\0';
    value++;

    char *next = strchr (value, ';');
    if (next) {
      *next = '\0';
      next++;
    }

    original_params = next;

    if (strcmp (key, parameter_name) == 0) {
      if (value_out && value_len) {
        strncpy (value_out, value, value_len - 1);
        value_out[value_len - 1] = '\0';
      }
      value_size = strlen (value);
      break;
    }
  }

  free (handle);
  return value_size;
}

static void
gst_pipewire_camera_stop (GstPipeWireCamera *camera)
{
  if (camera->camera) {
    GST_DEBUG_OBJECT (camera, "Stopping camera");

    droid_media_camera_stop_preview (camera->camera);

    droid_media_camera_unlock (camera->camera);
    droid_media_camera_disconnect (camera->camera);
    camera->camera = NULL;

    g_mutex_lock (&camera->queue_mutex);
    while (!g_queue_is_empty (camera->frame_queue)) {
      GstBuffer *buf = GST_BUFFER (g_queue_pop_head (camera->frame_queue));
      gst_buffer_unref (buf);
    }

    g_mutex_unlock (&camera->queue_mutex);
  }

  GST_INFO_OBJECT (camera, "Camera stopped. Stats: received=%u, dropped=%u",
                   camera->frames_received, camera->frames_dropped);
  camera->is_running = FALSE;
}

static void
gst_pipewire_camera_finalize (GObject *object)
{
  GstPipeWireCamera *camera = GST_PIPEWIRE_CAMERA (object);

  if (camera->is_running)
    gst_pipewire_camera_stop (camera);

  g_mutex_clear (&camera->queue_mutex);

  pthread_mutex_destroy (&camera->buffer_lock);

  if (camera->frame_queue) {
    while (!g_queue_is_empty (camera->frame_queue)) {
      GstBuffer *buf = GST_BUFFER (g_queue_pop_head (camera->frame_queue));
      gst_buffer_unref (buf);
    }
    g_queue_free (camera->frame_queue);
  }

  G_OBJECT_CLASS (gst_pipewire_camera_parent_class)->finalize (object);
}

static void
preview_frame_callback (void *userdata, DroidMediaData *data)
{
  GstPipeWireCamera *camera = (GstPipeWireCamera *)userdata;
  if (!camera->is_running)
    return;

  camera->frames_received++;

  g_mutex_lock (&camera->queue_mutex);
  gboolean queue_full = (g_queue_get_length (camera->frame_queue) >= camera->max_queue_length);
  g_mutex_unlock (&camera->queue_mutex);

  if (queue_full) {
    /* skip this frame since we already have more waiting */
    camera->frames_dropped++;
    GST_LOG_OBJECT (camera, "Queue already has a frame, dropping this one");
    return;
  }

  GST_LOG_OBJECT (camera, "Processing camera frame: size=%zd", data->size);

  ssize_t y_size = camera->width * camera->height;
  ssize_t uv_size = camera->width * camera->height / 4;
  ssize_t expected_size = y_size + uv_size * 2; /* Y + U + V */

  if (data->size < expected_size) {
    GST_WARNING_OBJECT (camera, "Frame data size (%zd) is smaller than expected (%zd)",
                        data->size, expected_size);
    return;
  }

  int width = camera->width;
  int height = camera->height;
  int rotated_width = width;
  int rotated_height = height;

  int final_rotation = camera->rotation;
  if (camera->user_orientation != 0)
    final_rotation = camera->user_orientation;

  if (final_rotation == 90 || final_rotation == 270) {
    rotated_width = height;
    rotated_height = width;
  }

  GstBuffer *buffer = gst_buffer_new_allocate (NULL,
                                               rotated_width * rotated_height * 3 / 2,
                                               NULL);

  if (!buffer) {
    GST_ERROR_OBJECT (camera, "Failed to allocate buffer");
    return;
  }

  GstMapInfo map;
  gst_buffer_map (buffer, &map, GST_MAP_WRITE);

  if (final_rotation == 0) {
    memcpy (map.data, data->data, y_size);

    /* convert from NV21 (VU interleaved) to I420 (planar) */
    uint8_t *src_vu = ((uint8_t*)data->data) + y_size;
    uint8_t *dst_u = map.data + y_size;
    uint8_t *dst_v = map.data + y_size + (y_size / 4);

    int num_uv_samples = (y_size / 4);
    for (int i = 0; i < num_uv_samples; i++) {
      dst_v[i] = src_vu[i * 2];     /* V plane (first in NV21) */
      dst_u[i] = src_vu[i * 2 + 1]; /* U plane (second in NV21) */
    }
  } else {
    uint8_t *temp_i420 = g_malloc (width * height * 3 / 2);
    if (!temp_i420) {
      GST_ERROR_OBJECT (camera, "Failed to allocate temporary buffer for rotation");
      gst_buffer_unmap (buffer, &map);
      gst_buffer_unref (buffer);
      return;
    }

    memcpy (temp_i420, data->data, y_size); /* Y plane */

    uint8_t *src_vu = ((uint8_t*)data->data) + y_size;
    uint8_t *temp_u = temp_i420 + y_size;
    uint8_t *temp_v = temp_i420 + y_size + (y_size / 4);

    int num_uv_samples = (y_size / 4);
    for (int i = 0; i < num_uv_samples; i++) {
      temp_v[i] = src_vu[i * 2];     /* V plane */
      temp_u[i] = src_vu[i * 2 + 1]; /* U plane */
    }

    uint8_t *dst_y = map.data;
    uint8_t *dst_u = map.data + (rotated_width * rotated_height);
    uint8_t *dst_v = map.data + (rotated_width * rotated_height) + (rotated_width * rotated_height / 4);

    int src_stride_y = width;
    int src_stride_uv = width / 2;
    int dst_stride_y = rotated_width;
    int dst_stride_uv = rotated_width / 2;

    enum RotationMode mode;
    switch (final_rotation) {
      case 90:
        mode = kRotate90;
        break;
      case 180:
        mode = kRotate180;
        break;
      case 270:
        mode = kRotate270;
        break;
      default:
        mode = kRotate0;
        break;
    }

    I420Rotate (temp_i420, src_stride_y,
                temp_u, src_stride_uv,
                temp_v, src_stride_uv,
                dst_y, dst_stride_y,
                dst_u, dst_stride_uv,
                dst_v, dst_stride_uv,
                width, height, mode);

    g_free (temp_i420);
  }

  gst_buffer_unmap (buffer, &map);

  GstClockTime now = g_get_monotonic_time () * 1000;
  GST_BUFFER_PTS (buffer) = now;
  GST_BUFFER_DTS (buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (buffer) = GST_SECOND / camera->fps;

  g_mutex_lock (&camera->queue_mutex);

  g_queue_push_tail (camera->frame_queue, buffer);

  droid_media_camera_start_auto_focus (camera->camera);

  GST_LOG_OBJECT (camera, "Added frame to queue, length now: %u",
                  g_queue_get_length (camera->frame_queue));

  g_mutex_unlock (&camera->queue_mutex);
}

static gboolean
gst_pipewire_camera_init_camera (GstPipeWireCamera *camera)
{
  DroidMediaCameraCallbacks callbacks;
  DroidMediaCameraInfo info;
  char parameter_buffer[1024];

  GST_DEBUG_OBJECT (camera, "Initializing camera id %d", camera->camera_id);

  camera->camera = droid_media_camera_connect (camera->camera_id);
  if (!camera->camera) {
    GST_ERROR_OBJECT (camera, "Failed to connect to camera %d", camera->camera_id);
    return FALSE;
  }

  droid_media_camera_get_info (&info, camera->camera_id);
  GST_DEBUG_OBJECT (camera, "Camera %d orientation: %d", camera->camera_id, info.orientation);

  camera->rotation = info.orientation;

  if (!droid_media_camera_lock (camera->camera)) {
    GST_ERROR_OBJECT (camera, "Failed to lock camera");
    droid_media_camera_disconnect (camera->camera);
    camera->camera = NULL;
    return FALSE;
  }

  char supported_sizes[1024];
  bool found_size = false;

  char *current_params = droid_media_camera_get_parameters (camera->camera);
  GST_DEBUG_OBJECT (camera, "Current parameters: %s", current_params);

  if (get_parameter_value (current_params, "preview-size-values",
                           supported_sizes, sizeof (supported_sizes))) {
    GST_DEBUG_OBJECT (camera, "Supported preview sizes: %s", supported_sizes);

    char *sizes = strdup (supported_sizes);
    char *token = strtok (sizes, ",");

    while (token != NULL) {
      int w, h;
      if (sscanf (token, "%dx%d", &w, &h) == 2) {
        GST_DEBUG_OBJECT (camera, "Found supported size: %dx%d", w, h);

        if (w == camera->width && h == camera->height) {
          found_size = true;
          break;
        }

        /* it might be off but something is better than nothing */
        if (!found_size) {
          camera->width = w;
          camera->height = h;
          found_size = true;
          GST_DEBUG_OBJECT (camera, "Using size: %dx%d", camera->width, camera->height);
        }
      }
      token = strtok (NULL, ",");
    }
    free (sizes);
  } else {
    GST_WARNING_OBJECT (camera, "Could not get supported preview sizes");
  }

  /* swap width and height for rotated output */
  if (camera->rotation == 90 || camera->rotation == 270)
    gst_video_info_set_format (&camera->video_info, GST_VIDEO_FORMAT_I420,
                               camera->height, camera->width);
  else
    gst_video_info_set_format (&camera->video_info, GST_VIDEO_FORMAT_I420,
                               camera->width, camera->height);

  sprintf (parameter_buffer, "preview-size=%dx%d", camera->width, camera->height);
  if (!droid_media_camera_set_parameters (camera->camera, parameter_buffer))
    GST_WARNING_OBJECT (camera, "Failed to set preview size");

  sprintf (parameter_buffer, "preview-frame-rate=%d", camera->fps);
  if (!droid_media_camera_set_parameters (camera->camera, parameter_buffer))
    GST_WARNING_OBJECT (camera, "Failed to set frame rate");

  sprintf (parameter_buffer, "preview-format=yuv420p");
  if (!droid_media_camera_set_parameters (camera->camera, parameter_buffer)) {
    GST_WARNING_OBJECT (camera, "Failed to set preview format to yuv420p");

    sprintf (parameter_buffer, "preview-format=yuv420sp");
    if (!droid_media_camera_set_parameters (camera->camera, parameter_buffer))
      /* continue anyway as the format may be set automatically */
      GST_WARNING_OBJECT (camera, "Failed to set preview format to yuv420sp");
  }

  char size_value[32] = {0};
  int actual_width = camera->width;
  int actual_height = camera->height;

  char *actual_params = droid_media_camera_get_parameters (camera->camera);
  if (get_parameter_value (actual_params, "preview-size", size_value, sizeof (size_value))) {
    sscanf (size_value, "%dx%d", &actual_width, &actual_height);
    GST_INFO_OBJECT (camera, "Camera is using preview size: %dx%d", actual_width, actual_height);

    camera->width = actual_width;
    camera->height = actual_height;

    /* swap width and height for rotated output */
    if (camera->rotation == 90 || camera->rotation == 270)
      gst_video_info_set_format (&camera->video_info, GST_VIDEO_FORMAT_I420,
                                 camera->height, camera->width);
    else
      gst_video_info_set_format (&camera->video_info, GST_VIDEO_FORMAT_I420,
                                 camera->width, camera->height);
  }
  free (actual_params);

  droid_media_camera_set_preview_callback_flags (camera->camera,
      CAMERA_CONSTANTS.CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK);

  memset (&callbacks, 0, sizeof (callbacks));
  callbacks.preview_frame_cb = preview_frame_callback;
  droid_media_camera_set_callbacks (camera->camera, &callbacks, camera);

  return TRUE;
}

static void
gst_pipewire_camera_init (GstPipeWireCamera *camera)
{
  static gboolean droidmedia_initialized = FALSE;

  if (!droidmedia_initialized) {
    droid_media_init ();
    droid_media_camera_constants_init (&CAMERA_CONSTANTS);
    droid_media_pixel_format_constants_init (&PIXEL_FORMAT_CONSTANTS);
    droid_media_colour_format_constants_init (&COLOR_FORMAT_CONSTANTS);
    droidmedia_initialized = TRUE;
  }

  g_mutex_init (&camera->queue_mutex);
  camera->frame_queue = g_queue_new ();
  camera->max_queue_length = 3;

  pthread_mutex_init (&camera->buffer_lock, NULL);

  camera->camera = NULL;
  camera->camera_id = DEFAULT_CAMERA_ID;
  camera->width = DEFAULT_CAMERA_WIDTH;
  camera->height = DEFAULT_CAMERA_HEIGHT;
  camera->fps = DEFAULT_CAMERA_FPS;
  camera->rotation = 0;
  camera->user_orientation = 0;
  camera->is_running = FALSE;
  camera->frames_received = 0;
  camera->frames_dropped = 0;

  gst_video_info_set_format (&camera->video_info, GST_VIDEO_FORMAT_I420,
                             camera->width, camera->height);
}

GstBuffer *
gst_pipewire_camera_get_latest_frame (GstPipeWireCamera *camera)
{
  GstBuffer *buffer = NULL;

  g_mutex_lock (&camera->queue_mutex);

  if (!g_queue_is_empty (camera->frame_queue))
    /* no need to ref the buffer, as we're transferring ownership */
    buffer = GST_BUFFER (g_queue_pop_head (camera->frame_queue));

  g_mutex_unlock (&camera->queue_mutex);

  return buffer;
}

static void
gst_pipewire_camera_class_init (GstPipeWireCameraClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_pipewire_camera_finalize;

  GST_DEBUG_CATEGORY_INIT (pipewire_camera_debug, "pipewirecamera", 0,
                           "DroidMedia Camera for Pipewire");
}

GstPipeWireCamera *
gst_pipewire_camera_new (GstPipeWireSrc *pwsrc)
{
  GstPipeWireCamera *camera = g_object_new (GST_TYPE_PIPEWIRE_CAMERA, NULL);

  camera->pwsrc = pwsrc;

  GST_DEBUG_OBJECT (camera, "Created new DroidMedia camera for PipeWire");

  return camera;
}

gboolean
gst_pipewire_camera_open (GstPipeWireCamera *camera)
{
  if (camera->is_running) {
    GST_WARNING_OBJECT (camera, "Camera already running");
    return TRUE;
  }

  if (!gst_pipewire_camera_init_camera (camera)) {
    GST_ERROR_OBJECT (camera, "Failed to initialize camera");
    return FALSE;
  }

  return TRUE;
}

void
gst_pipewire_camera_close (GstPipeWireCamera *camera)
{
  if (camera->is_running)
    gst_pipewire_camera_stop (camera);

  GST_DEBUG_OBJECT (camera, "Camera closed");
}

static gboolean
gst_pipewire_camera_start (GstPipeWireCamera *camera)
{
  if (!camera->camera) {
    if (!gst_pipewire_camera_init_camera (camera))
      return FALSE;
  }

  /* this should be moved elsewhere */
  droid_media_camera_start_auto_focus (camera->camera);

  droid_media_camera_set_parameters (camera->camera, "auto-exposure=on");

  droid_media_camera_set_parameters (camera->camera, "preview-format=yuv420sp");

  /* give camera time to set parameters */
  g_usleep (50000);

  if (!droid_media_camera_start_preview (camera->camera)) {
    GST_ERROR_OBJECT (camera, "Failed to start preview");
    droid_media_camera_unlock (camera->camera);
    droid_media_camera_disconnect (camera->camera);
    camera->camera = NULL;
    return FALSE;
  }

  /* give camera time to stabilize */
  g_usleep (100000);

  g_mutex_lock (&camera->queue_mutex);
  while (!g_queue_is_empty (camera->frame_queue)) {
    GstBuffer *buf = GST_BUFFER (g_queue_pop_head (camera->frame_queue));
    gst_buffer_unref (buf);
  }
  g_mutex_unlock (&camera->queue_mutex);

  camera->frames_received = 0;
  camera->frames_dropped = 0;
  camera->is_running = TRUE;
  GST_INFO_OBJECT (camera, "Camera preview started successfully");

  return TRUE;
}

void
gst_pipewire_camera_stop_streaming (GstPipeWireCamera *camera)
{
  gst_pipewire_camera_stop (camera);
}

gboolean
gst_pipewire_camera_start_streaming (GstPipeWireCamera *camera)
{
  g_return_val_if_fail (camera != NULL, FALSE);

  GST_DEBUG_OBJECT (camera, "Starting camera streaming");

  return gst_pipewire_camera_start (camera);
}

GstCaps *
gst_pipewire_camera_get_caps (GstPipeWireCamera *camera)
{
  return gst_video_info_to_caps (&camera->video_info);
}

gboolean
gst_pipewire_camera_set_format (GstPipeWireCamera *camera, GstCaps *caps)
{
  GstVideoInfo info;

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_ERROR_OBJECT (camera, "Failed to parse caps");
    return FALSE;
  }

  camera->width = info.width;
  camera->height = info.height;

  camera->video_info = info;

  if (camera->is_running) {
    gst_pipewire_camera_stop (camera);
    return gst_pipewire_camera_start (camera);
  }

  return TRUE;
}

void
gst_pipewire_camera_set_orientation (GstPipeWireCamera *camera, gint orientation)
{
  GST_DEBUG_OBJECT (camera, "Setting user orientation to %d (was %d)",
                    orientation, camera->user_orientation);

  camera->user_orientation = orientation;

  if (camera->is_running) {
    GST_DEBUG_OBJECT (camera, "Restarting camera to apply new orientation");
    gst_pipewire_camera_stop (camera);
    gst_pipewire_camera_start (camera);
  }
}

void
gst_pipewire_camera_get_info (GstPipeWireCamera *camera,
                              gint *width, gint *height, gint *rotation)
{
  if (width)
    *width = camera->width;
  if (height)
    *height = camera->height;
  if (rotation)
    *rotation = (camera->user_orientation != 0) ? camera->user_orientation : camera->rotation;
}

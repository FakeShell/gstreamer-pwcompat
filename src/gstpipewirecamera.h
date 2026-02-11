/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#ifndef __GST_PIPEWIRE_CAMERA_H__
#define __GST_PIPEWIRE_CAMERA_H__

#include "config.h"
#include <gst/gst.h>
#include <gst/video/video.h>

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacamera.h>
#include <droidmedia/droidmediaconstants.h>

G_BEGIN_DECLS

typedef struct _GstPipeWireSrc GstPipeWireSrc;

#define DEFAULT_CAMERA_ID 0
#define DEFAULT_CAMERA_WIDTH 1280
#define DEFAULT_CAMERA_HEIGHT 720
#define DEFAULT_CAMERA_FPS 30

/**
 * GstPipeWireCamera:
 *
 * Opaque data structure for camera functionality.
 */
struct _GstPipeWireCamera {
  GObject parent;

  GstElement *pipeline;
  GstPipeWireSrc *pwsrc;

  DroidMediaCamera *camera;
  int camera_id;
  int width;
  int height;
  int rotation;
  int user_orientation;
  int fps;

  pthread_mutex_t buffer_lock;
  gboolean is_running;
  GstVideoInfo video_info;

  GMutex queue_mutex;
  GQueue *frame_queue;
  guint max_queue_length;

  guint frames_received;
  guint frames_dropped;
};

#define GST_TYPE_PIPEWIRE_CAMERA (gst_pipewire_camera_get_type())
G_DECLARE_FINAL_TYPE (GstPipeWireCamera, gst_pipewire_camera, GST, PIPEWIRE_CAMERA, GObject)

GstPipeWireCamera *gst_pipewire_camera_new (GstPipeWireSrc *pwsrc);

gboolean gst_pipewire_camera_open (GstPipeWireCamera *camera);
void gst_pipewire_camera_close (GstPipeWireCamera *camera);

gboolean gst_pipewire_camera_start_streaming (GstPipeWireCamera *camera);
void gst_pipewire_camera_stop_streaming (GstPipeWireCamera *camera);

GstCaps *gst_pipewire_camera_get_caps (GstPipeWireCamera *camera);
gboolean gst_pipewire_camera_set_format (GstPipeWireCamera *camera, GstCaps *caps);
void gst_pipewire_camera_get_info (GstPipeWireCamera *camera,
                                   gint *width, gint *height, gint *rotation);

GstBuffer *gst_pipewire_camera_get_latest_frame (GstPipeWireCamera *camera);
void gst_pipewire_camera_set_orientation (GstPipeWireCamera *camera, gint orientation);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CAMERA_H__ */

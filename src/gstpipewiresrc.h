/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#ifndef __GST_PIPEWIRE_SRC_H__
#define __GST_PIPEWIRE_SRC_H__

#include "gstpipewirestream.h"
#include "gstpipewireshared.h"
#include "gstpipewirecamera.h"

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#include <gst/video/video.h>
#include <gst/audio/audio.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SRC (gst_pipewire_src_get_type())
#define GST_PIPEWIRE_SRC_CAST(obj) ((GstPipeWireSrc *) (obj))
G_DECLARE_FINAL_TYPE (GstPipeWireSrc, gst_pipewire_src, GST, PIPEWIRE_SRC, GstPushSrc)

/**
 * GstPipeWireSrc:
 *
 * Opaque data structure.
 */
struct _GstPipeWireSrc {
  GstPushSrc element;

  GstPipeWireStream *stream;

  gint use_bufferpool;
  gint min_buffers;
  gint max_buffers;
  gboolean resend_last;
  gint keepalive_time;
  gboolean autoconnect;

  GstCaps *caps;
  GstCaps *possible_caps;

  gboolean is_video;
  GstVideoInfo video_info;

  gboolean negotiated;
  gboolean flushing;
  gboolean started;
  gboolean eos;

  gboolean is_live;
  int64_t delay;
  GstClockTime min_latency;
  GstClockTime max_latency;

  GstBuffer *last_buffer;

  GstClockTime previous_ts;
  GstClockTime last_push_time;

  GstPipeWireCamera *camera;
  gboolean use_camera;
  gint camera_id;
  gint orientation;

  GstClockTime qos_delay;
};

G_END_DECLS

#endif /* __GST_PIPEWIRE_SRC_H__ */

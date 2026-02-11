/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <string.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include "gstpipewireformat.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_format_debug);
#define GST_CAT_DEFAULT pipewire_format_debug

typedef enum {
  ALT_FORMAT_UNKNOWN = 0,
  ALT_FORMAT_VIDEO_RAW,
  ALT_FORMAT_AUDIO_RAW,
  ALT_FORMAT_VIDEO_ENCODED,
  ALT_FORMAT_AUDIO_ENCODED
} AltFormatType;

typedef struct {
  const char *media_type;
  const char *media_subtype;
  AltFormatType format_type;
} AltFormatMapping;

static const AltFormatMapping format_mapping[] = {
  { "video/x-raw", NULL, ALT_FORMAT_VIDEO_RAW },
  { "audio/x-raw", NULL, ALT_FORMAT_AUDIO_RAW },
  { "image/jpeg", NULL, ALT_FORMAT_VIDEO_ENCODED },
  { "video/x-h264", NULL, ALT_FORMAT_VIDEO_ENCODED },
  { "video/mpeg", NULL, ALT_FORMAT_VIDEO_ENCODED },
  { "audio/mpeg", NULL, ALT_FORMAT_AUDIO_ENCODED },
  { "audio/x-flac", NULL, ALT_FORMAT_AUDIO_ENCODED },
  { NULL, NULL, ALT_FORMAT_UNKNOWN }
};

static void
init_debug (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (pipewire_format_debug, "pipewireformat", 0,
      "debug category for pipewireformat");
    g_once_init_leave (&initialized, 1);
  }
}

static AltFormatType
find_format_type (const gchar *media_type)
{
  int i;

  for (i = 0; format_mapping[i].media_type != NULL; i++) {
    if (g_str_equal (format_mapping[i].media_type, media_type))
      return format_mapping[i].format_type;
  }

  return ALT_FORMAT_UNKNOWN;
}

GPtrArray *
gst_pipewire_caps_to_format (GstCaps *caps)
{
  GPtrArray *formats;
  gint i, n;

  init_debug();

  GST_DEBUG ("converting caps %" GST_PTR_FORMAT, caps);

  formats = g_ptr_array_new_with_free_func (g_free);
  n = gst_caps_get_size (caps);

  for (i = 0; i < n; i++) {
    GstStructure *structure = gst_caps_get_structure (caps, i);
    const char *media_type = gst_structure_get_name (structure);
    AltFormatType format_type = find_format_type (media_type);

    if (format_type != ALT_FORMAT_UNKNOWN) {
      /* just store the format type as an integer  for now */
      uint32_t *format_id = g_new (uint32_t, 1);
      *format_id = (uint32_t)format_type;

      GST_DEBUG ("  adding format %u for media type %s", format_type, media_type);
      g_ptr_array_add (formats, format_id);
    }
  }

  return formats;
}

GstCaps *
gst_pipewire_format_to_caps (uint32_t format_id)
{
  GstCaps *caps = NULL;
  AltFormatType format_type = (AltFormatType)format_id;

  init_debug();

  GST_DEBUG ("converting format %u to caps", format_id);

  /* create some basic caps based on the format type for now */
  switch (format_type) {
    case ALT_FORMAT_VIDEO_RAW:
      caps = gst_caps_new_simple ("video/x-raw",
                                  "format", G_TYPE_STRING, "RGBA",
                                  "width", G_TYPE_INT, 640,
                                  "height", G_TYPE_INT, 480,
                                  "framerate", GST_TYPE_FRACTION, 30, 1,
                                  NULL);
      break;
    case ALT_FORMAT_AUDIO_RAW:
      caps = gst_caps_new_simple ("audio/x-raw",
                                  "format", G_TYPE_STRING, "S16LE",
                                  "rate", G_TYPE_INT, 44100,
                                  "channels", G_TYPE_INT, 2,
                                  "layout", G_TYPE_STRING, "interleaved",
                                  NULL);
      break;
    case ALT_FORMAT_VIDEO_ENCODED:
      caps = gst_caps_new_simple ("video/x-h264",
                                  "stream-format", G_TYPE_STRING, "byte-stream",
                                  "alignment", G_TYPE_STRING, "au",
                                  NULL);
      break;
    case ALT_FORMAT_AUDIO_ENCODED:
      caps = gst_caps_new_simple ("audio/mpeg",
                                  "mpegversion", G_TYPE_INT, 4,
                                  NULL);
      break;
    default:
      GST_WARNING ("unknown format type %u", format_type);
      break;
  }

  GST_DEBUG ("  created caps %" GST_PTR_FORMAT, caps);

  return caps;
}

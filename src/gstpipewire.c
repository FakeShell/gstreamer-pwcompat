/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "gstpipewirecore.h"

#include <gst/gst.h>

#include "gstpipewiresrc.h"
#include "gstpipewiresink.h"
#include "gstpipewiredeviceprovider.h"

GST_DEBUG_CATEGORY (pipewire_debug);
#define GST_CAT_DEFAULT pipewire_debug

static gboolean
plugin_init (GstPlugin *plugin)
{
  GST_DEBUG_CATEGORY_INIT (pipewire_debug, "pipewire", 0, "PipeWire elements");

  gst_element_register (plugin, "pipewiresrc", GST_RANK_PRIMARY + 1,
      GST_TYPE_PIPEWIRE_SRC);
  gst_element_register (plugin, "pipewiresink", GST_RANK_NONE,
      GST_TYPE_PIPEWIRE_SINK);

  if (!gst_device_provider_register (plugin, "pipewiredeviceprovider",
       GST_RANK_PRIMARY + 1, GST_TYPE_PIPEWIRE_DEVICE_PROVIDER))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pipewire,
    "Uses PipeWire to handle media streams",
    plugin_init, VERSION, "MIT/X11", "pipewire", "pipewire.org")

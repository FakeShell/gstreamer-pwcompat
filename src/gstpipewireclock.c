/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <gst/gst.h>

#include "gstpipewireclock.h"

GST_DEBUG_CATEGORY_STATIC (gst_pipewire_clock_debug_category);
#define GST_CAT_DEFAULT gst_pipewire_clock_debug_category

G_DEFINE_TYPE (GstPipeWireClock, gst_pipewire_clock, GST_TYPE_SYSTEM_CLOCK);

GstClock *
gst_pipewire_clock_new (GstPipeWireStream *stream, GstClockTime last_time)
{
  GstPipeWireClock *clock;

  clock = g_object_new (GST_TYPE_PIPEWIRE_CLOCK, NULL);
  g_weak_ref_set (&clock->stream, stream);
  clock->last_time = last_time;
  clock->time_offset = last_time;

  return GST_CLOCK_CAST (clock);
}

static GstClockTime
gst_pipewire_clock_get_internal_time (GstClock * clock)
{
  GstPipeWireClock *pclock = (GstPipeWireClock *) clock;
  GstClockTime result;
  g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&pclock->stream);

  if (G_UNLIKELY (!s))
    return pclock->last_time;

  result = gst_clock_get_time (clock);

  pclock->last_time = result;

  return result;
}

static void
gst_pipewire_clock_finalize (GObject * object)
{
  GstPipeWireClock *clock = GST_PIPEWIRE_CLOCK (object);

  GST_DEBUG_OBJECT (clock, "finalize");

  g_weak_ref_set (&clock->stream, NULL);

  G_OBJECT_CLASS (gst_pipewire_clock_parent_class)->finalize (object);
}

static void
gst_pipewire_clock_class_init (GstPipeWireClockClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstClockClass *gstclock_class = GST_CLOCK_CLASS (klass);

  gobject_class->finalize = gst_pipewire_clock_finalize;

  gstclock_class->get_internal_time = gst_pipewire_clock_get_internal_time;

  GST_DEBUG_CATEGORY_INIT (gst_pipewire_clock_debug_category, "pipewireclock", 0,
      "debug category for pipewireclock object");
}

static void
gst_pipewire_clock_init (GstPipeWireClock * clock)
{
  GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
}

void
gst_pipewire_clock_reset (GstPipeWireClock * clock, GstClockTime time)
{
  GstClockTimeDiff time_offset;

  if (clock->last_time >= time)
    time_offset = clock->last_time - time;
  else
    time_offset = -(time - clock->last_time);

  clock->time_offset = time_offset;

  GST_DEBUG_OBJECT (clock,
      "reset clock to %" GST_TIME_FORMAT ", last %" GST_TIME_FORMAT
      ", offset %" GST_STIME_FORMAT, GST_TIME_ARGS (time),
      GST_TIME_ARGS (clock->last_time), GST_STIME_ARGS (time_offset));
}

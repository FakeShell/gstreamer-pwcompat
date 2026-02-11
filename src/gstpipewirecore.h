/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#ifndef __GST_PIPEWIRE_CORE_H__
#define __GST_PIPEWIRE_CORE_H__

#include <gst/gst.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

GST_DEBUG_CATEGORY_EXTERN (pipewire_debug);

typedef struct _GstPipeWireCore GstPipeWireCore;

#define GST_PIPEWIRE_DEFAULT_TIMEOUT 30

/**
 * GstPipeWireCore:
 *
 * Opaque data structure for core functionality.
 */
struct _GstPipeWireCore {
  gint refcount;
  int fd;
  GThread *thread;
  GMainLoop *loop;
  GMainContext *context;
  GMutex lock;
  GCond cond;

  int last_error;
  int last_seq;
  int pending_seq;
  gboolean running;
};

GstPipeWireCore *gst_pipewire_core_get     (int fd);
void                gst_pipewire_core_release (GstPipeWireCore *core);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CORE_H__ */

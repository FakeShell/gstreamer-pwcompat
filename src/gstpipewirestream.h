/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#ifndef __GST_PIPEWIRE_STREAM_H__
#define __GST_PIPEWIRE_STREAM_H__

#include "config.h"

#include "gstpipewirecore.h"

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstPipeWirePool GstPipeWirePool;

#define GST_TYPE_PIPEWIRE_STREAM (gst_pipewire_stream_get_type())
G_DECLARE_FINAL_TYPE(GstPipeWireStream, gst_pipewire_stream, GST, PIPEWIRE_STREAM, GstObject)

typedef enum {
  GST_PIPEWIRE_STREAM_STATE_ERROR = -1,
  GST_PIPEWIRE_STREAM_STATE_UNCONNECTED = 0,
  GST_PIPEWIRE_STREAM_STATE_CONNECTING,
  GST_PIPEWIRE_STREAM_STATE_PAUSED,
  GST_PIPEWIRE_STREAM_STATE_STREAMING
} GstPipeWireStreamState;

typedef void (*GstPipeWireStreamStateChangedCb) (void *data, GstPipeWireStreamState old_state,
                                                    GstPipeWireStreamState state, const char *error);
typedef void (*GstPipeWireStreamParamChangedCb) (void *data, uint32_t id,
                                                    GstCaps *caps);
typedef void (*GstPipeWireStreamProcessCb) (void *data);

typedef struct {
  GstPipeWireStreamStateChangedCb state_changed;
  GstPipeWireStreamParamChangedCb param_changed;
  GstPipeWireStreamProcessCb process;
} GstPipeWireStreamEvents;

struct _GstPipeWireStream {
  GstObject parent;

  GstElement *element;
  GstPipeWireCore *core;
  GstPipeWirePool *pool;
  GstClock *clock;

  GstPipeWireStreamEvents events;
  void *events_data;

  GstPipeWireStreamState state;
  GMutex state_lock;
  char *error_message;

  gboolean active;

  int fd;
  gchar *path;
  gchar *target_object;
  gchar *client_name;
  GstStructure *client_properties;
  GstStructure *stream_properties;
};

GstPipeWireStream * gst_pipewire_stream_new (GstElement *element);

gboolean gst_pipewire_stream_open (GstPipeWireStream *self,
                                      const GstPipeWireStreamEvents *events, void *data);
void gst_pipewire_stream_close (GstPipeWireStream *self);

GstPipeWireStreamState gst_pipewire_stream_get_state (GstPipeWireStream *self,
                                                            const char **error);
void gst_pipewire_stream_set_active (GstPipeWireStream *self, gboolean active);
void gst_pipewire_stream_set_error (GstPipeWireStream *self, int res, const char *error, ...);
gboolean gst_pipewire_stream_connect (GstPipeWireStream *self, uint32_t target_id,
                                         uint32_t flags, GstCaps *caps);
void gst_pipewire_stream_disconnect (GstPipeWireStream *self);
void gst_pipewire_stream_update_params (GstPipeWireStream *self, GstCaps *caps);

G_END_DECLS

#endif /* __GST_PIPEWIRE_STREAM_H__ */

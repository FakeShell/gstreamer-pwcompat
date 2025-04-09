/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2025 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <string.h>
#include <inttypes.h>

#include <gst/gst.h>

#include "gstpipewireformat.h"
#include "gstpipewiredeviceprovider.h"
#include "gstpipewiresrc.h"
#include "gstpipewiresink.h"

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacamera.h>
#include <droidmedia/droidmediaconstants.h>

GST_DEBUG_CATEGORY_EXTERN (pipewire_debug);
#define GST_CAT_DEFAULT pipewire_debug

G_DEFINE_TYPE (GstPipeWireDevice, gst_pipewire_device, GST_TYPE_DEVICE);

enum
{
  PROP_ID = 1,
  PROP_SERIAL,
  PROP_FD_DEVICE,
};

static GstElement *
gst_pipewire_device_create_element (GstDevice *device, const gchar *name)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  GstElement *elem;
  gchar *serial_str;
  GstStructure *props;

  elem = gst_element_factory_make (pipewire_dev->element, name);
  if (!elem) {
    GST_ERROR_OBJECT (device, "Failed to create element %s", pipewire_dev->element);
    return NULL;
  }

  serial_str = g_strdup_printf ("%"PRIu64, pipewire_dev->serial);
  g_object_set (elem, "target-object", serial_str,
                "fd", pipewire_dev->fd, NULL);

  props = gst_device_get_properties (device);
  if (props) {
    if (gst_structure_has_field (props, "orientation")) {
      gint orientation;
      gst_structure_get_int (props, "orientation", &orientation);
      g_object_set (elem, "orientation", orientation, NULL);
      GST_DEBUG_OBJECT (device, "Setting orientation to %d", orientation);
    }

    gst_structure_free (props);
  }

  if (g_strcmp0 (pipewire_dev->element, "pipewiresrc") == 0) {
    g_object_set (elem, "do-timestamp", TRUE, NULL);

    if (g_str_has_prefix (gst_device_get_display_name (device), "Back Camera") ||
        g_str_has_prefix (gst_device_get_display_name (device), "Front Camera")) {
      g_object_set (elem, "always-copy", TRUE, NULL);
    }
  }

  g_free (serial_str);

  return elem;
}

static gboolean
gst_pipewire_device_reconfigure_element (GstDevice *device, GstElement *element)
{
  GstPipeWireDevice *pipewire_dev = GST_PIPEWIRE_DEVICE (device);
  gchar *serial_str;
  GstStructure *props;

  if (g_strcmp0 (pipewire_dev->element, "pipewiresrc") == 0) {
    if (!GST_IS_PIPEWIRE_SRC (element))
      return FALSE;
  } else if (g_strcmp0 (pipewire_dev->element, "pipewiresink") == 0) {
    if (!GST_IS_PIPEWIRE_SINK (element))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  serial_str = g_strdup_printf ("%"PRIu64, pipewire_dev->serial);
  g_object_set (element, "target-object", serial_str,
                "fd", pipewire_dev->fd, NULL);

  props = gst_device_get_properties (device);
  if (props) {
    if (gst_structure_has_field (props, "orientation")) {
      gint orientation;
      gst_structure_get_int (props, "orientation", &orientation);
      g_object_set (element, "orientation", orientation, NULL);
      GST_DEBUG_OBJECT (device, "Setting orientation to %d", orientation);
    }

    gst_structure_free (props);
  }

  if (g_strcmp0 (pipewire_dev->element, "pipewiresrc") == 0)
    g_object_set (element, "do-timestamp", TRUE, NULL);

  g_free (serial_str);

  return TRUE;
}

static void
gst_pipewire_device_get_property (GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
  GstPipeWireDevice *device;

  device = GST_PIPEWIRE_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_ID:
      g_value_set_uint (value, device->id);
      break;
    case PROP_SERIAL:
      g_value_set_uint64 (value, device->serial);
      break;
    case PROP_FD_DEVICE:
      g_value_set_int (value, device->fd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_device_set_property (GObject *object, guint prop_id,
                                     const GValue * value, GParamSpec *pspec)
{
  GstPipeWireDevice *device;

  device = GST_PIPEWIRE_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_ID:
      device->id = g_value_get_uint (value);
      break;
    case PROP_SERIAL:
      device->serial = g_value_get_uint64 (value);
      break;
    case PROP_FD_DEVICE:
      device->fd = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_device_finalize (GObject *object)
{
  G_OBJECT_CLASS (gst_pipewire_device_parent_class)->finalize (object);
}

static void
gst_pipewire_device_class_init (GstPipeWireDeviceClass *klass)
{
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  dev_class->create_element = gst_pipewire_device_create_element;
  dev_class->reconfigure_element = gst_pipewire_device_reconfigure_element;

  object_class->get_property = gst_pipewire_device_get_property;
  object_class->set_property = gst_pipewire_device_set_property;
  object_class->finalize = gst_pipewire_device_finalize;

  g_object_class_install_property (object_class, PROP_ID,
      g_param_spec_uint ("id", "Id",
          "The internal id of the PipeWire device", 0, G_MAXUINT32, 0,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, PROP_SERIAL,
      g_param_spec_uint64 ("serial", "Serial",
          "The internal serial of the PipeWire device", 0, G_MAXUINT64, 0,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
      PROP_FD_DEVICE,
      g_param_spec_int ("fd", "Fd", "The fd to connect with", -1, G_MAXINT, -1,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_pipewire_device_init (GstPipeWireDevice *device G_GNUC_UNUSED)
{
}

G_DEFINE_TYPE (GstPipeWireDeviceProvider, gst_pipewire_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

enum
{
  PROP_0,
  PROP_CLIENT_NAME,
  PROP_FD,
  PROP_LAST
};

static GstPipeWireDevice *
gst_pipewire_device_new (int fd, uint32_t id, uint64_t serial, GstPipeWireDeviceType type,
                            const gchar *element, int priority, const gchar *klass,
                            const gchar *display_name, const GstCaps *caps, const GstStructure *props)
{
  GstPipeWireDevice *gstdev;

  gstdev =
      g_object_new (GST_TYPE_PIPEWIRE_DEVICE, "display-name", display_name,
      "caps", caps, "device-class", klass, "id", id, "serial", serial, "fd", fd,
      "properties", props, NULL);

  gstdev->id = id;
  gstdev->serial = serial;
  gstdev->type = type;
  gstdev->element = element;
  gstdev->priority = priority;

  return gstdev;
}

static void
create_camera_devices (GstPipeWireDeviceProvider *self)
{
  static gboolean droidmedia_initialized = FALSE;

  if (!droidmedia_initialized) {
    droid_media_init ();
    droidmedia_initialized = TRUE;
  }

  int camera_count = droid_media_camera_get_number_of_cameras ();
  self->num_cameras = camera_count;

  if (camera_count <= 0) {
    GST_DEBUG_OBJECT (self, "No DroidMedia cameras found");
    return;
  }

  GST_DEBUG_OBJECT (self, "Found %d DroidMedia cameras", camera_count);

  for (int i = 0; i < camera_count; i++) {
    DroidMediaCameraInfo info;
    droid_media_camera_get_info (&info, i);

    GstCaps *caps = gst_caps_new_empty ();

    GstStructure *raw_struct = gst_structure_new ("video/x-raw",
                                                  "format", G_TYPE_STRING, "I420",
                                                  NULL);

    gst_structure_set (raw_struct,
                       "width", GST_TYPE_INT_RANGE, 320, 1920,
                       "height", GST_TYPE_INT_RANGE, 240, 1920,
                       "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 30, 1,
                       NULL);

    gst_caps_append_structure (caps, raw_struct);

    GstStructure *jpeg_struct = gst_structure_new_empty ("image/jpeg");
    gst_structure_set (jpeg_struct,
                       "width", GST_TYPE_INT_RANGE, 320, 1920,
                       "height", GST_TYPE_INT_RANGE, 240, 1920,
                       "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 30, 1,
                       NULL);

    gst_caps_append_structure (caps, jpeg_struct);

    GstStructure *props = gst_structure_new ("droidmedia-proplist",
                                             "camera-id", G_TYPE_INT, i,
                                             "camera-facing", G_TYPE_INT, info.facing,
                                             "camera-orientation", G_TYPE_INT, info.orientation,
                                             "is-default", G_TYPE_BOOLEAN, (i == 0),
                                             NULL);

    gst_structure_set (props, "orientation", G_TYPE_INT, info.orientation, NULL);

    gchar *name;
    if (info.facing == DROID_MEDIA_CAMERA_FACING_FRONT)
      name = g_strdup_printf ("Front Camera %d", i);
    else
      name = g_strdup_printf ("Back Camera %d", i);

    GST_DEBUG_OBJECT (self, "Creating camera device %s with orientation %d",
                      name, info.orientation);

    GstPipeWireDevice *camera_device = gst_pipewire_device_new (
        self->fd, i, i, GST_PIPEWIRE_DEVICE_TYPE_SOURCE,
        "pipewiresrc", 0, "Video/Source", name, caps, props);

    self->devices = g_list_append (self->devices, GST_DEVICE (camera_device));

    g_free (name);
    gst_caps_unref (caps);
    gst_structure_free (props);
  }
}

static GList *
gst_pipewire_device_provider_probe (GstDeviceProvider *provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "Starting device probe");

  self->devices = NULL;

  create_camera_devices (self);

  GST_DEBUG_OBJECT (self, "Probe found %d devices", g_list_length (self->devices));

  GList *l;
  for (l = self->devices; l != NULL; l = l->next) {
    GstDevice *device = GST_DEVICE (l->data);
    GstCaps *caps = gst_device_get_caps (device);

    GST_DEBUG_OBJECT (self, "Device: %s", gst_device_get_display_name (device));
    GST_DEBUG_OBJECT (self, "  Caps: %" GST_PTR_FORMAT, caps);

    GstStructure *props = gst_device_get_properties (device);
    if (props) {
      GST_DEBUG_OBJECT (self, "  Properties: %" GST_PTR_FORMAT, props);
      gst_structure_free (props);
    }

    gst_caps_unref (caps);
  }

  return self->devices;
}

static gboolean
gst_pipewire_device_provider_start (GstDeviceProvider *provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "starting provider");

  create_camera_devices (self);

  GList *l;
  for (l = self->devices; l != NULL; l = l->next) {
    gst_device_provider_device_add (provider, GST_DEVICE (l->data));
  }

  return TRUE;
}

static void
gst_pipewire_device_provider_stop (GstDeviceProvider *provider)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (provider);

  GST_DEBUG_OBJECT (self, "stopping provider");

  g_list_free_full (self->devices, g_object_unref);
  self->devices = NULL;
}

static void
gst_pipewire_device_provider_set_property (GObject *object, guint prop_id,
                                              const GValue *value, GParamSpec *pspec)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

  switch (prop_id) {
    case PROP_CLIENT_NAME:
      g_free (self->client_name);
      if (!g_value_get_string (value)) {
        GST_WARNING_OBJECT (self,
            "Empty PipeWire client name not allowed. "
            "Resetting to default value");
        self->client_name = g_strdup ("pipewire");
      } else {
        self->client_name = g_value_dup_string (value);
      }
      break;
    case PROP_FD:
      self->fd = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_device_provider_get_property (GObject *object, guint prop_id,
                                              GValue *value, GParamSpec *pspec)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

  switch (prop_id) {
    case PROP_CLIENT_NAME:
      g_value_set_string (value, self->client_name);
      break;
    case PROP_FD:
      g_value_set_int (value, self->fd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_device_provider_finalize (GObject *object)
{
  GstPipeWireDeviceProvider *self = GST_PIPEWIRE_DEVICE_PROVIDER (object);

  g_free (self->client_name);
  g_list_free_full (self->devices, g_object_unref);

  G_OBJECT_CLASS (gst_pipewire_device_provider_parent_class)->finalize (object);
}

static void
gst_pipewire_device_provider_class_init (GstPipeWireDeviceProviderClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);

  gobject_class->set_property = gst_pipewire_device_provider_set_property;
  gobject_class->get_property = gst_pipewire_device_provider_get_property;
  gobject_class->finalize = gst_pipewire_device_provider_finalize;

  dm_class->probe = gst_pipewire_device_provider_probe;
  dm_class->start = gst_pipewire_device_provider_start;
  dm_class->stop = gst_pipewire_device_provider_stop;

  g_object_class_install_property (gobject_class,
      PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The PipeWire client_name_to_use", "pipewire",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_FD,
      g_param_spec_int ("fd", "Fd", "The fd to connect with", -1, G_MAXINT, -1,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  gst_device_provider_class_set_static_metadata (dm_class,
      "Alternative PipeWire Device Provider", "Sink/Source/Audio/Video",
      "List and provide Alternative PipeWire source and sink devices",
      "Your Name <your.email@example.com>");
}

static void
gst_pipewire_device_provider_init (GstPipeWireDeviceProvider *self)
{
  self->client_name = g_strdup ("pipewire");
  self->fd = -1;
  self->devices = NULL;
  self->num_cameras = 0;
}

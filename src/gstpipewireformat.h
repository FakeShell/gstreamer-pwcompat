/*
 * SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans
 *                         Copyright © 2026 Bardia Moshiri
 * SPDX-License-Identifier: MIT
 */

#ifndef _GST_PIPEWIRE_FORMAT_H_
#define _GST_PIPEWIRE_FORMAT_H_

#include <gst/gst.h>
#include <stdint.h>

G_BEGIN_DECLS

GPtrArray *      gst_pipewire_caps_to_format  (GstCaps *caps);
GstCaps *        gst_pipewire_format_to_caps  (uint32_t format_id);

G_END_DECLS

#endif /* _GST_PIPEWIRE_FORMAT_H_ */

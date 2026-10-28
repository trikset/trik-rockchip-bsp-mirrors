/*
 * Copyright 2026 CyberTech Labs Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef __GST_MPP_RGA_ELEMENT_H__
#define __GST_MPP_RGA_ELEMENT_H__

#include <gst/gst.h>

#include "gstmpp.h"

G_BEGIN_DECLS;

#ifdef HAVE_RGA

#define GST_TYPE_MPP_RGA (gst_mpp_rga_get_type ())
G_DECLARE_FINAL_TYPE (GstMppRga, gst_mpp_rga, GST, MPP_RGA, GstBaseTransform)

#endif

gboolean gst_mpp_rga_register (GstPlugin * plugin, guint rank);

G_END_DECLS;

#endif /* __GST_MPP_RGA_ELEMENT_H__ */

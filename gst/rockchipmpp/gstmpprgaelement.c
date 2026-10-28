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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstmpprgaelement.h"

#ifdef HAVE_RGA

#include <string.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>

#include "gstmppallocator.h"

GST_DEBUG_CATEGORY_STATIC (mpp_rga_element_debug);
#define GST_CAT_DEFAULT mpp_rga_element_debug

/* RGA2 also accepts packed 4:2:2. Keep this in sync with the rga column of
 * gst_mpp_formats[], otherwise gst_mpp_rga_buffer() rejects the format. */
#define MPP_RGA_FORMATS GST_RGA_FORMATS ", YUY2, UYVY, YVYU, VYUY"

#define MPP_RGA_CAPS \
    GST_VIDEO_CAPS_MAKE ("{" MPP_RGA_FORMATS "}") "; " \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF, \
        "{" MPP_RGA_FORMATS "}")

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS (MPP_RGA_CAPS));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS (MPP_RGA_CAPS));

struct _GstMppRga
{
  GstBaseTransform parent;

  GstAllocator *allocator;

  GstVideoInfo sink_info;
  GstVideoInfo src_info;

  GstVideoFormat format;
  guint width;
  guint height;
  gint rotation;
};

#define parent_class gst_mpp_rga_parent_class
G_DEFINE_TYPE (GstMppRga, gst_mpp_rga, GST_TYPE_BASE_TRANSFORM);

enum
{
  PROP_0,
  PROP_FORMAT,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_ROTATION,
};

#define DEFAULT_PROP_FORMAT GST_VIDEO_FORMAT_UNKNOWN
#define DEFAULT_PROP_WIDTH 0
#define DEFAULT_PROP_HEIGHT 0
#define DEFAULT_PROP_ROTATION 0

#define GST_TYPE_MPP_RGA_ROTATION (gst_mpp_rga_rotation_get_type ())

static GType
gst_mpp_rga_rotation_get_type (void)
{
  static GType rotation_type = 0;

  if (!rotation_type) {
    static const GEnumValue rotations[] = {
      {0, "Rotate 0", "0"},
      {90, "Rotate 90", "90"},
      {180, "Rotate 180", "180"},
      {270, "Rotate 270", "270"},
      {0, NULL, NULL}
    };
    rotation_type = g_enum_register_static ("GstMppRgaRotation", rotations);
  }

  return rotation_type;
}

static gboolean
gst_mpp_rga_ensure_allocator (GstMppRga * self)
{
  if (self->allocator)
    return TRUE;

  /* This element is the RGA consumer, so it always asks for the DMA32
   * zone: RGA2 is a 32-bit DMA master and cannot access pages above 4G. */
  self->allocator = gst_mpp_allocator_new_with_type ((MppBufferType)
      (MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32));
  if (!self->allocator) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, (NULL),
        ("Failed to create the MPP DMA32 buffer allocator"));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_mpp_rga_format_is_supported (GstVideoFormat format, guint width,
    guint height)
{
  struct gst_mpp_format *fmt;

  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  if (gst_mpp_gst_format_to_rga_format (format) == RK_FORMAT_UNKNOWN)
    return FALSE;

  fmt = GST_MPP_GET_FORMAT (gst, format);
  if (fmt && fmt->is_yuv && ((width & 1) || (height & 1)))
    return FALSE;

  return TRUE;
}

static void
gst_mpp_rga_fixate_dimensions (GstMppRga * self, GstStructure * s)
{
  const GValue *in_width = gst_structure_get_value (s, "width");
  const GValue *in_height = gst_structure_get_value (s, "height");
  gboolean rotate = (self->rotation == 90 || self->rotation == 270);
  GValue width = G_VALUE_INIT;
  GValue height = G_VALUE_INIT;

  if (in_width) {
    g_value_init (&width, G_VALUE_TYPE (in_width));
    g_value_copy (in_width, &width);
  }
  if (in_height) {
    g_value_init (&height, G_VALUE_TYPE (in_height));
    g_value_copy (in_height, &height);
  }

  /* Fixed properties always win. For rotation without an explicit value the
   * dimensions are swapped, and we must handle both fixed and ranged caps. */
  if (self->width > 0)
    gst_structure_set (s, "width", G_TYPE_INT, (gint) self->width, NULL);
  else if (rotate && in_height)
    gst_structure_set_value (s, "width", &height);

  if (self->height > 0)
    gst_structure_set (s, "height", G_TYPE_INT, (gint) self->height, NULL);
  else if (rotate && in_width)
    gst_structure_set_value (s, "height", &width);

  if (G_IS_VALUE (&width))
    g_value_unset (&width);
  if (G_IS_VALUE (&height))
    g_value_unset (&height);
}

static GstCaps *
gst_mpp_rga_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstMppRga *self = GST_MPP_RGA (trans);
  GstCaps *result;
  guint i;

  if (direction == GST_PAD_SRC) {
    result = gst_static_pad_template_get_caps (&sink_template);
    if (filter) {
      GstCaps *tmp = gst_caps_intersect_full (result, filter,
          GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (result);
      result = tmp;
    }
    return result;
  }

  result = gst_caps_new_empty ();

  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_structure_copy (gst_caps_get_structure (caps, i));
    GstCapsFeatures *features = gst_caps_get_features (caps, i);
    GstCaps *one;

    if (self->format != GST_VIDEO_FORMAT_UNKNOWN) {
      const gchar *format = gst_video_format_to_string (self->format);

      if (format)
        gst_structure_set (s, "format", G_TYPE_STRING, format, NULL);
    }

    gst_mpp_rga_fixate_dimensions (self, s);

    one = gst_caps_new_full (s, NULL);
    if (features)
      gst_caps_set_features (one, 0, gst_caps_features_copy (features));
    gst_caps_append (result, one);
  }

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (result, filter,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (result);
    result = tmp;
  }

  return result;
}

static gboolean
gst_mpp_rga_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstMppRga *self = GST_MPP_RGA (trans);
  GstVideoInfo sink_info, src_info;
  GstVideoFormat sink_format, src_format;
  gboolean passthrough;

  if (!gst_video_info_from_caps (&sink_info, incaps) ||
      !gst_video_info_from_caps (&src_info, outcaps)) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("Invalid caps"));
    return FALSE;
  }

  sink_format = GST_VIDEO_INFO_FORMAT (&sink_info);
  src_format = GST_VIDEO_INFO_FORMAT (&src_info);

  if (!gst_mpp_rga_format_is_supported (sink_format,
          GST_VIDEO_INFO_WIDTH (&sink_info),
          GST_VIDEO_INFO_HEIGHT (&sink_info)) ||
      !gst_mpp_rga_format_is_supported (src_format,
          GST_VIDEO_INFO_WIDTH (&src_info),
          GST_VIDEO_INFO_HEIGHT (&src_info))) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("Unsupported RGA conversion %s %ux%u -> %s %ux%u",
            gst_video_format_to_string (sink_format),
            GST_VIDEO_INFO_WIDTH (&sink_info),
            GST_VIDEO_INFO_HEIGHT (&sink_info),
            gst_video_format_to_string (src_format),
            GST_VIDEO_INFO_WIDTH (&src_info),
            GST_VIDEO_INFO_HEIGHT (&src_info)));
    return FALSE;
  }

  if (GST_VIDEO_INFO_INTERLACE_MODE (&sink_info) !=
      GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
      GST_VIDEO_INFO_INTERLACE_MODE (&src_info) !=
      GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("Only progressive video is supported"));
    return FALSE;
  }

  self->sink_info = sink_info;
  self->src_info = src_info;

  passthrough = self->rotation == 0 &&
      GST_VIDEO_INFO_FORMAT (&self->sink_info) ==
      GST_VIDEO_INFO_FORMAT (&self->src_info) &&
      GST_VIDEO_INFO_WIDTH (&self->sink_info) ==
      GST_VIDEO_INFO_WIDTH (&self->src_info) &&
      GST_VIDEO_INFO_HEIGHT (&self->sink_info) ==
      GST_VIDEO_INFO_HEIGHT (&self->src_info);

  gst_base_transform_set_passthrough (trans, passthrough);

  GST_INFO_OBJECT (self, "%s %ux%u -> %s %ux%u (rotation %d)%s",
      gst_video_format_to_string (sink_format),
      GST_VIDEO_INFO_WIDTH (&sink_info),
      GST_VIDEO_INFO_HEIGHT (&sink_info),
      gst_video_format_to_string (src_format),
      GST_VIDEO_INFO_WIDTH (&src_info),
      GST_VIDEO_INFO_HEIGHT (&src_info), self->rotation,
      passthrough ? " passthrough" : "");

  return TRUE;
}

static gboolean
gst_mpp_rga_propose_allocation (GstBaseTransform * trans, GstQuery * decide_query,
    GstQuery * query)
{
  GstMppRga *self = GST_MPP_RGA (trans);
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool;
  GstStructure *config;
  GstVideoAlignment align;
  gsize plane_size[GST_VIDEO_MAX_PLANES] = { 0, };
  const GstStructure *params = NULL;
  guint size, min_buffers = 2;
  guint idx;
  gboolean need_pool, aligned = FALSE;

  (void) decide_query;

  gst_query_parse_allocation (query, &caps, &need_pool);
  if (!caps)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  if (!gst_mpp_rga_ensure_allocator (self))
    return FALSE;

  gst_video_alignment_reset (&align);

  /* Honor the alignment advertised by the producer/driver in the allocation
   * query (padding-* in the video-meta allocation meta). */
  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, &idx))
    gst_query_parse_nth_allocation_meta (query, idx, &params);

  if (params) {
    gst_structure_get_uint (params, "padding-top", &align.padding_top);
    gst_structure_get_uint (params, "padding-bottom", &align.padding_bottom);
    gst_structure_get_uint (params, "padding-left", &align.padding_left);
    gst_structure_get_uint (params, "padding-right", &align.padding_right);
  }

  if (align.padding_top || align.padding_bottom ||
      align.padding_left || align.padding_right) {
    if (!gst_video_info_align_full (&info, &align, plane_size)) {
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, (NULL),
          ("Failed to apply the requested video alignment"));
      return FALSE;
    }
    aligned = TRUE;
  }

  if (need_pool) {
    size = GST_VIDEO_INFO_SIZE (&info);

    pool = gst_video_buffer_pool_new ();
    if (!pool)
      return FALSE;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, min_buffers, 0);
    gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (aligned) {
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (config, &align);
    }

    if (!gst_buffer_pool_set_config (pool, config)) {
      config = gst_buffer_pool_get_config (pool);
      if (!gst_buffer_pool_set_config (pool, config)) {
        gst_object_unref (pool);
        return FALSE;
      }
    }

    gst_query_add_allocation_pool (query, pool, size, min_buffers, 0);
    gst_object_unref (pool);
  }

  gst_query_add_allocation_param (query, self->allocator, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  GST_DEBUG_OBJECT (self, "proposed MPP DMA32 allocator%s%s",
      need_pool ? " with pool" : "", aligned ? " (aligned)" : "");

  return TRUE;
}

static GstFlowReturn
gst_mpp_rga_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstMppRga *self = GST_MPP_RGA (trans);
  GstBuffer *buffer;
  GstMemory *mem;
  GstVideoMeta *vmeta;

  if (!self->allocator)
    return GST_FLOW_ERROR;

  buffer = gst_buffer_new ();
  if (!buffer)
    return GST_FLOW_ERROR;

  mem = gst_allocator_alloc (self->allocator,
      GST_VIDEO_INFO_SIZE (&self->src_info), NULL);
  if (!mem) {
    gst_buffer_unref (buffer);
    return GST_FLOW_ERROR;
  }

  gst_buffer_append_memory (buffer, mem);

  /* Copy flags, timestamps and all metadata (crop/region/overlay...). The
   * input video meta describes the input layout and is replaced by the
   * transform with the output one. */
  gst_buffer_copy_into (buffer, inbuf,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS |
      GST_BUFFER_COPY_META, 0, 0);

  while ((vmeta = gst_buffer_get_video_meta (buffer)))
    gst_buffer_remove_meta (buffer, (GstMeta *) vmeta);

  *outbuf = buffer;
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_mpp_rga_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMppRga *self = GST_MPP_RGA (trans);
  GstVideoInfo in_info = self->sink_info;
  GstVideoMeta *meta = gst_buffer_get_video_meta (inbuf);
  GstMemory *out_mem;
  guint i;

  if (meta) {
    for (i = 0; i < meta->n_planes; i++) {
      GST_VIDEO_INFO_PLANE_STRIDE (&in_info, i) = meta->stride[i];
      GST_VIDEO_INFO_PLANE_OFFSET (&in_info, i) = meta->offset[i];
    }
  }

  if (gst_buffer_get_size (inbuf) < GST_VIDEO_INFO_SIZE (&in_info)) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("Input buffer too small (%" G_GSIZE_FORMAT " < %" G_GSIZE_FORMAT ")",
            gst_buffer_get_size (inbuf), GST_VIDEO_INFO_SIZE (&in_info)));
    return GST_FLOW_ERROR;
  }

  out_mem = gst_buffer_peek_memory (outbuf, 0);
  if (!out_mem) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("Output buffer has no memory"));
    return GST_FLOW_ERROR;
  }

  if (!gst_mpp_rga_convert (inbuf, &in_info, out_mem, &self->src_info,
          self->rotation)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("RGA conversion failed, check the buffer format and memory zone"));
    return GST_FLOW_ERROR;
  }

  if (!gst_buffer_add_video_meta_full (outbuf, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&self->src_info),
          GST_VIDEO_INFO_WIDTH (&self->src_info),
          GST_VIDEO_INFO_HEIGHT (&self->src_info),
          GST_VIDEO_INFO_N_PLANES (&self->src_info),
          self->src_info.offset, self->src_info.stride)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, (NULL),
        ("Failed to attach the output video meta"));
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static void
gst_mpp_rga_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMppRga *self = GST_MPP_RGA (object);

  switch (prop_id) {
    case PROP_FORMAT:
      self->format = g_value_get_enum (value);
      break;
    case PROP_WIDTH:
      self->width = g_value_get_uint (value);
      break;
    case PROP_HEIGHT:
      self->height = g_value_get_uint (value);
      break;
    case PROP_ROTATION:
      self->rotation = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }

  if (GST_STATE (self) >= GST_STATE_PAUSED)
    gst_base_transform_reconfigure_src (GST_BASE_TRANSFORM (self));
}

static void
gst_mpp_rga_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstMppRga *self = GST_MPP_RGA (object);

  switch (prop_id) {
    case PROP_FORMAT:
      g_value_set_enum (value, self->format);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    case PROP_ROTATION:
      g_value_set_enum (value, self->rotation);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_mpp_rga_start (GstBaseTransform * trans)
{
  GstMppRga *self = GST_MPP_RGA (trans);

  return gst_mpp_rga_ensure_allocator (self);
}

static gboolean
gst_mpp_rga_stop (GstBaseTransform * trans)
{
  GstMppRga *self = GST_MPP_RGA (trans);

  g_clear_object (&self->allocator);

  return TRUE;
}

static void
gst_mpp_rga_finalize (GObject * object)
{
  GstMppRga *self = GST_MPP_RGA (object);

  g_clear_object (&self->allocator);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_mpp_rga_class_init (GstMppRgaClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "mpprgaelement", 0,
      "MPP RGA converter element");

  gobject_class->set_property = gst_mpp_rga_set_property;
  gobject_class->get_property = gst_mpp_rga_get_property;
  gobject_class->finalize = gst_mpp_rga_finalize;

  g_object_class_install_property (gobject_class, PROP_FORMAT,
      g_param_spec_enum ("format", "Format",
          "Output format (UNKNOWN keeps the input format)",
          GST_TYPE_VIDEO_FORMAT, DEFAULT_PROP_FORMAT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_uint ("width", "Width", "Output width (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_WIDTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_uint ("height", "Height", "Output height (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_HEIGHT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ROTATION,
      g_param_spec_enum ("rotation", "Rotation", "Rotation",
          GST_TYPE_MPP_RGA_ROTATION, DEFAULT_PROP_ROTATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "Rockchip RGA converter/scaler", "Filter/Converter/Video",
      "Scales, converts, rotates and flips video frames using Rockchip RGA",
      "CyberTech Labs Ltd.");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  trans_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_mpp_rga_transform_caps);
  trans_class->set_caps = GST_DEBUG_FUNCPTR (gst_mpp_rga_set_caps);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_mpp_rga_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_rga_stop);
  trans_class->transform = GST_DEBUG_FUNCPTR (gst_mpp_rga_transform);
  trans_class->prepare_output_buffer =
      GST_DEBUG_FUNCPTR (gst_mpp_rga_prepare_output_buffer);
  trans_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_mpp_rga_propose_allocation);
}

static void
gst_mpp_rga_init (GstMppRga * self)
{
  self->allocator = NULL;
  self->format = DEFAULT_PROP_FORMAT;
  self->width = DEFAULT_PROP_WIDTH;
  self->height = DEFAULT_PROP_HEIGHT;
  self->rotation = DEFAULT_PROP_ROTATION;

  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), FALSE);
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (self), TRUE);
}

gboolean
gst_mpp_rga_register (GstPlugin * plugin, guint rank)
{
  return gst_element_register (plugin, "mpprga", rank, GST_TYPE_MPP_RGA);
}

#else /* !HAVE_RGA */

gboolean
gst_mpp_rga_register (GstPlugin * plugin, guint rank)
{
  (void) plugin;
  (void) rank;
  return FALSE;
}

#endif /* HAVE_RGA */

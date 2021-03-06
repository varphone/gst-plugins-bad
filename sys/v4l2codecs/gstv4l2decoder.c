/* GStreamer
 * Copyright (C) 2020 Nicolas Dufresne <nicolas.dufresne@collabora.com>
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstv4l2codecallocator.h"
#include "gstv4l2codecpool.h"
#include "gstv4l2decoder.h"
#include "gstv4l2format.h"
#include "linux/media.h"
#include "linux/videodev2.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

GST_DEBUG_CATEGORY (v4l2_decoder_debug);
#define GST_CAT_DEFAULT v4l2_decoder_debug

enum
{
  PROP_0,
  PROP_MEDIA_DEVICE,
  PROP_VIDEO_DEVICE,
};

struct _GstV4l2Request
{
  GstV4l2Decoder *decoder;
  gint fd;
  GstMemory *bitstream;
  GstPoll *poll;
  GstPollFD pollfd;
  gboolean pending;
};

struct _GstV4l2Decoder
{
  GstObject parent;

  gboolean opened;
  gint media_fd;
  gint video_fd;
  GstAtomicQueue *request_pool;

  /* properties */
  gchar *media_device;
  gchar *video_device;
};

G_DEFINE_TYPE_WITH_CODE (GstV4l2Decoder, gst_v4l2_decoder, GST_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (v4l2_decoder_debug, "v4l2codecs-decoder", 0,
        "V4L2 stateless decoder helper"));

static guint32
direction_to_buffer_type (GstPadDirection direction)
{
  if (direction == GST_PAD_SRC)
    return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  else
    return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static void
gst_v4l2_decoder_finalize (GObject * obj)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (obj);

  gst_v4l2_decoder_close (self);

  g_free (self->media_device);
  g_free (self->video_device);
  gst_atomic_queue_unref (self->request_pool);

  G_OBJECT_CLASS (gst_v4l2_decoder_parent_class)->finalize (obj);
}

static void
gst_v4l2_decoder_init (GstV4l2Decoder * self)
{
  self->request_pool = gst_atomic_queue_new (16);
}

static void
gst_v4l2_decoder_class_init (GstV4l2DecoderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_v4l2_decoder_finalize;
  gobject_class->get_property = gst_v4l2_decoder_get_property;
  gobject_class->set_property = gst_v4l2_decoder_set_property;

  gst_v4l2_decoder_install_properties (gobject_class, 0, NULL);
}

GstV4l2Decoder *
gst_v4l2_decoder_new (GstV4l2CodecDevice * device)
{
  GstV4l2Decoder *decoder;

  g_return_val_if_fail (device->function == MEDIA_ENT_F_PROC_VIDEO_DECODER,
      NULL);

  decoder = g_object_new (GST_TYPE_V4L2_DECODER,
      "media-device", device->media_device_path,
      "video-device", device->video_device_path, NULL);

  return gst_object_ref_sink (decoder);
}

gboolean
gst_v4l2_decoder_open (GstV4l2Decoder * self)
{
  self->media_fd = open (self->media_device, 0);
  if (self->media_fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open '%s': %s",
        self->media_device, g_strerror (errno));
    return FALSE;
  }

  self->video_fd = open (self->video_device, O_NONBLOCK);
  if (self->video_fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open '%s': %s",
        self->video_device, g_strerror (errno));
    return FALSE;
  }

  self->opened = TRUE;

  return TRUE;
}

gboolean
gst_v4l2_decoder_close (GstV4l2Decoder * self)
{
  GstV4l2Request *request;

  while ((request = gst_atomic_queue_pop (self->request_pool)))
    gst_v4l2_request_free (request);

  if (self->media_fd)
    close (self->media_fd);
  if (self->video_fd)
    close (self->media_fd);

  self->media_fd = 0;
  self->video_fd = 0;
  self->opened = FALSE;

  return TRUE;
}

gboolean
gst_v4l2_decoder_streamon (GstV4l2Decoder * self, GstPadDirection direction)
{
  gint ret;
  guint32 type = direction_to_buffer_type (direction);

  ret = ioctl (self->video_fd, VIDIOC_STREAMON, &type);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_STREAMON failed: %s", g_strerror (errno));
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_streamoff (GstV4l2Decoder * self, GstPadDirection direction)
{
  gint ret;
  guint32 type = direction_to_buffer_type (direction);

  ret = ioctl (self->video_fd, VIDIOC_STREAMOFF, &type);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_STREAMOFF failed: %s", g_strerror (errno));
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_flush (GstV4l2Decoder * self)
{
  /* We ignore streamoff failure as it's not relevant, if we manage to
   * streamon again, we are good. */
  gst_v4l2_decoder_streamoff (self, GST_PAD_SINK);
  gst_v4l2_decoder_streamoff (self, GST_PAD_SRC);

  return gst_v4l2_decoder_streamon (self, GST_PAD_SINK) &&
      gst_v4l2_decoder_streamon (self, GST_PAD_SRC);
}

gboolean
gst_v4l2_decoder_enum_sink_fmt (GstV4l2Decoder * self, gint i,
    guint32 * out_fmt)
{
  struct v4l2_fmtdesc fmtdesc = { i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, };
  gint ret;

  g_return_val_if_fail (self->opened, FALSE);

  ret = ioctl (self->video_fd, VIDIOC_ENUM_FMT, &fmtdesc);
  if (ret < 0) {
    if (errno != EINVAL)
      GST_ERROR_OBJECT (self, "VIDIOC_ENUM_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Found format %" GST_FOURCC_FORMAT " (%s)",
      GST_FOURCC_ARGS (fmtdesc.pixelformat), fmtdesc.description);
  *out_fmt = fmtdesc.pixelformat;

  return TRUE;
}

gboolean
gst_v4l2_decoder_set_sink_fmt (GstV4l2Decoder * self, guint32 pix_fmt,
    gint width, gint height)
{
  struct v4l2_format format = (struct v4l2_format) {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .fmt.pix_mp = (struct v4l2_pix_format_mplane) {
          .pixelformat = pix_fmt,
          .width = width,
          .height = height,
        },
  };
  gint ret;

  ret = ioctl (self->video_fd, VIDIOC_S_FMT, &format);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  if (format.fmt.pix_mp.pixelformat != pix_fmt
      || format.fmt.pix_mp.width != width
      || format.fmt.pix_mp.height != height) {
    GST_WARNING_OBJECT (self, "Failed to set sink format to %"
        GST_FOURCC_FORMAT " %ix%i", GST_FOURCC_ARGS (pix_fmt), width, height);
    errno = EINVAL;
    return FALSE;
  }

  return TRUE;
}

GstCaps *
gst_v4l2_decoder_enum_src_formats (GstV4l2Decoder * self)
{
  gint ret;
  struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
  };
  GstVideoFormat format;
  GstCaps *caps;
  GValue list = G_VALUE_INIT;
  GValue value = G_VALUE_INIT;
  gint i;

  g_return_val_if_fail (self->opened, FALSE);

  ret = ioctl (self->video_fd, VIDIOC_G_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_G_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  /* We first place a structure with the default pixel format */
  if (gst_v4l2_format_to_video_format (fmt.fmt.pix_mp.pixelformat, &format))
    caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING,
        gst_video_format_to_string (format), NULL);
  else
    caps = gst_caps_new_empty ();

  /* And then enumerate other possible formats and place that as a second
   * structure in the caps */
  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&value, G_TYPE_STRING);

  for (i = 0; ret >= 0; i++) {
    struct v4l2_fmtdesc fmtdesc = { i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, };

    ret = ioctl (self->video_fd, VIDIOC_ENUM_FMT, &fmtdesc);
    if (ret < 0) {
      if (errno != EINVAL)
        GST_ERROR_OBJECT (self, "VIDIOC_ENUM_FMT failed: %s",
            g_strerror (errno));
      continue;
    }

    if (gst_v4l2_format_to_video_format (fmtdesc.pixelformat, &format)) {
      g_value_set_static_string (&value, gst_video_format_to_string (format));
      gst_value_list_append_value (&list, &value);
    }
  }
  g_value_reset (&value);

  if (gst_value_list_get_size (&list) > 0) {
    GstStructure *str = gst_structure_new_empty ("video/x-raw");
    gst_structure_take_value (str, "format", &list);
    gst_caps_append_structure (caps, str);
  } else {
    g_value_reset (&list);
  }

  return caps;
}

gboolean
gst_v4l2_decoder_select_src_format (GstV4l2Decoder * self, GstCaps * caps,
    GstVideoInfo * info)
{
  gint ret;
  struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
  };
  GstStructure *str;
  const gchar *format_str;
  GstVideoFormat format;
  guint32 pix_fmt;

  if (gst_caps_is_empty (caps))
    return FALSE;

  ret = ioctl (self->video_fd, VIDIOC_G_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_G_FMT failed: %s", g_strerror (errno));
    return FALSE;
  }

  caps = gst_caps_make_writable (caps);
  str = gst_caps_get_structure (caps, 0);
  gst_structure_fixate_field (str, "format");

  format_str = gst_structure_get_string (str, "format");
  format = gst_video_format_from_string (format_str);

  if (gst_v4l2_format_from_video_format (format, &pix_fmt) &&
      pix_fmt != fmt.fmt.pix_mp.pixelformat) {
    GST_DEBUG_OBJECT (self, "Trying to use peer format: %s ", format_str);
    fmt.fmt.pix_mp.pixelformat = pix_fmt;

    ret = ioctl (self->video_fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
      GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed: %s", g_strerror (errno));
      return FALSE;
    }
  }

  if (!gst_v4l2_format_to_video_info (&fmt, info)) {
    GST_ERROR_OBJECT (self, "Unsupported V4L2 pixelformat %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (fmt.fmt.pix_mp.pixelformat));
    return FALSE;
  }

  GST_INFO_OBJECT (self, "Selected format %s %ix%i",
      gst_video_format_to_string (info->finfo->format),
      info->width, info->height);

  return TRUE;
}

gint
gst_v4l2_decoder_request_buffers (GstV4l2Decoder * self,
    GstPadDirection direction, guint num_buffers)
{
  gint ret;
  struct v4l2_requestbuffers reqbufs = {
    .count = num_buffers,
    .memory = V4L2_MEMORY_MMAP,
    .type = direction_to_buffer_type (direction),
  };

  GST_DEBUG_OBJECT (self, "Requesting %u buffers", num_buffers);

  ret = ioctl (self->video_fd, VIDIOC_REQBUFS, &reqbufs);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_REQBUFS failed: %s", g_strerror (errno));
    return ret;
  }

  return reqbufs.count;
}

gboolean
gst_v4l2_decoder_export_buffer (GstV4l2Decoder * self,
    GstPadDirection direction, gint index, gint * fds, gsize * sizes,
    gsize * offsets, guint * num_fds)
{
  gint i, ret;
  struct v4l2_plane planes[GST_VIDEO_MAX_PLANES] = { {0} };
  struct v4l2_buffer v4l2_buf = {
    .index = 0,
    .type = direction_to_buffer_type (direction),
    .length = GST_VIDEO_MAX_PLANES,
    .m.planes = planes,
  };

  ret = ioctl (self->video_fd, VIDIOC_QUERYBUF, &v4l2_buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_QUERYBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  *num_fds = v4l2_buf.length;
  for (i = 0; i < v4l2_buf.length; i++) {
    struct v4l2_plane *plane = v4l2_buf.m.planes + i;
    struct v4l2_exportbuffer expbuf = {
      .type = direction_to_buffer_type (direction),
      .index = index,
      .plane = i,
      .flags = O_CLOEXEC | O_RDWR,
    };

    ret = ioctl (self->video_fd, VIDIOC_EXPBUF, &expbuf);
    if (ret < 0) {
      gint j;
      GST_ERROR_OBJECT (self, "VIDIOC_EXPBUF failed: %s", g_strerror (errno));

      for (j = i - 1; j >= 0; j--)
        close (fds[j]);

      return FALSE;
    }

    fds[i] = expbuf.fd;
    sizes[i] = plane->length;
    offsets[i] = plane->data_offset;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_queue_sink_mem (GstV4l2Decoder * self,
    GstV4l2Request * request, GstMemory * mem, guint32 frame_num,
    gsize bytesused)
{
  gint ret;
  struct v4l2_plane plane = {
    .bytesused = bytesused,
  };
  struct v4l2_buffer buf = {
    .type = direction_to_buffer_type (GST_PAD_SINK),
    .memory = V4L2_MEMORY_MMAP,
    .index = gst_v4l2_codec_memory_get_index (mem),
    .timestamp.tv_usec = frame_num,
    .request_fd = request->fd,
    .flags = V4L2_BUF_FLAG_REQUEST_FD,
    .length = 1,
    .m.planes = &plane,
  };

  ret = ioctl (self->video_fd, VIDIOC_QBUF, &buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_QBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  request->bitstream = gst_memory_ref (mem);

  return TRUE;
}

gboolean
gst_v4l2_decoder_queue_src_buffer (GstV4l2Decoder * self, GstBuffer * buffer,
    guint32 frame_num)
{
  gint i, ret;
  struct v4l2_plane plane[GST_VIDEO_MAX_PLANES];
  struct v4l2_buffer buf = {
    .type = direction_to_buffer_type (GST_PAD_SRC),
    .memory = V4L2_MEMORY_MMAP,
    .index = gst_v4l2_codec_buffer_get_index (buffer),
    .length = gst_buffer_n_memory (buffer),
    .m.planes = plane,
  };

  for (i = 0; i < buf.length; i++) {
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    /* *INDENT-OFF* */
    plane[i] = (struct v4l2_plane) {
      .bytesused = gst_memory_get_sizes (mem, NULL, NULL),
    };
    /* *INDENT-ON* */
  }

  ret = ioctl (self->video_fd, VIDIOC_QBUF, &buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_QBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_dequeue_sink (GstV4l2Decoder * self)
{
  gint ret;
  struct v4l2_plane plane[GST_VIDEO_MAX_PLANES] = { {0} };
  struct v4l2_buffer buf = {
    .type = direction_to_buffer_type (GST_PAD_SINK),
    .memory = V4L2_MEMORY_MMAP,
    .length = GST_VIDEO_MAX_PLANES,
    .m.planes = plane,
  };

  ret = ioctl (self->video_fd, VIDIOC_DQBUF, &buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_DQBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_v4l2_decoder_dequeue_src (GstV4l2Decoder * self, guint32 * out_frame_num)
{
  gint ret;
  struct v4l2_plane plane[GST_VIDEO_MAX_PLANES] = { {0} };
  struct v4l2_buffer buf = {
    .type = direction_to_buffer_type (GST_PAD_SRC),
    .memory = V4L2_MEMORY_MMAP,
    .length = GST_VIDEO_MAX_PLANES,
    .m.planes = plane,
  };

  ret = ioctl (self->video_fd, VIDIOC_DQBUF, &buf);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_DQBUF failed: %s", g_strerror (errno));
    return FALSE;
  }

  *out_frame_num = buf.timestamp.tv_usec;
  return TRUE;
}

gboolean
gst_v4l2_decoder_set_controls (GstV4l2Decoder * self, GstV4l2Request * request,
    struct v4l2_ext_control * control, guint count)
{
  gint ret;
  struct v4l2_ext_controls controls = {
    .controls = control,
    .count = count,
    .request_fd = request ? request->fd : 0,
    .which = request ? V4L2_CTRL_WHICH_REQUEST_VAL : 0,
  };

  ret = ioctl (self->video_fd, VIDIOC_S_EXT_CTRLS, &controls);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_EXT_CTRLS failed: %s",
        g_strerror (errno));
    return FALSE;
  }

  return TRUE;
}

void
gst_v4l2_decoder_install_properties (GObjectClass * gobject_class,
    gint prop_offset, GstV4l2CodecDevice * device)
{
  const gchar *media_device_path = NULL;
  const gchar *video_device_path = NULL;

  if (device) {
    media_device_path = device->media_device_path;
    video_device_path = device->video_device_path;
  }

  g_object_class_install_property (gobject_class, PROP_MEDIA_DEVICE,
      g_param_spec_string ("media-device", "Media Device Path",
          "Path to the media device node", media_device_path,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_VIDEO_DEVICE,
      g_param_spec_string ("video-device", "Video Device Path",
          "Path to the video device node", video_device_path,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

void
gst_v4l2_decoder_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (object);

  switch (prop_id) {
    case PROP_MEDIA_DEVICE:
      g_free (self->media_device);
      self->media_device = g_value_dup_string (value);
      break;
    case PROP_VIDEO_DEVICE:
      g_free (self->video_device);
      self->video_device = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

void
gst_v4l2_decoder_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstV4l2Decoder *self = GST_V4L2_DECODER (object);

  switch (prop_id) {
    case PROP_MEDIA_DEVICE:
      g_value_set_string (value, self->media_device);
      break;
    case PROP_VIDEO_DEVICE:
      g_value_set_string (value, self->video_device);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

GstV4l2Request *
gst_v4l2_decoder_alloc_request (GstV4l2Decoder * self)
{
  GstV4l2Request *request = gst_atomic_queue_pop (self->request_pool);
  gint ret;

  if (!request) {
    request = g_new0 (GstV4l2Request, 1);

    ret = ioctl (self->media_fd, MEDIA_IOC_REQUEST_ALLOC, &request->fd);
    if (ret < 0) {
      GST_ERROR_OBJECT (self, "MEDIA_IOC_REQUEST_ALLOC failed: %s",
          g_strerror (errno));
      return NULL;
    }

    request->poll = gst_poll_new (FALSE);
    gst_poll_fd_init (&request->pollfd);
    request->pollfd.fd = request->fd;
    gst_poll_add_fd (request->poll, &request->pollfd);
    gst_poll_fd_ctl_pri (request->poll, &request->pollfd, TRUE);
  }

  request->decoder = g_object_ref (self);
  return request;
}

void
gst_v4l2_request_free (GstV4l2Request * request)
{
  GstV4l2Decoder *decoder = request->decoder;
  gint ret;

  if (!decoder) {
    close (request->fd);
    gst_poll_free (request->poll);
    g_free (request);
    return;
  }

  g_clear_pointer (&request->bitstream, gst_memory_unref);
  request->decoder = NULL;

  if (request->pending) {
    GST_DEBUG_OBJECT (decoder, "Freeing pending request %p.", request);
    gst_v4l2_request_free (request);
    g_object_unref (decoder);
    return;
  }

  GST_DEBUG_OBJECT (decoder, "Recycling request %p.", request);

  ret = ioctl (request->fd, MEDIA_REQUEST_IOC_REINIT, NULL);
  if (ret < 0) {
    GST_ERROR_OBJECT (request->decoder, "MEDIA_REQUEST_IOC_REINIT failed: %s",
        g_strerror (errno));
    gst_v4l2_request_free (request);
    g_object_unref (decoder);
    return;
  }

  gst_atomic_queue_push (decoder->request_pool, request);
  g_object_unref (decoder);
}

gboolean
gst_v4l2_request_queue (GstV4l2Request * request)
{
  gint ret;

  GST_DEBUG_OBJECT (request->decoder, "Queuing request %p.", request);

  ret = ioctl (request->fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
  if (ret < 0) {
    GST_ERROR_OBJECT (request->decoder, "MEDIA_REQUEST_IOC_QUEUE, failed: %s",
        g_strerror (errno));
    return FALSE;
  }

  request->pending = TRUE;

  return TRUE;
}

gint
gst_v4l2_request_poll (GstV4l2Request * request, GstClockTime timeout)
{
  return gst_poll_wait (request->poll, timeout);
}

void
gst_v4l2_request_set_done (GstV4l2Request * request)
{
  if (request->bitstream) {
    gst_v4l2_decoder_dequeue_sink (request->decoder);
    g_clear_pointer (&request->bitstream, gst_memory_unref);
  }

  request->pending = FALSE;
}

gboolean
gst_v4l2_request_is_done (GstV4l2Request * request)
{
  return !request->pending;
}

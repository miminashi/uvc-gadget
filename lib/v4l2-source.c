/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * V4L2 video source
 *
 * Copyright (C) 2018 Laurent Pinchart
 *
 * Contact: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/videodev2.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "events.h"
#include "tools.h"
#include "v4l2.h"
#include "v4l2-source.h"
#include "video-buffers.h"

struct v4l2_source {
	struct video_source src;

	struct v4l2_device *vdev;

	struct buffer {
		void *start;
		size_t length;
	} *mmap_buffers;
	unsigned int n_mmap_buffers;
	bool use_mmap;
};

#define to_v4l2_source(s) container_of(s, struct v4l2_source, src)

static void v4l2_source_video_process(void *d)
{
    struct v4l2_source *src = d;
    struct video_buffer buf;
    int ret;

    printf("DEBUG: v4l2_source_video_process called, use_mmap=%s\n",
           src->use_mmap ? "true" : "false");

    if (src->use_mmap) {
        /* MMAP用の処理 */
        struct v4l2_buffer v4l2_buf;

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;

        ret = ioctl(src->vdev->fd, VIDIOC_DQBUF, &v4l2_buf);
        if (ret < 0) {
            if (errno != EAGAIN)
                perror("VIDIOC_DQBUF");
            return;
        }

        printf("DEBUG: Dequeued buffer %d, bytesused=%u\n",
               v4l2_buf.index, v4l2_buf.bytesused);

        /* video_bufferに変換 */
        buf.index = v4l2_buf.index;
        buf.size = v4l2_buf.bytesused;
        buf.bytesused = v4l2_buf.bytesused;
        buf.dmabuf = -1;
        buf.mem = src->mmap_buffers[v4l2_buf.index].start;

        printf("DEBUG: Calling handler with buffer size=%u\n", buf.size);
        src->src.handler(src->src.handler_data, &src->src, &buf);

        /* バッファをキューに戻す */
        ret = ioctl(src->vdev->fd, VIDIOC_QBUF, &v4l2_buf);
        if (ret < 0)
            perror("VIDIOC_QBUF");
        else
            printf("DEBUG: Requeued buffer %d\n", v4l2_buf.index);
    } else {
        /* 既存のDMABUF処理 */
        ret = v4l2_dequeue_buffer(src->vdev, &buf);
        if (ret < 0)
            return;

        src->src.handler(src->src.handler_data, &src->src, &buf);
    }
}

static void v4l2_source_destroy(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	v4l2_close(src->vdev);
	free(src);
}

static int v4l2_source_set_format(struct video_source *s,
				  struct v4l2_pix_format *fmt)
{
	struct v4l2_source *src = to_v4l2_source(s);

	return v4l2_set_format(src->vdev, fmt);
}

static int v4l2_source_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct v4l2_source *src = to_v4l2_source(s);

	return v4l2_set_frame_rate(src->vdev, fps);
}

static int v4l2_source_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
    struct v4l2_source *src = to_v4l2_source(s);
    int ret;

    if (src->use_mmap) {
        /* MMAP用のバッファ割り当て */
        struct v4l2_requestbuffers req;
        unsigned int i;

        memset(&req, 0, sizeof(req));
        req.count = nbufs;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        ret = ioctl(src->vdev->fd, VIDIOC_REQBUFS, &req);
        if (ret < 0) {
            perror("VIDIOC_REQBUFS");
            return ret;
        }

        if (req.count < 2) {
            fprintf(stderr, "Insufficient buffer memory\n");
            return -ENOMEM;
        }

        src->mmap_buffers = calloc(req.count, sizeof(*src->mmap_buffers));
        if (!src->mmap_buffers) {
            fprintf(stderr, "Out of memory\n");
            return -ENOMEM;
        }

        for (i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf;

            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            ret = ioctl(src->vdev->fd, VIDIOC_QUERYBUF, &buf);
            if (ret < 0) {
                perror("VIDIOC_QUERYBUF");
                return ret;
            }

            src->mmap_buffers[i].length = buf.length;
            src->mmap_buffers[i].start = mmap(NULL, buf.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              src->vdev->fd, buf.m.offset);

            if (MAP_FAILED == src->mmap_buffers[i].start) {
                perror("mmap");
                return -errno;
            }
        }

        src->n_mmap_buffers = req.count;
        return 0;
    } else {
        /* 既存のDMABUF処理 */
        return v4l2_alloc_buffers(src->vdev, V4L2_MEMORY_MMAP, nbufs);
    }
}

static int v4l2_source_export_buffers(struct video_source *s,
                      struct video_buffer_set **bufs)
{
    struct v4l2_source *src = to_v4l2_source(s);
    struct video_buffer_set *buffers;
    unsigned int i;
    int ret;

    if (src->use_mmap) {
        /* MMAPモードの場合、エクスポートは不要 */
        buffers = video_buffer_set_new(src->n_mmap_buffers);
        if (!buffers)
            return -ENOMEM;

        for (i = 0; i < src->n_mmap_buffers; ++i) {
            buffers->buffers[i].size = src->mmap_buffers[i].length;
            buffers->buffers[i].dmabuf = -1; /* DMABUFは使用しない */
        }

        *bufs = buffers;
        return 0;
    } else {
        /* 既存のDMABUF処理 */
        ret = v4l2_export_buffers(src->vdev);
        if (ret < 0)
            return ret;

        buffers = video_buffer_set_new(src->vdev->buffers.nbufs);
        if (!buffers)
            return -ENOMEM;

        for (i = 0; i < src->vdev->buffers.nbufs; ++i) {
            struct video_buffer *buffer = &src->vdev->buffers.buffers[i];

            buffers->buffers[i].size = buffer->size;
            buffers->buffers[i].dmabuf = buffer->dmabuf;
        }

        *bufs = buffers;
        return 0;
    }
}

static int v4l2_source_free_buffers(struct video_source *s)
{
    struct v4l2_source *src = to_v4l2_source(s);
    unsigned int i;

    if (src->use_mmap) {
        /* MMAP用のクリーンアップ */
        for (i = 0; i < src->n_mmap_buffers; ++i) {
            if (munmap(src->mmap_buffers[i].start, src->mmap_buffers[i].length) < 0)
                perror("munmap");
        }
        free(src->mmap_buffers);
        src->mmap_buffers = NULL;
        src->n_mmap_buffers = 0;
        return 0;
    } else {
        /* 既存のDMABUF処理 */
        return v4l2_free_buffers(src->vdev);
    }
}

static int v4l2_source_stream_on(struct video_source *s)
{
    struct v4l2_source *src = to_v4l2_source(s);
    unsigned int i;
    int ret;
    enum v4l2_buf_type type;

    if (src->use_mmap) {
        /* MMAPモード用の処理 */
        for (i = 0; i < src->n_mmap_buffers; ++i) {
            struct v4l2_buffer buf;

            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            ret = ioctl(src->vdev->fd, VIDIOC_QBUF, &buf);
            if (ret < 0) {
                perror("VIDIOC_QBUF");
                return ret;
            }
        }

        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ret = ioctl(src->vdev->fd, VIDIOC_STREAMON, &type);
        if (ret < 0) {
            perror("VIDIOC_STREAMON");
            return ret;
        }
    } else {
        /* 既存のDMABUF処理 */
        for (i = 0; i < src->vdev->buffers.nbufs; ++i) {
            struct video_buffer buf = {
                .index = i,
                .size = src->vdev->buffers.buffers[i].size,
                .dmabuf = src->vdev->buffers.buffers[i].dmabuf,
            };

            ret = v4l2_queue_buffer(src->vdev, &buf);
            if (ret < 0)
                return ret;
        }

        ret = v4l2_stream_on(src->vdev);
        if (ret < 0)
            return ret;
    }

    events_watch_fd(src->src.events, src->vdev->fd, EVENT_READ,
            v4l2_source_video_process, src);

    return 0;
}

static int v4l2_source_stream_off(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	events_unwatch_fd(src->src.events, src->vdev->fd, EVENT_READ);

	return v4l2_stream_off(src->vdev);
}

static int v4l2_source_queue_buffer(struct video_source *s,
				    struct video_buffer *buf)
{
	struct v4l2_source *src = to_v4l2_source(s);

	// return v4l2_queue_buffer(src->vdev, buf);
    if (src->use_mmap) {
        return 0;
    } else {
        /* 既存のDMABUF処理 */
        return v4l2_queue_buffer(src->vdev, buf);
    }
}

static const struct video_source_ops v4l2_source_ops = {
	.destroy = v4l2_source_destroy,
	.set_format = v4l2_source_set_format,
	.set_frame_rate = v4l2_source_set_frame_rate,
	.alloc_buffers = v4l2_source_alloc_buffers,
	.export_buffers = v4l2_source_export_buffers,
	.free_buffers = v4l2_source_free_buffers,
	.stream_on = v4l2_source_stream_on,
	.stream_off = v4l2_source_stream_off,
	.queue_buffer = v4l2_source_queue_buffer,
};

struct video_source *v4l2_video_source_create(const char *devname)
{
    struct v4l2_source *src;

    src = malloc(sizeof *src);
    if (!src)
        return NULL;

    memset(src, 0, sizeof *src);
    src->src.ops = &v4l2_source_ops;

    src->vdev = v4l2_open(devname);
    if (!src->vdev)
        goto err_free_src;

    if (src->vdev->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        fprintf(stderr, "v4l2 device does not support video capture\n");
        goto err_close_v4l2;
    }

    /* DMABUFサポートの検査 */
    if (v4l2_check_dmabuf_support(src->vdev)) {
        printf("Using DMABUF mode for %s\n", devname);
        src->src.type = VIDEO_SOURCE_DMABUF;
        src->use_mmap = false;
    } else {
        printf("Using MMAP mode for %s (DMABUF not supported)\n", devname);
        src->src.type = VIDEO_SOURCE_MMAP;
        src->use_mmap = true;
    }

    return &src->src;

err_close_v4l2:
    v4l2_close(src->vdev);
err_free_src:
    free(src);

    return NULL;
}

void v4l2_video_source_init(struct video_source *s, struct events *events)
{
	struct v4l2_source *src = to_v4l2_source(s);

	src->src.events = events;
}

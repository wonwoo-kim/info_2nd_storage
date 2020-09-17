#include "drm_module.h"

int drm_open(int *out_fd, const char *device)
{
	int fd, ret;
	uint64_t has_dumb;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "Cannot open %s %m\n", device);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n", device);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out_fd = fd;
	return 0;
}

int drm_prepare(int fd, drm_dev_t **modeset_list)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	drm_dev_t *dev;
	int ret;

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "Cannot retrieve DRM resources (%d): %m\n", errno);
		return -errno;
	}

	for (i = 0; i < res->count_connectors; ++i) {
		/* Get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "Cannot retrieve DRM connector %u: %u (%d): %m\n", i, res->connectors[i], errno);
			continue;
		}

		/* Create device structure */
		dev = (drm_dev_t*)malloc(sizeof(*dev));
		memset(dev, 0x00, sizeof(*dev));
		dev->conn = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = drm_setup_dev(fd, res, conn, dev, *modeset_list);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "Cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		drmModeFreeConnector(conn);
		dev->next = *modeset_list;
		*modeset_list = dev;
		(*modeset_list)->next = NULL;
	}

	drmModeFreeResources(res);
 	return 0;
}

int drm_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn, drm_dev_t *dev, drm_dev_t *modeset_list)
{
	int ret;
	
	/* Monitor is Connected? */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
		return -ENOENT;
	}

	/* is at least one valid mode? */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
		return -EFAULT;
	}
	
	/* Copy mode info */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	dev->bufs[0].width = conn->modes[0].hdisplay;
	dev->bufs[0].height = conn->modes[0].vdisplay;
	dev->bufs[1].width = conn->modes[0].hdisplay;	//For Double-Buffering
	dev->bufs[1].height = conn->modes[0].vdisplay;
	fprintf(stderr, "mode for connector %u is %ux%u\n",
				conn->connector_id, dev->bufs[0].width, dev->bufs[0].height);

	/* Find crtc for this connector */
	ret = drm_find_crtc(fd, res, conn, dev, modeset_list);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
		return ret;
	}

	ret = drm_create_fb(fd, &dev->bufs[0]);
	if (ret) {
		fprintf(stderr, "Cannot create framebuffer for connector %u\n", conn->connector_id);
		return ret;
	}

	ret = drm_create_fb(fd, &dev->bufs[1]);
	if (ret) {
		fprintf(stderr, "Cannot create framebuffer for connector %u\n", conn->connector_id);
		drm_destroy_fb(fd, &dev->bufs[0]);
		return ret;
	}

	return 0;
}

int drm_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, drm_dev_t *dev, drm_dev_t* modeset_list)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	drm_dev_t *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;
	
	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >=0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "Cannot retrieve encoder %u:%u (%d): %m\n", i, conn->encoders[i], errno);
			continue;
		}

		for (j = 0; j < res->count_crtcs; j++) {
			/* Check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;
		
			/* Check other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc = crtc) {
					crtc = -1;
					break;
				}
			}

			/* Found CRTC, save & return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}
	}
}

int drm_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "Cannot create dumb buffer (%d): %m\n", errno);
		return -errno;
	}

	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* Create framebuffer for dumb buffer*/
	ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride, buf->handle, &buf->fb);
	if (ret) {
		fprintf(stderr, "Cannot create framebuffer (%d): %m\n", errno);
		ret = -errno;
		goto err_destroy;
	}

	/* buffer for mmap */
	memset(&mreq, 0x00, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "Cannot map dumb buffer (%d): %m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform mmap */
	buf->map = (uint8_t *)mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "Cannot mmap dumb buffer (%d): %m\n", errno);
		ret = -errno;
		goto err_fb;
	}

	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0x00, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

int drm_init(const char *name, drm_dev_t **dev)
{
	TODO("Change Error/Debug printf to macro");

	int ret, fd;
	struct drm_dev *iter;
	struct modeset_buf *buf;

	ret = drm_open(&fd, name);
	if (ret) {
		fprintf(stderr, "Failed to open device\n");
		goto out_return;
	}
	
	ret = drm_prepare(fd, dev);
	if (ret) {
		fprintf(stderr, "Failed to prepare device\n");
		goto out_close;
	}

	for (iter = *dev; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc);
		buf = &iter->bufs[iter->front_buf];
		ret = drmModeSetCrtc(fd, iter->crtc, buf->fb, 0, 0, &iter->conn, 1, &iter->mode);
		if (ret) {
			fprintf(stderr, "Cannot set CRTC for connector %u (%d): %m\n", iter->conn, errno);
		}
	}

	(*dev)->pflip_pending = false;
	(*dev)->fd = fd;
	return 0;
	
out_close: {
	close(fd);
}
out_return: {
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error (%d): %m\n", errno);
	} 
	else {
		fprintf(stderr, "exit\n");
	}
}
}

void drm_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	munmap(buf->map, buf->size);
	drmModeRmFB(fd, buf->fb);

	memset(&dreq, 0x00, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

int drm_cleanup(int fd, drm_dev_t *modeset_list)
{
	int ret;
	drm_dev_t *iter;

	while(modeset_list) {
		iter = modeset_list;
		modeset_list = iter->next;

		ret = drmModeSetCrtc(fd,
					iter->saved_crtc->crtc_id,
					iter->saved_crtc->buffer_id,
					iter->saved_crtc->x,
					iter->saved_crtc->y,
					&iter->conn,
					1,
					&iter->saved_crtc->mode);

		if (ret != 0) {
			fprintf(stderr, "drmModeSetCrtc Failed\n");
			return -1;
		}

		drmModeFreeCrtc(iter->saved_crtc);

		drm_destroy_fb(fd, &iter->bufs[0]);
		drm_destroy_fb(fd, &iter->bufs[1]);

		free(iter);
	}
	
	return 0;
}

int drm_flip(drm_dev_t *dev)
{
	int ret;
	struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];

	drm_check_event(dev);

	if (dev->pflip_pending == true)
		return 0;

	ret = drmModePageFlip(dev->fd, dev->crtc, buf->fb, 
					DRM_MODE_PAGE_FLIP_EVENT, dev);

	if (ret) {
		DRM_ERR("Cannot flip CRTC for connector %u (%d): %m\n",
			dev->conn, errno);
		return -1;
	}
	
	dev->front_buf ^= 1;
	dev->pflip_pending = true;	//When call event handler, it will be set false

	return 0;
}

void drm_flip_handler(int32_t fd, uint32_t frame,
							uint32_t sec, uint32_t usec,
							void *data)
{
	drm_dev_t *dev = (drm_dev_t *)data;

	dev->pflip_pending = false;
}

int drm_check_event(drm_dev_t *dev)
{
	int ret;
	fd_set fds;
	struct timeval v;
	drmEventContext ev;

	FD_ZERO(&fds);
	memset(&v, 0x00, sizeof(v));
	memset(&ev, 0x00, sizeof(ev));

	FD_SET(0, &fds);
	FD_SET(dev->fd, &fds);

	ev.version = 2;
	ev.page_flip_handler = drm_flip_handler;
//	ev.vblank_handler = drm_flip_handler;

	ret = select(dev->fd + 1, &fds, NULL, NULL, &v);
	if (ret < 0) {
		DRM_ERR("select() failed\n");
		return -1;
	}
	else if (FD_ISSET(dev->fd, &fds)) {
		drmHandleEvent(dev->fd, &ev);
	}
}

int drm_draw(drm_dev_t* modeset_list, uint8_t *buf, uint32_t size)
{
//	memcpy((uint8_t*)&modeset_list->map[0], &buf[0], size);

	return 0;
}

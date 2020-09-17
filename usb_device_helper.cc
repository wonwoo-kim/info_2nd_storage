#include "usb_device_helper.h"

static void _mkdir(const char *dir) {
	char tmp[256];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp),"%s",dir);
	len = strlen(tmp);

	if(tmp[len - 1] == '/')
		tmp[len - 1] = 0;

	for(p = tmp + 1; *p; p++)
		if(*p == '/') {
			*p = 0;
			mkdir(tmp, S_IRWXU);
			*p = '/';
		}

	mkdir(tmp, S_IRWXU);
}


int usb_device_init(const char* path ,usb_ctx_t *usb_ctx)
{
	int ret, i;
	char *ep_path;
	usbg_gadget *g;
	usbg_udc *udc;

	/* Create USB Gadget */
	usb_ffs_setup(&(usb_ctx->usbg));

	/* Initialize USB Descriptor */
	init_descriptor();

	/* If not exist */
	_mkdir(path);

	ret = mount("test", path, "functionfs", 0, "rmode=0770,fmode=0660,uid=1024,gid=1024");
	if (ret != 0) {
		USB_ERR("Failed to mount functionfs : %s\n", strerror(errno));
		return -1;
	}

	/* Set Endpoint Path */
	ep_path = (char *)malloc(strlen(path) + 4 /* "/ep#" */ + 1 /* '\0' */);
	sprintf(ep_path, "%s/ep0", path);

	usb_ctx->ep0 = open(ep_path, O_RDWR);
	if (usb_ctx->ep0 < 0) {
		USB_ERR("Failed to open ep0 : %s\n", strerror(errno));
		return -1;
	}

	ret = write(usb_ctx->ep0, &descriptors, sizeof(descriptors));
	if (ret < 0) {
		USB_ERR("Failed to write descriptors to endpoint : %s\n", strerror(errno));
		return -1;
	}

	ret = write(usb_ctx->ep0, &strings, sizeof(strings));
	if (ret < 0) {
		USB_ERR("Failed to write strings : %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; i < 2; ++i) {
		sprintf(ep_path, "%s/ep%d", path, i+1);
		usb_ctx->ep[i] = open(ep_path, O_RDWR);
		if (usb_ctx->ep[i] < 0) {
			
			return -1;
		}
	}

	free(ep_path);

	/* Setup aio context */
	memset(&(usb_ctx->io_ctx), 0x00, sizeof(io_context_t));

	ret = io_setup(2, &(usb_ctx->io_ctx));
	if (ret < 0) {
	
		return -1;
	}

	usb_ctx->evfd = eventfd(0, 0);
	if (usb_ctx->evfd < 0) {

		return -1;		
	}

	g = usbg_get_gadget(usb_ctx->usbg, "g1");
	if (ret != USBG_SUCCESS) {
		USB_ERR("Error : %s : %s\n", usbg_error_name((usbg_error)ret),
			usbg_strerror((usbg_error)ret));
		usbg_cleanup(usb_ctx->usbg);
		return -1;	
	}

	udc = usbg_get_udc(usb_ctx->usbg, "fd000000.dwc3");
	if (ret != USBG_SUCCESS) {
		USB_ERR("Error : %s : %s\n", usbg_error_name((usbg_error)ret),
			usbg_strerror((usbg_error)ret));
		usbg_cleanup(usb_ctx->usbg);
		return -1;	
	}

	ret = usbg_enable_gadget(g, udc);
	if (ret != USBG_SUCCESS) {
		USB_ERR("Error : %s : %s\n", usbg_error_name((usbg_error)ret),
			usbg_strerror((usbg_error)ret));
		usbg_cleanup(usb_ctx->usbg);
		return -1;	
	}

	usb_ctx->iocb_in = (struct iocb*)malloc(sizeof(struct iocb));
	usb_ctx->iocb_out = (struct iocb*)malloc(sizeof(struct iocb));

	USB_DBG("Done!\n");

	return 0;
}

int usb_device_wait_ready(usb_ctx_t *usb_ctx)
{
	int ret;
	char str_buf[128];
	fd_set rfds;

	while (!(usb_ctx->state & USB_STATE_READY))
	{
		FD_ZERO(&rfds);
		FD_SET(usb_ctx->ep0, &rfds);
		FD_SET(usb_ctx->evfd, &rfds);

		ret = select(((usb_ctx->ep0 > usb_ctx->evfd) ? usb_ctx->ep0 : usb_ctx->evfd) + 1,
					&rfds, NULL, NULL, NULL);

		if ((ret < 0) && (errno != EINTR)) {
			strerror_r(errno, str_buf, sizeof(str_buf));
			USB_ERR("select() : %s\n", str_buf);
			return -1;
		}

		if (FD_ISSET(usb_ctx->ep0, &rfds))
			usb_handle_ep0(usb_ctx->ep0, &(usb_ctx->state));
	}

	/* Prepare write request (Device -> Host) */
	io_prep_pwrite(usb_ctx->iocb_in, usb_ctx->ep[0], str_buf, 128, 0);
	/* Enable eventfd notification */
	usb_ctx->iocb_in->u.c.flags |= IOCB_FLAG_RESFD;
	usb_ctx->iocb_in->u.c.resfd = usb_ctx->evfd;

	/* Submit table of requests */
	ret = io_submit(usb_ctx->io_ctx, 1, &(usb_ctx->iocb_in));
	if (ret >= 0) {
		USB_DBG("Submit : in\n");
	}
	else {
		strerror_r(errno, str_buf, sizeof(str_buf));
		USB_ERR("Unable to submit request : %s\n", str_buf);
	}

	/* Prepare read request (Device <- Host) */
	io_prep_pread(usb_ctx->iocb_out, usb_ctx->ep[1], str_buf, 128, 0);
	/* Enable eventfd notification */
	usb_ctx->iocb_out->u.c.flags |= IOCB_FLAG_RESFD;
	usb_ctx->iocb_out->u.c.resfd = usb_ctx->evfd;

	/* Submit table of requests */
	ret = io_submit(usb_ctx->io_ctx, 1, &(usb_ctx->iocb_out));
	if (ret >= 0) {
		USB_DBG("Submit : out\n");
	}
	else {
		strerror_r(errno, str_buf, sizeof(str_buf));
		USB_ERR("Unable to submit request : %s\n", str_buf);
	}

	USB_DBG("USB Ready\n");
	return 0;
}


int usb_device_send(usb_ctx_t *usb_ctx, void *data, ssize_t len)
{
	int ret, i;
	char str_buf[128];
	fd_set rfds;
	bool req_in = false, req_out = false;
	struct timeval tv = { tv_sec : 0, tv_usec : 0 };

	FD_ZERO(&rfds);
	FD_SET(usb_ctx->ep0, &rfds);
	FD_SET(usb_ctx->evfd, &rfds);

	ret = select(((usb_ctx->ep0 > usb_ctx->evfd) ? usb_ctx->ep0 : usb_ctx->evfd) + 1,
				&rfds, NULL, NULL, &tv); /* Blocking */

	if ((ret < 0) && (errno != EINTR)) {
		strerror_r(errno, str_buf, sizeof(str_buf));
		USB_ERR("select() : %s\n", str_buf);
		return -1;
	}

	if (FD_ISSET(usb_ctx->ep0, &rfds))
		usb_handle_ep0(usb_ctx->ep0, &(usb_ctx->state));

	if (!(usb_ctx->state & USB_STATE_READY)) {
		USB_ERR("USB not ready\n");
		return -1;
	}

	// Check data request from host
	if (FD_ISSET(usb_ctx->evfd, &rfds)) {
		uint64_t ev_cnt;
		ret = read(usb_ctx->evfd, &ev_cnt, sizeof(ev_cnt));
		if (ret < 0) {
			strerror_r(errno, str_buf, sizeof(str_buf));
			USB_ERR("Unable read eventfd : %s\n", str_buf);
			return -1;
		}

		struct io_event e[2];
		ret = io_getevents(usb_ctx->io_ctx, 1, 2, e, NULL);

		/* if get event */
		for (i = 0; i < ret; ++i) {
			if (e[i].obj->aio_fildes == usb_ctx->ep[0]) {	// In request
				USB_DBG("ev_in; ret=%lu\n", e[i].res);
				req_in = true;
			}
			else if (e[i].obj->aio_fildes == usb_ctx->ep[1]) { // Out request
				USB_DBG("ev=output; ret=%lu\n", e[i].res);
				req_out = true;
			}
		}
	}

	if (req_in) {
		/* Prepare write request (Device -> Host) */
		io_prep_pwrite(usb_ctx->iocb_in, usb_ctx->ep[0], data, len, 0);
		/* Enable eventfd notification */
		usb_ctx->iocb_in->u.c.flags |= IOCB_FLAG_RESFD;
		usb_ctx->iocb_in->u.c.resfd = usb_ctx->evfd;

		/* Submit table of requests */
		ret = io_submit(usb_ctx->io_ctx, 1, &(usb_ctx->iocb_in));
		if (ret >= 0) {
			USB_DBG("Submit : in\n");
		}
		else {
			strerror_r(errno, str_buf, sizeof(str_buf));
			USB_ERR("Unable to submit request : %s\n", str_buf);
		}
	}

	if (req_out) {
		/* Prepare read request (Device <- Host) */
		io_prep_pread(usb_ctx->iocb_out, usb_ctx->ep[1], data, len, 0);
		/* Enable eventfd notification */
		usb_ctx->iocb_out->u.c.flags |= IOCB_FLAG_RESFD;
		usb_ctx->iocb_out->u.c.resfd = usb_ctx->evfd;

		/* Submit table of requests */
		ret = io_submit(usb_ctx->io_ctx, 1, &(usb_ctx->iocb_out));
		if (ret >= 0) {
			USB_DBG("Submit : out\n");
		}
		else {
			strerror_r(errno, str_buf, sizeof(str_buf));
			USB_ERR("Unable to submit request : %s\n", str_buf);
		}
	}
}

int usb_device_deinit(usb_ctx_t *usb_ctx)
{
	int i, ret;
	usbg_gadget *g;

	ret = io_destroy(usb_ctx->io_ctx);
	if (ret != 0) {
		USB_ERR("Failed to destroy aio : %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; i < 2; i++) {
		close(usb_ctx->ep[i]);
	}

	close(usb_ctx->ep0);

	g = usbg_get_gadget(usb_ctx->usbg, "g1");
	if (g == NULL) {
		USB_ERR("Failed to get usb gadget\n");
		return -1;
	}

	/* USB Gadget disable */
	ret = usbg_rm_gadget(g, USBG_RM_RECURSE);
	if (ret != USBG_SUCCESS) {
		USB_ERR("Failed to remove usb gadget : %s\n",
			usbg_strerror((usbg_error)ret));
		return -1;
	}

	/* USB Gadget cleanup */
	usbg_cleanup(usb_ctx->usbg);
	if (ret != USBG_SUCCESS) {
		USB_ERR("Failed to cleanup usb gadget : %s\n",
			usbg_strerror((usbg_error)ret));
		return -1;
	}

	/* unmount configfs/functionfs */
	ret = umount("/sys/kernel/config");
	if (ret != 0) {
		USB_ERR("Failed to unmount configfs : %s\n", 
				strerror(errno));
		return -1;
	}

	ret = umount("/dev/usb-ffs/test");
	if (ret != 0) {
		USB_ERR("Failed to unmount functionfs : %s\n", 
				strerror(errno));
		return -1;
	}

	free(usb_ctx->iocb_in);
	free(usb_ctx->iocb_out);

	return 0;
}

static void display_event(struct usb_functionfs_event *event)
{
	static const char *const names[] = {
		[FUNCTIONFS_BIND] = "BIND",
		[FUNCTIONFS_UNBIND] = "UNBIND",
		[FUNCTIONFS_ENABLE] = "ENABLE",
		[FUNCTIONFS_DISABLE] = "DISABLE",
		[FUNCTIONFS_SETUP] = "SETUP",
		[FUNCTIONFS_SUSPEND] = "SUSPEND",
		[FUNCTIONFS_RESUME] = "RESUME",
	};

	switch (event->type) {
		case FUNCTIONFS_BIND:
		case FUNCTIONFS_UNBIND:
		case FUNCTIONFS_ENABLE:
		case FUNCTIONFS_DISABLE:
		case FUNCTIONFS_SETUP:
		case FUNCTIONFS_SUSPEND:
		case FUNCTIONFS_RESUME:
			printf("Event %s\n", names[event->type]);
	}
}

void usb_handle_ep0(int ep0, uint8_t *state)
{
	struct usb_functionfs_event event;
	int ret;
	char buf[128];

	struct pollfd pfds[1];
	pfds[0].fd = ep0;
	pfds[0].events = POLLIN;

	ret = poll(pfds, 1, 0);

	if (ret && pfds[0].revents & POLLIN) {
		ret = read(ep0, &event, sizeof(event));
		if (!ret) {
			strerror_r(errno, buf, sizeof(buf));	// Thread-safe
			USB_ERR("Unable to read event from ep0 : %s\n", buf);
			return;
		}

		display_event(&event);

		switch (event.type) {
			case FUNCTIONFS_SETUP:
				if (event.u.setup.bRequestType & USB_DIR_IN)
					write(ep0, NULL, 0);
				else
					read(ep0, NULL, 0);
				break;

			case FUNCTIONFS_ENABLE:
				*state |= USB_STATE_READY;
				break;

			case FUNCTIONFS_DISABLE:
				*state &= ~USB_STATE_READY;
				break;

			default:
				break;
		}
	}
}


static inline void init_descriptor()
{
	/* Descriptor header	*/
	descriptors.header.magic	= htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
	descriptors.header.flags	= htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | 
													FUNCTIONFS_HAS_SS_DESC | FUNCTIONFS_HAS_MS_OS_DESC);
	descriptors.header.length	= htole32(sizeof(descriptors));

	/* FullSpeed descriptor */
	descriptors.fs_count			= htole32(3);

	descriptors.fs_descs.intf.bLength			= sizeof(descriptors.fs_descs.intf);
	descriptors.fs_descs.intf.bDescriptorType	= USB_DT_INTERFACE;
	descriptors.fs_descs.intf.bNumEndpoints	= 2;
	descriptors.fs_descs.intf.bInterfaceClass	= USB_CLASS_VENDOR_SPEC;
	descriptors.fs_descs.intf.iInterface		= 1;

	descriptors.fs_descs.bulk_sink.bLength				= sizeof(descriptors.fs_descs.bulk_sink);
	descriptors.fs_descs.bulk_sink.bDescriptorType	= USB_DT_ENDPOINT;
	descriptors.fs_descs.bulk_sink.bEndpointAddress	= 1 | USB_DIR_IN;
	descriptors.fs_descs.bulk_sink.bmAttributes		= USB_ENDPOINT_XFER_BULK;

	descriptors.fs_descs.bulk_source.bLength				= sizeof(descriptors.fs_descs.bulk_source);
	descriptors.fs_descs.bulk_source.bDescriptorType	= USB_DT_ENDPOINT;
	descriptors.fs_descs.bulk_source.bEndpointAddress	= 2 | USB_DIR_OUT;
	descriptors.fs_descs.bulk_source.bmAttributes		= USB_ENDPOINT_XFER_BULK;

	/* HighSpeed descriptor */
	descriptors.hs_count			= htole32(3);

	descriptors.hs_descs.intf.bLength			= sizeof(descriptors.hs_descs.intf);
	descriptors.hs_descs.intf.bDescriptorType	= USB_DT_INTERFACE;
	descriptors.hs_descs.intf.bNumEndpoints	= 2;
	descriptors.hs_descs.intf.bInterfaceClass	= USB_CLASS_VENDOR_SPEC;
	descriptors.hs_descs.intf.iInterface		= 1;

	descriptors.hs_descs.bulk_sink.bLength				= sizeof(descriptors.hs_descs.bulk_sink);
	descriptors.hs_descs.bulk_sink.bDescriptorType	= USB_DT_ENDPOINT;
	descriptors.hs_descs.bulk_sink.bEndpointAddress	= 1 | USB_DIR_IN;
	descriptors.hs_descs.bulk_sink.bmAttributes		= USB_ENDPOINT_XFER_BULK;
	descriptors.hs_descs.bulk_sink.wMaxPacketSize	= htole16(512);
	
	descriptors.hs_descs.bulk_source.bLength				= sizeof(descriptors.fs_descs.bulk_source);
	descriptors.hs_descs.bulk_source.bDescriptorType	= USB_DT_ENDPOINT;
	descriptors.hs_descs.bulk_source.bEndpointAddress	= 2 | USB_DIR_OUT;
	descriptors.hs_descs.bulk_source.bmAttributes		= USB_ENDPOINT_XFER_BULK;
	descriptors.hs_descs.bulk_source.wMaxPacketSize		= htole16(512);

	/* SuperSpeed descriptor */
	descriptors.ss_count			= htole32(5);

	descriptors.ss_descs.intf.bLength				= sizeof(descriptors.ss_descs.intf);
	descriptors.ss_descs.intf.bDescriptorType		= USB_DT_INTERFACE;
	descriptors.ss_descs.intf.bInterfaceNumber	= 0;
	descriptors.ss_descs.intf.bNumEndpoints		= 2;
	descriptors.ss_descs.intf.bInterfaceClass		= USB_CLASS_VENDOR_SPEC;
	descriptors.ss_descs.intf.iInterface			= 1;

	descriptors.ss_descs.sink.bLength				= sizeof(descriptors.ss_descs.sink);
	descriptors.ss_descs.sink.bDescriptorType		= USB_DT_ENDPOINT;
	descriptors.ss_descs.sink.bEndpointAddress	= 1 | USB_DIR_IN;
	descriptors.ss_descs.sink.bmAttributes			= USB_ENDPOINT_XFER_BULK;
	descriptors.ss_descs.sink.wMaxPacketSize		= htole16(1024);
	
	descriptors.ss_descs.sink_comp.bLength				= sizeof(descriptors.ss_descs.sink_comp);
	descriptors.ss_descs.sink_comp.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP;
	descriptors.ss_descs.sink_comp.bMaxBurst			= 4;
	
	descriptors.ss_descs.source.bLength				= sizeof(descriptors.ss_descs.source);
	descriptors.ss_descs.source.bDescriptorType	= USB_DT_ENDPOINT;
	descriptors.ss_descs.source.bEndpointAddress	= 2 | USB_DIR_OUT;
	descriptors.ss_descs.source.bmAttributes		= USB_ENDPOINT_XFER_BULK;
	descriptors.ss_descs.source.wMaxPacketSize	= htole16(1024);

	descriptors.ss_descs.source_comp.bLength				= sizeof(descriptors.ss_descs.source_comp);
	descriptors.ss_descs.source_comp.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP;
	descriptors.ss_descs.source_comp.bMaxBurst			= 4;
	
	/* Microsoft OS Descriptor */
	descriptors.os_count = htole32(1);

	descriptors.os_header.interface	= htole32(1);
	descriptors.os_header.dwLength	= htole32(sizeof(descriptors.os_header) + sizeof(descriptors.os_desc));
	descriptors.os_header.bcdVersion	= htole32(1);
	descriptors.os_header.wIndex		= htole32(4);
	descriptors.os_header.bCount		= htole32(1);
	descriptors.os_header.Reserved	= htole32(0);
	
	descriptors.os_desc.bFirstInterfaceNumber	= 0;
	descriptors.os_desc.Reserved1					= htole32(1);

	memset(&(descriptors.os_desc.Reserved2), 0x00, sizeof(descriptors.os_desc.Reserved2));
	memset(&(descriptors.os_desc.CompatibleID), 0x00, sizeof(descriptors.os_desc.CompatibleID));
	memset(&(descriptors.os_desc.SubCompatibleID), 0x00, sizeof(descriptors.os_desc.SubCompatibleID));
}

int usb_ffs_setup(usbg_state **state)
{
	usbg_state *s;
	usbg_gadget *g;
	usbg_config *c;
	usbg_function *ffs;

	int ret;
	const uint32_t VENDOR = 0x1d6b;
	const uint32_t PRODUCT = 0x0105;

	struct usbg_gadget_attrs g_attrs = {
		bcdUSB : 0x0300,
		bDeviceClass : USB_CLASS_PER_INTERFACE,
		bDeviceSubClass : 0x00,
		bDeviceProtocol : 0x00,
		bMaxPacketSize0 : 64, //512,		/* ep0 max packet size */
		idVendor : VENDOR,
		idProduct : PRODUCT,
		bcdDevice : 0x0001
	};

	struct usbg_gadget_strs g_strs = {
		manufacturer : (char *)"Infoworks",
		product : (char *)"Fusion Device",
		serial : (char *)"0123456789"
	};

	struct usbg_config_strs c_strs = {
		configuration : (char *)"test"
	};

	ret = mount("none", "/sys/kernel/config", "configfs", 0, 0);
	if (ret != 0) {
		USB_ERR("Failed to mount configfs\n");
	}

	ret = usbg_init("/sys/kernel/config", &s);
	if (ret != USBG_SUCCESS) {
		USB_ERR("USB Gadget init error\n");
		goto out_error;
	}

	ret = usbg_create_gadget(s, "g1", &g_attrs, &g_strs, &g);
	if (ret != USBG_SUCCESS) {
		USB_ERR("USB Gadget create error\n");
		goto out_error;
	}

	ret = usbg_create_function(g, USBG_F_FFS, "test", NULL, &ffs);
	if (ret != USBG_SUCCESS) {
		USB_ERR("USB function create error\n");
		goto out_error;
	}

	ret = usbg_create_config(g, USBG_F_FFS, "test", NULL, &c_strs, &c);
	if (ret != USBG_SUCCESS) {
		USB_ERR("USB function create error\n");
		goto out_error;
	}

	ret = usbg_add_config_function(c, "config_binding", ffs);
	if (ret != USBG_SUCCESS) {
		USB_ERR("Error adding ffs\n");
		goto out_error;
	}

	*state = s;

	/* Success*/
	return 0;

out_error: {
	USB_ERR("Error : %s : %s\n", usbg_error_name((usbg_error)ret),
			usbg_strerror((usbg_error)ret));
	usbg_cleanup(s);
	return -1;
}
}

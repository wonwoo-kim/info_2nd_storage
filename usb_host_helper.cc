#include "usb_host_helper.h"

int usb_host_init(usb_state_t *state, uint16_t vendor, uint16_t product)
{
	int i, ret;
	ssize_t cnt;
	libusb_device **list;

	state->found = NULL;
	state->ctx = NULL;
	state->handle = NULL;
	state->attached = 0;

	state->conf = NULL;
	state->iface = NULL;

	/* Init libusb before other function calling */
	ret = libusb_init(&state->ctx);
	if (ret) {
		fprintf(stderr, "Cannot init libusb %s\n", libusb_error_name(ret));
		return 1;
	}

	/* Wait until device(vendor, product) found */
	while(!state->found) {
		cnt = libusb_get_device_list(state->ctx, &list);
		if (cnt <= 0) {
			fprintf(stderr, "No devices found\n");
			continue; 	//TODO
		}

		for (i = 0; i < cnt; ++i) {
			libusb_device *dev = list[i];
			struct libusb_device_descriptor desc;

			ret = libusb_get_device_descriptor(dev, &desc);
			if (ret) {
				fprintf(stderr, "unable to get device descriptor: %s\n", libusb_error_name(ret));
				goto error2;
			}
			if(desc.idVendor == vendor && desc.idProduct == product) {
				state->found = dev;
				break;
			}
		}
	
		if (!state->found) {
			fprintf(stderr, "No devices found!\n");
   		libusb_free_device_list(list, 1); 
			continue;	//TODO
		}
		
		ret = libusb_open(state->found, &state->handle);
		if (ret) {
			fprintf(stderr, "Cannot open device: %s\n", libusb_error_name(ret));
			goto error2;
		}

		if (libusb_claim_interface(state->handle, 0)) {
			ret = libusb_detach_kernel_driver(state->handle, 0);
			if (ret) {
				fprintf(stderr, "Unable to detach kernel driver: %s\n", libusb_error_name(ret));
				goto error3;
			}

			state->attached = 1;
			ret = libusb_claim_interface(state->handle, 0);
			if (ret) {
				fprintf(stderr, "Cannot claim interface: %s\n", libusb_error_name(ret));
				goto error4;
			}
		}
	}

	ret = libusb_get_config_descriptor(state->found, 0, &state->conf);
	if (ret != 0) {
		fprintf(stderr, "Cannot get usb config descriptor : %s\n", libusb_error_name(ret));
		goto error4;
	}

	state->iface = &state->conf->interface[0].altsetting[0];
	state->in_addr = state->iface->endpoint[0].bEndpointAddress;
	state->out_addr = state->iface->endpoint[1].bEndpointAddress;

	
	return 0;

error4:
   if (state->attached == 1)
      libusb_attach_kernel_driver(state->handle, 0); 

error3:
   libusb_close(state->handle);

error2:
   libusb_free_device_list(list, 1); 

error1:
   libusb_exit(state->ctx);
   return 1;	
}

int usb_host_exit(usb_state_t *state)
{
	libusb_release_interface(state->handle, 0);

	if (state->attached == 1) {
		libusb_attach_kernel_driver(state->handle, 0);
	}

	libusb_close(state->handle);
	libusb_exit(state->ctx);
}

int usb_host_transfer(usb_state_t *state, uint8_t *buf, uint32_t size, uint8_t direction)
{
	int ret, bytes;

	if	(direction == 0) {
		ret = libusb_bulk_transfer(state->handle, state->in_addr, buf, size, &bytes, 500);
	} 
	else {
		ret = libusb_bulk_transfer(state->handle, state->out_addr, buf, size, &bytes, 500);
	}

	if (ret != 0) {
		fprintf(stderr, "libusb_bulk_transfer failed : %s\n", libusb_error_name(ret));
		return -1;
	}

	return bytes;
}

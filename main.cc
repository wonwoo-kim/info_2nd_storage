#include "app.h"
uint8_t jpg_buf[1920*1080*4];
uint8_t jpg_480p_buf[640*480*4];

uint8_t jpg_eo_buf[1225728];
uint8_t jpg_eo_480p_buf[460800];

uint8_t jpg_ir_buf[817152];
uint8_t jpg_origin_ir_buf[307200];

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

// Your code must be between pthread_mutex_lock() and status=true.
void *camera_loop(void *arg)
{
	int ret;
	ssize_t size, cap_size, eo_480p_size;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;
	uint8_t *tmp = (uint8_t*)malloc(1920*1080*4);

	struct timeval start, end;
	double diffTime;

	size = ctx->eo.width * ctx->eo.height;
	eo_480p_size = ctx->eo_480p.width * ctx->eo_480p.height;






	ret = camera_streamon(ctx->eo.fd, 3);

	while (1) {
		ret = camera_get_frame_helper(ctx->eo.fd, &ctx->eo.buf, &cap_size);
#if 0
		recvmsg(ctx->nl.fd, &ctx->nl.msg, 0);
		
		gettimeofday(&end, NULL);
		diffTime = (end.tv_sec) * 1000.0;
		diffTime += (end.tv_usec) / 1000.0;

		printf("recvmsg %f\n", diffTime);
#endif

		rknn_run_helper(ctx->fus.ctx, ctx->eo.buf, size, ctx->fus.buf);
		memcpy(&(ctx->fus.buf[size]), &(ctx->eo.buf[size]), (size)/2);

		pthread_cond_signal(&cond);
	}
}

// About using condition variable
// https://stackoverflow.com/questions/16522858/understanding-of-pthread-cond-wait-and-pthread-cond-signal/16524148#16524148
// spurious wakeups
// https://stackoverflow.com/questions/8594591/why-does-pthread-cond-wait-have-spurious-wakeups

void *display_loop(void *arg)
{
	int ret;
	ssize_t size;
	rga_transform_t src, dst;
	struct timeval start, end;
	double diffTime;
	int crop_y;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;
	drm_dev_t *mode = ctx->disp.list;

	CLEAR(src); CLEAR(dst);

	size = ctx->eo.width * ctx->eo.height;
	
	/*
	struct timeval start, end;
	double diffTime;

	gettimeofday(&start, NULL);
	gettimeofday(&end, NULL);
	diffTime = (end.tv_sec - start.tv_sec) * 1000.0;
	diffTime += (end.tv_usec - start.tv_usec) / 1000.0;
	printf("Display done : %f\n", diffTime);
*/
	
	while (1) {
		pthread_cond_wait(&cond, &mtx);
		usb_host_transfer(&ctx->usb.ctx, ctx->usb.buf, 307200, 0);


		// IR Up-Scaling
		src = {	.data = ctx->usb.buf, .width = (int)ctx->ir.height, .height = (int)ctx->ir.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		dst = {	.data = ctx->ir.buf, .width = (int)ctx->eo.height, .height = (int)ctx->eo.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		rga_transform(&src, &dst);


/*
		// IR Origin
		
		src = {	.data = ctx->usb.buf, .width = (int)ctx->ir.height, .height = (int)ctx->ir.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		dst = {	.data = ctx->ir_origin.buf, .width = (int)ctx->ir.height, .height = (int)ctx->ir.width, 
					.format = RK_FORMAT_YCbCr_420_P, .direction = 0 };
		rga_transform(&src, &dst);
*/
		

		//printf("ctx->ir.height = %d   ctx->ir.width = %d    \n",(int)ctx->ir.height,(int)ctx->ir.width);
					// 480	640


		// YCbCr Gray
		memset(ctx->tmpbuf, 128, size*2);


		// Legacy Fusion
		pthread_mutex_lock(&ctx->mutex_lock);
		fusion_legacy(ctx->fus.buf, ctx->ir.buf, ctx->tmpbuf,
							ctx->eo.width, ctx->eo.height, ctx->fus.crop_y, 6);
		pthread_mutex_unlock(&ctx->mutex_lock);


		// Display Fusion Image
		src = {	.data = ctx->tmpbuf, 
					.width = (int)(ctx->eo.height - ctx->fus.crop_y),
					.height = (int)ctx->eo.width, 
					.format = RK_FORMAT_YCbCr_420_P, 
					.direction = 0 };
		dst = {	.data = mode->bufs[mode->front_buf ^ 1].map, 
					.width = (int)ctx->disp.height, 
					.height = (int)ctx->disp.width, 
					.format = RK_FORMAT_BGRA_8888, 
					.direction = 0 };

		rga_transform(&src, &dst);
		drm_flip(mode);

//		memcpy(ctx->disp.list->map, jpg_buf, 1920*1080*4);
		
	}
}

void *usb_loop(void *arg)
{
	int ret;
	size_t size;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

//	size = ctx->ir.width * ctx->ir.height;
	size = ctx->eo.width * ctx->eo.height;

	while(1) {
//		usb_host_transfer(&ctx->usb.ctx, ctx->usb.buf, 307200, 0);
	}

}

void *icd_loop(void *arg)
{
	fd_set rfds;
	int ret, fd_max;
	uint8_t tmp[100];
	ssize_t bytes = 0;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;

//	fd_max = MAX(ctx->eo.uart.fd, ctx->ir.uart.fd);
	fd_max = MAX(ctx->eo.uart.fd, ctx->icd.fd);

	while(1)
	{
		// Add uart fd to check list
		FD_ZERO(&rfds);
		FD_SET(ctx->icd.fd, &rfds);
		FD_SET(ctx->eo.uart.fd, &rfds);
//		FD_SET(ctx->ir.uart.fd, &rfds);

		select(fd_max + 1, &rfds, NULL, NULL, NULL);

		if (FD_ISSET(ctx->icd.fd, &rfds)) {
			bytes = read(ctx->icd.fd, tmp, 100);
			if (bytes > 0) {
				ret = mq_send(ctx->icd.mfd, (const char*)tmp, 100, 1);
				if (ret != 0) {
					printf("Failed to send message queue\n");
				}
				pthread_cond_signal(&ctx->thread_cond);
			}		
		}
		else if (FD_ISSET(ctx->eo.uart.fd, &rfds)) {
			uint32_t ack = (uint32_t)0xA1B2C3FE;
			bytes = read(ctx->eo.uart.fd, tmp, 100);
			if (bytes > 0) {
				printf("EO READ : ");
				for (int i = 0; i < bytes; i++)
					printf("0x%02X ", tmp[i]);
				printf("\n");

				write(ctx->eo.uart.fd, tmp, 100);
			}

		}
#if 0
		else if (FD_ISSET(ctx->ir.uart.fd, &rfds)) {
			bytes = read(ctx->ir.uart.fd, tmp, 100);
			if (bytes > 0) {
				printf("IR READ : ");
				for (int i = 0; i < bytes; i++)
					printf("0x%02X ", tmp[i]);
				printf("\n");
			}
		}
#endif
	}

}

void *cmd_loop(void *arg)
{
	int ret, idx = 0;
	uint8_t tmp[100];
	char filename[50];
	uint16_t eo_y, ir_y;
	daytime_ctx_t *ctx = (daytime_ctx_t*)arg;
	int mode_num=0;
	while(1)
	{
		pthread_cond_wait(&ctx->thread_cond, &ctx->mutex_lock);
		
		ret = mq_receive(ctx->icd.mfd, (char *)tmp, 100, NULL);
		if (ret == -1) {
			perror("Failed to recv message queue ");
		}

		int mode_buf = check_cmd(tmp, 100);
		if(mode_buf<0){}
		else
		{
			mode_num = mode_buf;
		}

		if (tmp[5] == REGISTRATION_Y) {
			eo_y = (uint16_t) ((tmp[6] << 8) | tmp[7]);
			
			if (eo_y > 200)
				eo_y = 200;

			if (eo_y % 2 == 1)
				eo_y += 1;

			ctx->fus.crop_y = eo_y;
			printf("Change Y : %d\n", eo_y);
		}
		else if (tmp[5] == DATA_LOGGING && tmp[6] == 0x01) {
			if(mode_num==0)
			{
				memcpy(jpg_buf, ctx->disp.list->bufs[ctx->disp.list->front_buf ^ 1].map, 1920*1080*4);
				printf("FUSION save start\n");
				sprintf(filename, "/sdcard/FU-%d.jpg", idx);
				write_jpeg(filename, jpg_buf, 1920, 1080, 4, TJPF_BGRA, 75);
			}
			else if(mode_num==1)
			{
				memcpy(jpg_eo_buf, ctx->eo.buf, 1225728);
				printf("EO save start\n");
				sprintf(filename, "/sdcard/EO-%d.jpg", idx);
				write_jpeg_yuv(filename, jpg_eo_buf, 1024, 798, 2, TJSAMP_420, 80);
			}
			else if(mode_num==2)
			{
				memcpy(jpg_ir_buf, ctx->ir.buf, 817152);
				printf("IR save start\n");
				sprintf(filename, "/sdcard/IR-%d.jpg", idx);
				write_jpeg(filename, jpg_ir_buf, 1024, 798, 1, TJPF_GRAY, 80);
			}
			else
			{
				printf("choose mode\n");
			}
			idx++;
			printf("Image saved\n");
		}
		
		else if (tmp[5] == DATA_LOGGING && tmp[6] == 0x02)
		{
			memcpy(jpg_buf, ctx->disp.list->bufs[ctx->disp.list->front_buf ^ 1].map, 1920*1080*4);
			memcpy(jpg_eo_buf, ctx->eo.buf, 1225728);
			memcpy(jpg_origin_ir_buf, ctx->usb.buf, 307200);
		
			printf("FUSION save start\n");
			sprintf(filename, "/sdcard/FU-%d.jpg", idx);
			write_jpeg(filename, jpg_buf, 1920, 1080, 4, TJPF_BGRA, 75);

			printf("EO save start\n");
			sprintf(filename, "/sdcard/EO-%d.jpg", idx);
			write_jpeg_yuv(filename, jpg_eo_buf, 1024, 798, 2, TJSAMP_420, 80);

			printf("IR save start\n");
			sprintf(filename, "/sdcard/IR_origin-%d.jpg", idx);
			write_jpeg(filename, jpg_origin_ir_buf, 640, 480, 1, TJPF_GRAY, 80);

			idx++;
			printf("all type images are saved\n");
		}			

	}
}

int init_netlink(daytime_ctx_t *ctx)
{
	int ret;
	struct buff_info buf;
	size_t MAX_PAYLOAD = sizeof(struct buff_info);

	buf.bytes = 1234;
	buf.addr = ctx->eo.buf;

	ctx->nl.fd = socket(PF_NETLINK, SOCK_RAW, 31);
	if (ctx->nl.fd < 0) {
		APP_ERR("Failed to create netlink socket\n");
		return -1;
	}

	memset(&ctx->nl.src_addr, 0x00, sizeof(struct sockaddr_nl));
	ctx->nl.src_addr.nl_family = AF_NETLINK;
	ctx->nl.src_addr.nl_pid = getpid();

	bind(ctx->nl.fd, (struct sockaddr*)&ctx->nl.src_addr, sizeof(struct sockaddr_nl));

	memset(&ctx->nl.dst_addr, 0x00, sizeof(struct sockaddr_nl));
	ctx->nl.dst_addr.nl_family = AF_NETLINK;
	ctx->nl.dst_addr.nl_pid = 0;	/* Kernel */
	ctx->nl.dst_addr.nl_groups = 0;
	
	ctx->nl.nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
	memset(ctx->nl.nlh, 0x00, NLMSG_SPACE(MAX_PAYLOAD));
	ctx->nl.nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
	ctx->nl.nlh->nlmsg_pid = getpid();
	ctx->nl.nlh->nlmsg_flags = 0;

	memcpy(NLMSG_DATA(ctx->nl.nlh), &buf, sizeof(buff_info));
	ctx->nl.iov.iov_base		= (void *)ctx->nl.nlh;
	ctx->nl.iov.iov_len		= ctx->nl.nlh->nlmsg_len;
	ctx->nl.msg.msg_name		= (void *)&ctx->nl.dst_addr;
	ctx->nl.msg.msg_namelen	= sizeof(struct sockaddr_nl);
	ctx->nl.msg.msg_iov		= &ctx->nl.iov;
	ctx->nl.msg.msg_iovlen	= 1;

	ret = sendmsg(ctx->nl.fd, &ctx->nl.msg, 0);
	if (ret == -1) {
		APP_ERR("Failed to send message to kernel\n");
		return -1;
	}

	return 0;
}

int main(int argc, char** argv)
{
	int ret;
	ir_pkt pkt;
	daytime_ctx_t *app_ctx;
	ret = app_init(&app_ctx, "./config.cfg");

#if 0
	ret = init_netlink(app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to initialize netlink\n");
		exit(EXIT_FAILURE);
	}
#endif
	ret = pthread_create(&app_ctx->cam_thread, NULL, camera_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->disp_thread, NULL, display_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->usb_thread, NULL, usb_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->icd_thread, NULL, icd_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}

	ret = pthread_create(&app_ctx->cmd_thread, NULL, cmd_loop, (void*)app_ctx);
	if (ret < 0) {
		APP_ERR("Failed to create thread\n");
		exit(EXIT_FAILURE);
	}
  
	ret = pthread_join(app_ctx->cam_thread, NULL);

	exit(EXIT_SUCCESS);

	return 0;
}


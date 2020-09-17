#include "utils.h"

int write_jpeg(const char *filename, uint8_t *raw_data, 
					int w, int h, int bpp, 
					int pixelformat, int quality)
{
	int ret;
	uint8_t *jpeg_data;
	uint64_t jpeg_size = 0;
	FILE *fp;

	tjhandle _jpegCompressor = tjInitCompress();
	if (_jpegCompressor == NULL) {
		ERR_MSG("Failed to init turbojpeg compressor\n");
		return -1;
	}
	

	if(w<1500) //ir image
	{
		ret = tjCompress2(_jpegCompressor, raw_data, w, 0, h, pixelformat,
							&jpeg_data, &jpeg_size, TJSAMP_GRAY, quality, TJFLAG_FASTDCT);
	}
	else     //fusion image
	{
		ret = tjCompress2(_jpegCompressor, raw_data, w, 0, h, pixelformat,
							&jpeg_data, &jpeg_size, TJSAMP_444, quality, TJFLAG_FASTDCT);
	
	}

	if (ret != 0) {
		ERR_MSG("Failed to encode image\n");
		return -1;
	}
	
	
	


	fp = fopen(filename, "wb");
	if (fp == NULL) {
		ERR_MSG("Failed to open file\n");
		return -1;
	}

	ret = fwrite((void*)jpeg_data, jpeg_size, 1, fp);
	if (ret != jpeg_size) {
		ERR_MSG("Failed to write file : %d\n", ret);
	}

	fflush(fp);
	fclose(fp);

	tjDestroy(_jpegCompressor);

	tjFree(jpeg_data);

	return 0;
}

int write_jpeg_yuv(const char *filename, uint8_t *raw_data, 
					int w, int h, int bpp, 
					int pixelformat, int quality)
{
	int ret;
	uint8_t *jpeg_data;
	uint64_t jpeg_size = 0;
	FILE *fp;

	tjhandle _jpegCompressor = tjInitCompress();
	if (_jpegCompressor == NULL) {
		ERR_MSG("Failed to init turbojpeg compressor\n");
		return -1;
	}

	ret = tjCompressFromYUV(_jpegCompressor, raw_data, w, 2, h, pixelformat,
							&jpeg_data, &jpeg_size, quality, TJFLAG_FASTDCT);
	if (ret != 0) {
		ERR_MSG("Failed to encode image\n");
		return -1;
	}

	fp = fopen(filename, "wb");
	if (fp == NULL) {
		ERR_MSG("Failed to open file\n");
		return -1;
	}

	ret = fwrite((void*)jpeg_data, jpeg_size, 1, fp);
	if (ret != jpeg_size) {
		ERR_MSG("Failed to write file : %d\n", ret);
	}

	fflush(fp);
	fclose(fp);

	tjDestroy(_jpegCompressor);

	tjFree(jpeg_data);

	return 0;
}


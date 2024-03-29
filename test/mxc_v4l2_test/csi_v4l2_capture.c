/*
 * Copyright 2009-2014 Freescale Semiconductor, Inc. All rights reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*
 * @file csi_v4l2_capture.c
 *
 * @brief Mx25 Video For Linux 2 driver test application
 *
 */

#ifdef __cplusplus
extern "C"{
#endif

/*=======================================================================
                                        INCLUDE FILES
=======================================================================*/
/* Standard Include Files */
#include <errno.h>

/* Verification Test Environment Include Files */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <asm/types.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>

#ifdef	GET_CONTI_PHY_MEM_VIA_PXP_LIB
#include "pxp_lib.h"
#endif

sigset_t sigset;
int quitflag;

#define TEST_BUFFER_NUM 3
#define MAX_V4L2_DEVICE_NR     64

struct testbuffer
{
        unsigned char *start;
        size_t offset;
        unsigned int length;
};

struct testbuffer buffers[TEST_BUFFER_NUM];
#ifdef	GET_CONTI_PHY_MEM_VIA_PXP_LIB
struct pxp_mem_desc mem[TEST_BUFFER_NUM];
#endif
int g_in_width = 640;
int g_in_height = 480;
int g_out_width = 640;
int g_out_height = 480;
int g_cap_fmt = V4L2_PIX_FMT_RGB565;
int g_capture_mode = 0;
int g_timeout = 10;
int g_camera_framerate = 30;	/* 30 fps */
int g_hflip = 0;
int g_vflip = 0;
int g_rotation = 0;
int g_loop = 0;
int g_mem_type = V4L2_MEMORY_MMAP;
int g_frame_size;
char g_v4l_device[100] = "/dev/video0";
char g_saved_filename[100] = "/1.yuv";
int  g_saved_to_file = 0;

int start_capturing(int fd_v4l)
{
	unsigned int i;
	struct v4l2_buffer buf;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers req;

	memset(&req, 0, sizeof (req));
	req.count = TEST_BUFFER_NUM;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = g_mem_type;

	if (ioctl(fd_v4l, VIDIOC_REQBUFS, &req) < 0) {
		printf("VIDIOC_REQBUFS failed\n");
		return -1;
	}

	for (i = 0; i < TEST_BUFFER_NUM; i++) {
		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = g_mem_type;
		buf.index = i;
		if (g_mem_type == V4L2_MEMORY_USERPTR) {
			buf.length = buffers[i].length;
			buf.m.userptr = (unsigned long) buffers[i].offset;
			buf.length = buffers[i].length;
		}

		if (ioctl(fd_v4l, VIDIOC_QUERYBUF, &buf) < 0) {
			printf("VIDIOC_QUERYBUF error\n");
			return -1;
		}

		if (g_mem_type == V4L2_MEMORY_MMAP) {
			buffers[i].length = buf.length;
			buffers[i].offset = (size_t) buf.m.offset;
			buffers[i].start = mmap(NULL, buffers[i].length,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				fd_v4l, buffers[i].offset);
			memset(buffers[i].start, 0xFF, buffers[i].length);
		}
	}

	for (i = 0; i < TEST_BUFFER_NUM; i++)
	{
		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = g_mem_type;
		buf.index = i;
		buf.m.offset = buffers[i].offset;

		if (ioctl (fd_v4l, VIDIOC_QBUF, &buf) < 0) {
			printf("VIDIOC_QBUF error\n");
			return -1;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl (fd_v4l, VIDIOC_STREAMON, &type) < 0) {
		printf("VIDIOC_STREAMON error\n");
		return -1;
	}

	return 0;
}

int stop_capturing(int fd_v4l)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return ioctl (fd_v4l, VIDIOC_STREAMOFF, &type);
}

static int find_video_device(void)
{
	struct v4l2_capability cap;
	int fd_v4l;

	if ((fd_v4l = open(g_v4l_device, O_RDWR, 0)) < 0) {
		printf("unable to open %s for capture, continue searching "
			"device.\n", g_v4l_device);
	}
	if (ioctl(fd_v4l, VIDIOC_QUERYCAP, &cap) == 0) {
		if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
			printf("Found v4l2 capture device %s.\n", g_v4l_device);
			return fd_v4l;
		}
	} else {
		close(fd_v4l);
	}

	return fd_v4l;
}

static void print_pixelformat(char *prefix, int val)
{
	printf("%s: %c%c%c%c\n", prefix ? prefix : "pixelformat",
					val & 0xff,
					(val >> 8) & 0xff,
					(val >> 16) & 0xff,
					(val >> 24) & 0xff);
}

int v4l_capture_setup(void)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_fmtdesc fmtdesc;
	struct v4l2_frmivalenum frmival;
	struct v4l2_crop crop;
	struct v4l2_control vc;
	int fd_v4l = 0;

	if ((fd_v4l = find_video_device()) < 0)
	{
		printf("Unable to open v4l2 capture device.\n");
		return 0;
	}

	fmtdesc.index = 0;
	while (ioctl(fd_v4l, VIDIOC_ENUM_FMT, &fmtdesc) >= 0) {
		print_pixelformat("pixelformat (output by camera)",
				fmtdesc.pixelformat);
		frmival.index = 0;
		frmival.pixel_format = fmtdesc.pixelformat;
		frmival.width = g_in_width;
		frmival.height = g_in_height;
		while (ioctl(fd_v4l, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) >= 0)
		{
			printf("%.3f fps\n", 1.0 * frmival.discrete.denominator
				/ frmival.discrete.numerator);
			frmival.index++;
		}
		fmtdesc.index++;
	}

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.capturemode = g_capture_mode;
	parm.parm.capture.timeperframe.denominator = g_camera_framerate;
	parm.parm.capture.timeperframe.numerator = 1;
	if (ioctl(fd_v4l, VIDIOC_S_PARM, &parm) < 0)
	{
		printf("VIDIOC_S_PARM failed\n");
		return -1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.pixelformat = g_cap_fmt;
	print_pixelformat("pixelformat (output by v4l)", fmt.fmt.pix.pixelformat);
	fmt.fmt.pix.width = g_out_width;
	fmt.fmt.pix.height = g_out_height;
	if (ioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0)
	{
		printf("set format failed\n");
		return 0;
	}

	if (ioctl(fd_v4l, VIDIOC_G_FMT, &fmt) < 0)
	{
		printf("get format failed\n");
		return -1;
	} else {
		printf("\t Width = %d", fmt.fmt.pix.width);
		printf("\t Height = %d", fmt.fmt.pix.height);
		printf("\t Image size = %d\n", fmt.fmt.pix.sizeimage);
	}
	g_frame_size = fmt.fmt.pix.sizeimage;

	memset(&crop, 0, sizeof(crop));
	crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	crop.c.top = 0;
	crop.c.left = 0;
	crop.c.width = g_in_width;
	crop.c.height = g_in_height;
	if (ioctl(fd_v4l, VIDIOC_S_CROP, &crop) < 0) {
		printf("set crop failed.\n");
		return -1;
	}
	memset(&crop, 0, sizeof(crop));
	crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd_v4l, VIDIOC_G_CROP, &crop) < 0) {
		printf("get crop failed.\n");
		return -1;
	} else
		printf("crop.c.l/t/w/h = %d/%d/%d/%d\n", crop.c.left,
			crop.c.top, crop.c.width, crop.c.height);

	if (g_hflip)
		vc.value = 1;
	else
		vc.value = 0;
	vc.id = V4L2_CID_HFLIP;
	if (ioctl(fd_v4l, VIDIOC_S_CTRL, &vc) < 0) {
		printf("s_ctrl failed.\n");
		return -1;
	}

	if (g_vflip)
		vc.value = 1;
	else
		vc.value = 0;
	vc.id = V4L2_CID_VFLIP;
	if (ioctl(fd_v4l, VIDIOC_S_CTRL, &vc) < 0) {
		printf("s_ctrl failed.\n");
		return -1;
	}

	vc.id = V4L2_CID_PRIVATE_BASE;
	vc.value = g_rotation;
	if (ioctl(fd_v4l, VIDIOC_S_CTRL, &vc) < 0) {
		printf("s_ctrl failed.\n");
		return -1;
	}

	return fd_v4l;
}

int v4l_capture_test(int fd_v4l)
{
	struct fb_var_screeninfo var;
	struct v4l2_buffer buf;
	char fb_device[100] = "/dev/fb0";
	int fd_fb = 0;
	int frame_num = 0, i, fb0_size;
	unsigned char *fb0;
	struct timeval tv1, tv2;
	int j = 0;
	int out_w = 0, out_h = 0;
	FILE * fd_y_file = 0;

	if (g_saved_to_file == 1) {
		if ((fd_y_file = fopen(g_saved_filename, "wb")) == NULL) {
			printf("Unable to create y frame recording file\n");
			return -1;
		}
		goto loop; /* skip the fb display */
	}

	if ((fd_fb = open(fb_device, O_RDWR )) < 0) {
		printf("Unable to open frame buffer\n");
		goto FAIL;
	}

	if (ioctl(fd_fb, FBIOGET_VSCREENINFO, &var) < 0) {
		printf("FBIOPUT_VSCREENINFO failed\n");
		goto FAIL;
	}

	if (g_rotation == 90 || g_rotation == 270) {
		out_w = g_out_height;
		out_h = g_out_width;
	} else {
		out_w = g_out_width;
		out_h = g_out_height;
	}

	var.xres_virtual = var.xres;
	var.yres_virtual = 2 * var.yres;

	if (out_w > var.xres || out_h > var.yres) {
		printf("The output width or height is exceeding the resolution"
			" of the screen.\n"
			"wxh: %dx%d, screen wxh: %dx%d\n", out_w, out_h,
			var.xres, var.yres);
		goto FAIL;
	}

	if (ioctl(fd_fb, FBIOPUT_VSCREENINFO, &var) < 0) {
		printf("FBIOPUT_VSCREENINFO failed\n");
		goto FAIL;
	}

	 /* Map the device to memory*/
	fb0_size = var.xres * var.yres_virtual * var.bits_per_pixel / 8;
	fb0 = (unsigned char *)mmap(0, fb0_size,
					PROT_READ | PROT_WRITE, MAP_SHARED, fd_fb, 0);
	if ((int)fb0 == -1) {
		printf("Error: failed to map framebuffer device 0 to memory.\n");
		goto FAIL;
	}

loop:
	if (start_capturing(fd_v4l) < 0) {
		printf("start_capturing failed\n");
		goto FAIL;
	}

	gettimeofday(&tv1, NULL);
	do {
		memset(&buf, 0, sizeof (buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = g_mem_type;
		if (ioctl (fd_v4l, VIDIOC_DQBUF, &buf) < 0) {
			printf("VIDIOC_DQBUF failed.\n");
			break;
		}

		if (fd_y_file)
	                fwrite(buffers[buf.index].start, g_frame_size, 1, fd_y_file);

		else
		for (i = 0; i < TEST_BUFFER_NUM; i++) {
			if (buf.m.userptr == buffers[i].offset) {
				if (var.yoffset == 0) {
					var.yoffset += var.yres;
					for (j = 0; j < out_h; j++) {
						memcpy(fb0 + var.xres * var.yres * 2  + j * var.xres * 2,
							buffers[i].start + j * out_w * 2,
							out_w * 2);
					}
					if (ioctl(fd_fb, FBIOPAN_DISPLAY, &var) < 0) {
						printf("FBIOPAN_DISPLAY failed\n");
						break;
					}
				} else {
					var.yoffset = 0;
					for (j = 0; j < out_h; j++) {
						memcpy(fb0 + j * var.xres * 2,
							buffers[i].start + j * out_w * 2,
							out_w * 2);
					}
					if (ioctl(fd_fb, FBIOPAN_DISPLAY, &var) < 0) {
						printf("FBIOPAN_DISPLAY failed\n");
						break;
					}
				}

				break;
			}
		}

		if (ioctl (fd_v4l, VIDIOC_QBUF, &buf) < 0) {
			printf("VIDIOC_QBUF failed\n");
			break;
		}

		frame_num += 1;
		gettimeofday(&tv2, NULL);
	} while((tv2.tv_sec - tv1.tv_sec < g_timeout) && !quitflag);

	/* Make sure pan display offset is zero before capture is stopped */
	if (g_saved_to_file == 0)
		if (var.yoffset) {
			var.yoffset = 0;
			if (ioctl(fd_fb, FBIOPAN_DISPLAY, &var) < 0)
				printf("FBIOPAN_DISPLAY failed\n");
		}

	if (fd_y_file)
		fclose(fd_y_file);

	if (stop_capturing(fd_v4l) < 0)
		printf("stop_capturing failed\n");

	if (g_loop > 0 && !quitflag) {
		printf("loop %d done!\n", g_loop);
		g_loop--;
		goto loop;
	}

	if (g_saved_to_file == 0) {
		munmap((void *)fd_fb, fb0_size);
		close(fd_fb);
	}

	close(fd_v4l);
	return 0;
FAIL:
	if (g_saved_to_file == 0)
		close(fd_fb);
	close(fd_v4l);
	return -1;
}

void print_help(void)
{
	printf("CSI Video4Linux capture Device Test\n"
		"Syntax: ./csi_v4l2_capture -iw <capture input width>\n"
		" -ih <capture input height>\n"
		" -ow <capture output width>\n"
		" -oh <capture output height>\n"
		" -hf <horizontal flip>\n"
		" -vf <vertical flip>\n"
		" -m <capture mode, 0-640x480, 1-320x240, etc>\n" \
		" -r <rotation 0, 90, 180, 270>\n"
		" -t <time> -fr <framerate>\n"
		" -d <camera select, /dev/video0, /dev/video1> \n" \
		" -f <format> \n"
		" -of <save_to_file>\n"
		" -loop <times>\n"
#ifdef	GET_CONTI_PHY_MEM_VIA_PXP_LIB
		" [-u if defined, means use userp, otherwise mmap]\n"
#endif
	);
}

int process_cmdline(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-m") == 0) {
			g_capture_mode = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-iw") == 0) {
			g_in_width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-ih") == 0) {
			g_in_height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-ow") == 0) {
			g_out_width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-oh") == 0) {
			g_out_height = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "-d") == 0) {
			strcpy(g_v4l_device, argv[++i]);
		} else if (strcmp(argv[i], "-t") == 0) {
			g_timeout = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-hf") == 0) {
			g_hflip = 1;
		} else if (strcmp(argv[i], "-vf") == 0) {
			g_vflip = 1;
		} else if (strcmp(argv[i], "-r") == 0) {
			g_rotation = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-loop") == 0) {
			g_loop = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-help") == 0) {
			print_help();
			return -1;
		} else if (strcmp(argv[i], "-fr") == 0) {
			g_camera_framerate = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-of") == 0) {
			strcpy(g_saved_filename, argv[++i]);
			g_saved_to_file = 1;
                } else if (strcmp(argv[i], "-f") == 0) {
                        i++;
                        g_cap_fmt = v4l2_fourcc(argv[i][0], argv[i][1],argv[i][2],argv[i][3]);

                        if ((g_cap_fmt != V4L2_PIX_FMT_RGB565) &&
			     (g_cap_fmt != V4L2_PIX_FMT_NV12) &&
			     (g_cap_fmt != V4L2_PIX_FMT_UYVY) &&
			     (g_cap_fmt != V4L2_PIX_FMT_YUYV))
                        {
                                return -1;
                        }
#ifdef	GET_CONTI_PHY_MEM_VIA_PXP_LIB
		} else if (strcmp(argv[i], "-u") == 0) {
			g_mem_type = V4L2_MEMORY_USERPTR;
#endif
		} else {
			print_help();
			return -1;
		}
	}

	return 0;
}

static int signal_thread(void *arg)
{
	int sig;

	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	while (1) {
		sigwait(&sigset, &sig);
		if (sig == SIGINT) {
			printf("Ctrl-C received. Exiting.\n");
		} else {
			printf("Unknown signal. Still exiting\n");
		}
		quitflag = 1;
		break;
	}

	return 0;
}

#ifdef	GET_CONTI_PHY_MEM_VIA_PXP_LIB
void memfree(int buf_size, int buf_cnt)
{
	int i;
        unsigned int page_size;

	page_size = getpagesize();
	buf_size = (buf_size + page_size - 1) & ~(page_size - 1);

	for (i = 0; i < buf_cnt; i++) {
		if (buffers[i].start) {
			pxp_put_mem(&mem[i]);
			buffers[i].start = NULL;
		}
	}
	pxp_uninit();
}

int memalloc(int buf_size, int buf_cnt)
{
	int i, ret;
        unsigned int page_size;

	ret = pxp_init();
	if (ret < 0) {
		printf("pxp init err\n");
		return -1;
	}

	for (i = 0; i < buf_cnt; i++) {
		page_size = getpagesize();
		buf_size = (buf_size + page_size - 1) & ~(page_size - 1);
		buffers[i].length = mem[i].size = buf_size;
		ret = pxp_get_mem(&mem[i]);
		if (ret < 0) {
			printf("Get PHY memory fail\n");
			ret = -1;
			goto err;
		}
		buffers[i].offset = mem[i].phys_addr;
		buffers[i].start = (unsigned char *)mem[i].virt_uaddr;
		if (!buffers[i].start) {
			printf("mmap fail\n");
			ret = -1;
			goto err;
		}
		printf("USRP: alloc bufs offset 0x%x size %d\n",
				buffers[i].offset, buf_size);
	}

	return ret;
err:
	memfree(buf_size, buf_cnt);
	return ret;
}
#else
void memfree(int buf_size, int buf_cnt) {}
int memalloc(int buf_size, int buf_cnt) { return 0; }
#endif

int main(int argc, char **argv)
{
	int fd_v4l;
	quitflag = 0;

	pthread_t sigtid;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);
	pthread_create(&sigtid, NULL, (void *)&signal_thread, NULL);

	if (process_cmdline(argc, argv) < 0) {
		return -1;
	}

	fd_v4l = v4l_capture_setup();
	if (g_mem_type == V4L2_MEMORY_USERPTR)
		if (memalloc(g_frame_size, TEST_BUFFER_NUM) < 0) {
			close(fd_v4l);
		}

	v4l_capture_test(fd_v4l);

	if (g_mem_type == V4L2_MEMORY_USERPTR)
		memfree(g_frame_size, TEST_BUFFER_NUM);

	return 0;
}


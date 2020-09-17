#include "uart_helper.h"

int uart_init_helper(const char* device, speed_t baudrate, sa_handler_t hnd, int *fd)
{
	int uart_fd, ret;
	struct termios termattr;
	struct sigaction saio;

	uart_fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
	if (uart_fd == -1) {
		UART_ERR("Unable to open %s : %s\n", device, strerror(errno));
		return -1;
	}

	saio.sa_handler = hnd;
	saio.sa_flags = 0;
	saio.sa_restorer = NULL;
	sigaction(SIGIO, &saio, NULL);

	fcntl(uart_fd, F_SETFL, FNDELAY);
	fcntl(uart_fd, F_SETOWN, getpid());

	memset(&termattr, 0x00, sizeof(termattr));
	tcgetattr(uart_fd, &termattr);

	cfsetispeed(&termattr, baudrate);
	cfsetospeed(&termattr, baudrate);

	termattr.c_cflag &= ~PARENB;
	termattr.c_cflag &= ~CSTOPB;
	termattr.c_cflag &= ~CSIZE;
	termattr.c_cflag |= CS8;
	termattr.c_cflag &= ~CRTSCTS;
	termattr.c_cflag |= (CLOCAL | CREAD);
	termattr.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	termattr.c_iflag &= ~(IXON | IXOFF | IXANY);

	termattr.c_oflag &= ~OPOST;

	tcsetattr(uart_fd, TCSANOW, &termattr);
	tcflush(uart_fd, TCIOFLUSH);

	*fd = uart_fd;

	return 0;
}

int uart_deinit_helper(int fd)
{
	close(fd);
}

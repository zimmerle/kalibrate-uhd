#include <netinet/tcp.h>
#include <sys/types.h>	   
#include <sys/socket.h>	 
#include <arpa/inet.h>   
#include <netinet/in.h>   
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "socket.h" 

int make_connection(char *ipaddress, uint16_t port)
{
	int sockfd, flag, rc;
	struct sockaddr_in servaddr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	/* Disable Nagle (Gierth) */
	flag = 1;
	rc = setsockopt(sockfd,	IPPROTO_TCP,
				TCP_NODELAY, (char *) &flag, sizeof(int));
	if (rc < 0) {
		fprintf(stderr, "setsockopt error: %s\n", strerror(errno));
	}

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	inet_pton(AF_INET, ipaddress, &servaddr.sin_addr);
 
	rc = connect(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr));
	if (rc < 0) {
		fprintf(stderr, "Socket connect failed: %s\n", strerror(errno));
		return -1;
	}

	return sockfd;
}   

int setup_server(uint16_t port)
{
	int listenfd, connfd, rc;
	socklen_t clilen;

	struct sockaddr_in cliaddr, servaddr;
	listenfd = socket(AF_INET, SOCK_STREAM, 0);

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htons(INADDR_ANY);
	servaddr.sin_port = htons(port);

	bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr));

	rc = listen(listenfd, 1024);
	if (rc < 0) {
		fprintf(stderr, "Socket listen failed: %s\n", strerror(errno));
		return -1;
	}

	clilen = sizeof(cliaddr);
	connfd = accept(listenfd, (struct sockaddr *) &cliaddr, &clilen);
	if (connfd < 0) {
		fprintf(stderr, "Socket accept failed: %s\n", strerror(errno));
		return -1;
	}

	close(listenfd);

	return connfd;
}

ssize_t writen(int fd, const char *vptr, size_t n)
{
	size_t nleft;
	ssize_t nwritten;
	const char *ptr;

	ptr = vptr;
	nleft = n;
	while (nleft > 0) {
		if ((nwritten = write(fd, ptr, nleft)) <= 0) {
			if (nwritten < 0 && errno == EINTR)
				nwritten = 0;
			else
				return -1;
		}

		nleft -= nwritten;
		ptr += nwritten;
	}

	return (n);
}

ssize_t readn(int fd, char *vptr, size_t n)
{
	size_t  nleft;
	ssize_t nread;
	char   *ptr;

	ptr = vptr;
	nleft = n;
	while (nleft > 0) {
		if ((nread = read(fd, ptr, nleft)) < 0) {
			if (errno == EINTR)
				nread = 0;
			else
				return (-1);
		} else if (nread == 0) {
			break;
		}

		nleft -= nread;
		ptr += nread;
	}

	return (n - nleft);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#define MAX_CPUS 16

#define MAX_URL_LEN 128
#define HTTP_HEADER_LEN 1024

#define MAX_IP_STR_LEN 16

#define BUF_SIZE (2*1024)

/*----------------------------------------------------------------------------*/
static pthread_t app_thread;
/*----------------------------------------------------------------------------*/
static char host[MAX_IP_STR_LEN + 1] = {'\0'};
static char url[MAX_URL_LEN + 1] = {'\0'};
static in_addr_t daddr;
static in_port_t dport;
static in_addr_t saddr;
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

void *isDone(void *arg)
{
	mctx_t mctx;
	int core = *(int *)arg;
	struct sockaddr_in addr;
	int sockid;
	char request[HTTP_HEADER_LEN], buf[BUF_SIZE];
	int ret, len;

	mtcp_core_affinitize(core);
	mctx = mtcp_create_context(core);
	sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = daddr;
	addr.sin_port = dport;

	ret = mtcp_connect(mctx, sockid, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (ret < 0) printf("E1\n");

	
	snprintf(request, HTTP_HEADER_LEN, "GET %s HTTP/1.0\r\n"
			"User-Agent: Wget/1.12 (linux-gnu)\r\n"
			"Accept: */*\r\n"
			"Host: %s\r\n"
			"Connection: Close\r\n\r\n",
			url, host);
	len = strlen(request);

	printf("Client Sending:\n%s\n",request);
	ret = mtcp_write(mctx, sockid, request, len);
	if (ret < len) printf("E2\n");
	sleep(2);

	mtcp_read(mctx, sockid, buf, BUF_SIZE);

	ret = printf("Download:\n%s\n", buf);

	return NULL;
}

/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv)
{
	struct mtcp_conf mcfg;
	int ret, core_limit, max_fds=2500;
	int coreN=0;

	char* slash_p = strchr(argv[1], '/');
	if (slash_p) {
		strncpy(host, argv[1], slash_p - argv[1]);
		strncpy(url, strchr(argv[1], '/'), MAX_URL_LEN);
	} else {
		strncpy(host, argv[1], MAX_IP_STR_LEN);
		strncpy(url, "/", 2);
	}

	daddr = inet_addr(host);
	dport = htons(80);
	saddr = INADDR_ANY;

	core_limit = 1;
	mtcp_getconf(&mcfg);
	mcfg.num_cores = core_limit;
	mtcp_setconf(&mcfg);

	ret = mtcp_init("epwget.conf");
	if (ret) {
		exit(EXIT_FAILURE);
	}

	mtcp_getconf(&mcfg);
	mcfg.max_concurrency = max_fds;
	mcfg.max_num_buffers = max_fds;
	mtcp_setconf(&mcfg);

	pthread_create(&app_thread, NULL, isDone, (void *)&coreN);
	pthread_join(app_thread, NULL);

	mtcp_destroy();
	return 0;
}
/*----------------------------------------------------------------------------*/

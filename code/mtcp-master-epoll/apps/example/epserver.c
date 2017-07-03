#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"
#include "tcp_app_buffer.h"

#define MAX_SOCKETS 20000
#define MAX_WORKERS 1

#define MAX_FLOW_NUM  (12000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_CPUS 16
#define MAX_FILES 30

#define NAME_LIMIT 128
#define FULLNAME_LIMIT 256

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

/*----------------------------------------------------------------------------*/
struct file_cache
{
	char name[NAME_LIMIT];
	char fullname[FULLNAME_LIMIT];
	uint64_t size;
	char *file;
};
/*----------------------------------------------------------------------------*/
struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[NAME_LIMIT];				// file name
	long int fsize;					// file size
};
/*----------------------------------------------------------------------------*/
struct thread_context
{
	mctx_t mctx;
	mctx_t mctx_master;
	int ep;
};

struct worker_info{
	int core;
	int core_master;
	int w_id;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS], done_thread;
static int done[MAX_CPUS];
static char *conf_file = NULL;
/*----------------------------------------------------------------------------*/
const char *www_main;
static struct file_cache fcache[MAX_FILES];
static int nfiles;
/*----------------------------------------------------------------------------*/
static int finished;
/*----------------------------------------------------------------------------*/

app_queue_t m_app_queue[9];
struct server_vars *svars;
struct thread_context *ctx_master;
int g_wcore[MAX_SOCKETS];
long long g_total_sent[8]={0};
long g_total_msgs[8]={0};
static char *
StatusCodeToString(int scode)
{
	switch (scode) {
		case 200:
			return "OK";
			break;

		case 404:
			return "Not Found";
			break;
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->done = 0;
	sv->rspheader_sent = 0;
	sv->keep_alive = 0;
}
/*----------------------------------------------------------------------------*/
void 
CloseConnection(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
	mtcp_close(ctx->mctx, sockid);
}
/*----------------------------------------------------------------------------*/
static int 
SendUntilAvailable(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int ret;
	int sent;
	int len;

	if (sv->done || !sv->rspheader_sent) {
		return 0;
	}

	sent = 0;
	ret = 1;
	while (ret>0) {
		len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
		if (len <= 0) {
			break;
		}
		printf("[epserver.c SendDataUntilAvailable] Data Sending | Socket: %d | Worker-id: %d\n", sockid, g_wcore[sockid]);
		ret = mtcp_write(ctx->mctx, sockid, fcache[sv->fidx].file + sv->total_sent, len);
		//if (ret <= 0) {
		//	printf("Write Error\n");
			
		//}
		sent += ret;
		if(ret>0)g_total_sent[g_wcore[sockid]]+=ret;
		sv->total_sent += ret;
	}

	if (sv->total_sent >= fcache[sv->fidx].size) {
		sv->done = TRUE;
		finished++;

		if (sv->keep_alive) {
			/* if keep-alive connection, wait for the incoming request */
			//ev.events = MTCP_EPOLLIN;
			//ev.data.sockid = sockid;
			//mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

			CleanServerVariable(sv);
		} else {
			/* else, close connection */
			//CloseConnection(ctx, sockid, sv);
		}
	}

	return sent;
}
/*----------------------------------------------------------------------------*/
static int 
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	struct mtcp_epoll_event ev;
	char buf[HTTP_HEADER_LEN];
	char url[URL_LEN];
	char response[HTTP_HEADER_LEN];
	int scode;						// status code
	time_t t_now;
	char t_str[128];
	char keepalive_str[128];
	int rd;
	int i;
	int len;
	int sent;

	/* HTTP request handling */
	rd = mtcp_read(ctx->mctx, sockid, buf, HTTP_HEADER_LEN);
	if (rd <= 0) {
		return rd;
	}
	printf("[epserver.c HandleReadEvent] Data Read | Socket: %d | Worker-id: %d\nBuffer:\n%s", sockid, g_wcore[sockid], buf);


	memcpy(sv->request + sv->recv_len, 
			(char *)buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
	sv->recv_len += rd;
	//sv->request[rd] = '\0';
	//fprintf(stderr, "HTTP Request: \n%s", request);
	sv->request_len = find_http_header(sv->request, sv->recv_len);
	if (sv->request_len <= 0) {
		TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
				"read bytes: %d, recv_len: %d, "
				"request_len: %d, strlen: %ld, request: \n%s\n", 
				sockid, rd, sv->recv_len, 
				sv->request_len, strlen(sv->request), sv->request);
		return rd;
	}

	http_get_url(sv->request, sv->request_len, url, URL_LEN);
	TRACE_APP("Socket %d URL: %s\n", sockid, url);
	sprintf(sv->fname, "%s%s", www_main, url);
	TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);

	sv->keep_alive = FALSE;
	if (http_header_str_val(sv->request, "Connection: ", 
				strlen("Connection: "), keepalive_str, 128)) {	
		if (strstr(keepalive_str, "Keep-Alive")) {
			sv->keep_alive = TRUE;
		} else if (strstr(keepalive_str, "Close")) {
			sv->keep_alive = FALSE;
		}
	}

	/* Find file in cache */
	scode = 404;
	for (i = 0; i < nfiles; i++) {
		if (strcmp(sv->fname, fcache[i].fullname) == 0) {
			sv->fsize = fcache[i].size;
			sv->fidx = i;
			scode = 200;
			break;
		}
	}
	TRACE_APP("Socket %d File size: %ld (%ldMB)\n", 
			sockid, sv->fsize, sv->fsize / 1024 / 1024);

	/* Response header handling */
	time(&t_now);
	strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
	if (sv->keep_alive)
		sprintf(keepalive_str, "Keep-Alive");
	else
		sprintf(keepalive_str, "Close");

	sprintf(response, "HTTP/1.1 %d %s\r\n"
			"Date: %s\r\n"
			"Server: Webserver on Middlebox TCP (Ubuntu)\r\n"
			"Content-Length: %ld\r\n"
			"Connection: %s\r\n\r\n", 
			scode, StatusCodeToString(scode), t_str, sv->fsize, keepalive_str);
	len = strlen(response);
	TRACE_APP("Socket %d HTTP Response: \n%s", sockid, response);
	sent = mtcp_write(ctx->mctx, sockid, response, len);
	TRACE_APP("Socket %d Sent response header: try: %d, sent: %d\n", 
			sockid, len, sent);
	assert(sent == len);
	g_total_sent[g_wcore[sockid]]+=sent;
	sv->rspheader_sent = TRUE;

	ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
	ev.data.sockid = sockid;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

	SendUntilAvailable(ctx, sockid, sv);

	return rd;
}
/*----------------------------------------------------------------------------*/
int 
AcceptConnection(struct thread_context *ctx, int listener, int worker_id)
{
	mctx_t mctx = ctx->mctx;
	struct server_vars *sv;
	struct mtcp_epoll_event ev;
	int c, w_id;

	w_id = worker_id;

	c = mtcp_accept(mctx, listener, NULL, NULL, &w_id);
	printf("[epserver.c AcceptConnection] Connection Accepted | Socket: %d | Worker-id: %d\n", c, w_id);
	if(c>=0) g_wcore[c]=w_id;

	//printf("[epserver.c mtcp_accept] %d\n", w_id);
	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			return -1;
		}

		sv = &svars[c];	// Todo: make global
		CleanServerVariable(sv);
		ev.events = MTCP_EPOLLIN;
		ev.data.sockid = c;
		mtcp_setsock_nonblock(ctx->mctx, c);
		mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);

	} else {
		if (errno != EAGAIN) {
			printf("mtcp_accept() error %s\n", 
					strerror(errno));
		}
		return -1;
	}

	return w_id;
}
/*----------------------------------------------------------------------------*/

struct thread_context *
InitializeIOThread(int core, int core_master, int w_id)
{
	struct thread_context *ctx;

	/* affinitize application thread to a CPU core */
#if HT_SUPPORT
	mtcp_core_affinitize(core + (num_cores / 2));
#else
	mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}

	/* create mtcp context: this will spawn an mtcp thread */
	ctx->mctx = mtcp_create_context(core, core_master, MTCP_IO, w_id);
	if (!ctx->mctx) {
		TRACE_ERROR("Failed to create mtcp context!\n");
		free(ctx);
		return NULL;
	}

	/* allocate memory for server variables */

	return ctx;
}

struct thread_context *
InitializeProcessThread(int core, int cpu_bitset)
{
	struct thread_context *ctx;

	/* affinitize application thread to a CPU core */
#if HT_SUPPORT
	mtcp_core_affinitize(core + (num_cores / 2));
#else
	mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}

	/* create mtcp context: this will spawn an mtcp thread */
	ctx->mctx = mtcp_create_context(core, -1, MTCP_MASTER, -1);
	if (!ctx->mctx) {
		printf("Failed to create mtcp context!\n");
		free(ctx);
		return NULL;
	}

	/* create epoll descriptor */
	ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
	if (ctx->ep < 0) {
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		printf("Failed to create epoll descriptor!\n");
		return NULL;
	}

	/* allocate memory for server variables */
	svars = (struct server_vars *)
			calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!svars) {
		mtcp_close(ctx->mctx, ctx->ep);
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		TRACE_ERROR("Failed to create server_vars struct!\n");
		return NULL;
	}
	
	ctx_master = ctx;
	return ctx;
}
/*----------------------------------------------------------------------------*/
int 
CreateListeningSocket(struct thread_context *ctx)
{
	int listener;
	struct mtcp_epoll_event ev;
	struct sockaddr_in saddr;
	int ret;

	/* create socket and set it as nonblocking */
	listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}
	ret = mtcp_setsock_nonblock(ctx->mctx, listener);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		return -1;
	}

	/* bind to port 80 */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(80);
	ret = mtcp_bind(ctx->mctx, listener, 
			(struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		return -1;
	}

	/* listen (backlog: 4K) */
	ret = mtcp_listen(ctx->mctx, listener, 8192);
	if (ret < 0) {
		TRACE_ERROR("mtcp_listen() failed!\n");
		return -1;
	}
	
	/* wait for incoming accept events */
	ev.events = MTCP_EPOLLIN;
	ev.data.sockid = listener;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);
	
	return listener;
}
/*----------------------------------------------------------------------------*/
int done_time = FALSE;
void *DoneTime(void *arg){
	int i,x,ret;
	long long total_sent=0;

	printf("Waiting: ");
	ret=scanf("%d", &x);
	x=ret;
	sleep(20);
	
	done_time = TRUE;
	for(i = 0; i<=MAX_WORKERS; i++)
		total_sent+=g_total_sent[i];

	printf("[promod-all] Th: %f\n", (double)total_sent/(1024*1024)); 
	return NULL;
}

void *
RunIOThread(void *arg){
	struct worker_info* w_info = (struct worker_info*)arg;
	int core = w_info->core;
	int core_master = w_info->core_master;
	int w_id = w_info->w_id;

	struct thread_context *ctx;
	mctx_t mctx;

	struct app_info* w_app_info;

	/* initialization */
	ctx = InitializeIOThread(core, core_master, w_id);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		return NULL;
	}
	mctx = ctx->mctx;
	app_queue_t w_app_queue = m_app_queue[w_id];

	while (!done_time){
		while(1){
			w_app_info = AppDequeue(w_app_queue);
			if(w_app_info == NULL) continue;
			if (w_app_info->j_id == 0) {
				printf("[RunIO Thread] Read Event Dequeued | Worker-id: %d\n", w_id);
				HandleReadEvent(ctx, w_app_info->s_id,
						&svars[w_app_info->s_id]);
				g_total_msgs[core]++;

			} else if (w_app_info->j_id == 1) {
				struct server_vars *sv = &svars[w_app_info->s_id];
				if (sv->rspheader_sent) {
					SendUntilAvailable(ctx, w_app_info->s_id, sv);
					g_total_msgs[core]++;
				} else {
					TRACE_APP("Socket %d: Response header not sent yet.\n",
							w_app_info->s_id);
				}

			} else {
				assert(0);
			}
		}
	}

	/* destroy mtcp context: this will kill the mtcp thread */
	mtcp_destroy_context(mctx);
	pthread_exit(NULL);

	return NULL;
}


void *
RunServerThread(void *arg)
{
	int core = *(int *)arg;
	//int n_wid=MAX_WORKERS, w_id = 0;
	struct thread_context *ctx;
	mctx_t mctx;
	int listener;
	int cpu_bitset=0;
	int ep;
	struct mtcp_epoll_event *events;
	int nevents;
	int i, ret;
	int do_accept;
	
	struct app_info *m_app_info, *w_app_info;
	app_queue_t w_app_queue;
	int s_id;

	/* initialization */
	ctx = InitializeProcessThread(core, cpu_bitset);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		return NULL;
	}
	mctx = ctx->mctx;
	ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
		exit(-1);
	}

	listener = CreateListeningSocket(ctx);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
		exit(-1);
	}

	for(i=0;i<=MAX_WORKERS;i++)
		m_app_queue[i] = CreateAppQueue(10000);
	w_app_queue = m_app_queue[0];

	while (!done_time) {
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			if (errno != EINTR)
				perror("mtcp_epoll_wait");
			break;
		}

		do_accept = FALSE;
		for (i = 0; i < nevents; i++) {

			if (events[i].data.sockid == listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;

			} else if (events[i].events & MTCP_EPOLLERR) {
				int err;
				socklen_t len = sizeof(err);

				/* error on the connection */
				printf("[CPU %d] Error on socket %d\n", 
						core, events[i].data.sockid);
				if (mtcp_getsockopt(mctx, events[i].data.sockid, 
						SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
					if (err != ETIMEDOUT) {
						fprintf(stderr, "Error on socket %d: %s\n", 
								events[i].data.sockid, strerror(err));
					}
				} else {
					perror("mtcp_getsockopt");
				}	
			
				CloseConnection(ctx, events[i].data.sockid, 
						&svars[events[i].data.sockid]);

			} else if (events[i].events & MTCP_EPOLLIN) {
				s_id=events[i].data.sockid;

				m_app_info = (struct app_info*)malloc(sizeof(struct app_info));
				m_app_info->s_id = s_id;
				m_app_info->j_id = 0;

				printf("[RunServerThread epserver.c] Core %d | Enqueue Read Event | Socket: %d | To-Worker-id: %d\n", core, s_id, g_wcore[s_id]);
				AppEnqueue(m_app_queue[g_wcore[s_id]], m_app_info);

			} else if (events[i].events & MTCP_EPOLLOUT) {
				s_id=events[i].data.sockid;

				m_app_info = (struct app_info*)malloc(sizeof(struct app_info));
				m_app_info->s_id = s_id;
				m_app_info->j_id = 1;

				AppEnqueue(m_app_queue[g_wcore[s_id]], m_app_info);
		
			} else {
				assert(0);
			}
		}


		/* if do_accept flag is set, accept connections */
		if (do_accept) {
			while (1) {
				printf("[RunServerThread.c epserver.c] Core: %d | Accepting Connection\n", core);
				ret = AcceptConnection(ctx, listener, 0);
				//w_id++;
				if (ret < 0){
					break;
				}
			}
		}

		while(1){
			w_app_info = AppDequeue(w_app_queue);
			if(w_app_info == NULL) break;
			if (w_app_info->j_id == 0) {
				ret = HandleReadEvent(ctx, w_app_info->s_id,
						&svars[w_app_info->s_id]);
				g_total_msgs[0]++;
				if (ret == 0) {
					/* connection closed by remote host */
					CloseConnection(ctx, w_app_info->s_id,
							&svars[w_app_info->s_id]);
				} else if (ret < 0) {
					/* if not EAGAIN, it's an error */
					if (errno != EAGAIN) {
						CloseConnection(ctx, w_app_info->s_id,
								&svars[w_app_info->s_id]);
					}
				}

			} else if (w_app_info->j_id == 1) {
				struct server_vars *sv = &svars[w_app_info->s_id];
				if (sv->rspheader_sent) {
					SendUntilAvailable(ctx, w_app_info->s_id, sv);
					g_total_msgs[0]++;
				} else {
					TRACE_APP("Socket %d: Response header not sent yet.\n",
							w_app_info->s_id);
				}

			} else {
				assert(0);
			}
		}

	}

	/* destroy mtcp context: this will kill the mtcp thread */
	mtcp_destroy_context(mctx);
	pthread_exit(NULL);

	return NULL;
}
/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
	int i;

	for (i = 0; i < core_limit; i++) {
		if (app_thread[i] == pthread_self()) {
			//TRACE_INFO("Server thread %d got SIGINT\n", i);
			done[i] = TRUE;
		} else {
			if (!done[i]) {
				pthread_kill(app_thread[i], signum);
			}
		}
	}
}
/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name)
{
	TRACE_CONFIG("%s -p <path_to_www/> -f <mtcp_conf_file> "
		     "[-N num_cores] [-c <per-process core_id>] [-h]\n",
		     prog_name);
	exit(EXIT_SUCCESS);
}
/*----------------------------------------------------------------------------*/

int 
main(int argc, char **argv)
{
	DIR *dir;
	struct dirent *ent;
	int fd;
	int ret;
	uint64_t total_read;
	struct mtcp_conf mcfg;
	int cores[MAX_CPUS];
	int process_cpu;
	int i, o;

	struct worker_info *w_info;
	num_cores = GetNumCPUs();
	core_limit = num_cores;
	process_cpu = -1;
	dir = NULL;

	if (argc < 2) {
		TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
		return FALSE;
	}

	while (-1 != (o = getopt(argc, argv, "N:f:p:c:h"))) {
		switch (o) {
		case 'p':
			/* open the directory to serve */
			www_main = optarg;
			dir = opendir(www_main);
			if (!dir) {
				TRACE_CONFIG("Failed to open %s.\n", www_main);
				perror("opendir");
				return FALSE;
			}
			break;
		case 'N':
			core_limit = mystrtol(optarg, 10);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
					     "number of CPUs: %d\n", num_cores);
				return FALSE;
			}
			/** 
			 * it is important that core limit is set 
			 * before mtcp_init() is called. You can
			 * not set core_limit after mtcp_init()
			 */
			mtcp_getconf(&mcfg);
			mcfg.num_cores = core_limit;
			mtcp_setconf(&mcfg);
			break;
		case 'f':
			conf_file = optarg;
			break;
		case 'c':
			process_cpu = mystrtol(optarg, 10);
			if (process_cpu > core_limit) {
				TRACE_CONFIG("Starting CPU is way off limits!\n");
				return FALSE;
			}
			break;
		case 'h':
			printHelp(argv[0]);
			break;
		}
	}

	if (dir == NULL) {
		TRACE_CONFIG("You did not pass a valid www_path!\n");
		exit(EXIT_FAILURE);
	}

	nfiles = 0;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0)
			continue;
		else if (strcmp(ent->d_name, "..") == 0)
			continue;

		snprintf(fcache[nfiles].name, NAME_LIMIT, "%s", ent->d_name);
		snprintf(fcache[nfiles].fullname, FULLNAME_LIMIT, "%s/%s",
			 www_main, ent->d_name);
		fd = open(fcache[nfiles].fullname, O_RDONLY);
		if (fd < 0) {
			perror("open");
			continue;
		} else {
			fcache[nfiles].size = lseek64(fd, 0, SEEK_END);
			lseek64(fd, 0, SEEK_SET);
		}

		fcache[nfiles].file = (char *)malloc(fcache[nfiles].size);
		if (!fcache[nfiles].file) {
			TRACE_CONFIG("Failed to allocate memory for file %s\n", 
				     fcache[nfiles].name);
			perror("malloc");
			continue;
		}

		TRACE_INFO("Reading %s (%lu bytes)\n", 
				fcache[nfiles].name, fcache[nfiles].size);
		total_read = 0;
		while (1) {
			ret = read(fd, fcache[nfiles].file + total_read, 
					fcache[nfiles].size - total_read);
			if (ret < 0) {
				break;
			} else if (ret == 0) {
				break;
			}
			total_read += ret;
		}
		if (total_read < fcache[nfiles].size) {
			free(fcache[nfiles].file);
			continue;
		}
		close(fd);
		nfiles++;

		if (nfiles >= MAX_FILES)
			break;
	}

	finished = 0;

	/* initialize mtcp */
	if (conf_file == NULL) {
		TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
		exit(EXIT_FAILURE);
	}

	ret = mtcp_init(conf_file);
	if (ret) {
		TRACE_CONFIG("Failed to initialize mtcp\n");
		exit(EXIT_FAILURE);
	}

	/* register signal handler to mtcp */
	mtcp_register_signal(SIGINT, SignalHandler);

	printf("[epserver.c main]  Application initialization finished.\n");

	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		cores[i] = i;
		done[i] = FALSE;
		
		if(i==0){
			if (pthread_create(&app_thread[i], NULL, RunServerThread, (void *)&cores[i])) {
				perror("pthread_create");
				TRACE_CONFIG("Failed to create server thread.\n");
				exit(EXIT_FAILURE);
			}
			sleep(2);
			if (process_cpu != -1)
				break;

		} else {
			w_info = (struct worker_info*)malloc(sizeof(struct worker_info));
			w_info->core=i;
			w_info->core_master=0;
			w_info->w_id=i;
			pthread_create(&app_thread[i], NULL, RunIOThread, (void *)w_info);
		}
	}

	pthread_create(&done_thread, NULL, DoneTime, NULL);

	for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
		pthread_join(app_thread[i], NULL);

		if (process_cpu != -1)
			break;
	}

	pthread_join(done_thread, NULL);	
	
	mtcp_destroy();
	closedir(dir);
	return 0;
}
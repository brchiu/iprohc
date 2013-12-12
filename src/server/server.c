/*
This file is part of iprohc.

iprohc is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
any later version.

iprohc is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with iprohc.  If not, see <http://www.gnu.org/licenses/>.
*/

/* server.c -- Implements server side of the ROHC IP-IP tunnel
*/

#include "config.h"

#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <getopt.h>
#include <assert.h>
#include <gnutls/gnutls.h>
#include <gnutls/pkcs12.h>

#include "tun_helpers.h"
#include "client.h"
#include "messages.h"
#include "tls.h"
#include "server_config.h"
#include "rohc_tunnel.h"
#include "log.h"


/** Toggle to true to print clients stats at next event loop */
static bool clients_do_dump_stats = false;

int log_max_priority = LOG_INFO;
bool iprohc_log_stderr = true;


static size_t iprohc_get_ipv4_range_width(const uint32_t addr,
                                          const size_t netmasklen)
	__attribute__((warn_unused_result));


/*
 * Route function that will be threaded twice to route
 * from tun to fake_tun and from raw to fake_raw
*/
enum type_route { TUN, RAW };

struct route_args
{
	int fd;
	struct client *clients;
	size_t clients_max_nr;   /**< The maximum number of simultaneous clients */
	enum type_route type;
};

/* Thread that will be called to monitor tun or raw and route */
void * route(void*arg)
{
	/* Getting args */
	const struct route_args *const _arg = (struct route_args *) arg;
	int fd = _arg->fd;
	struct client *clients = _arg->clients;
	size_t clients_max_nr = _arg->clients_max_nr;
	enum type_route type = _arg->type;

	int i;
	int ret;
	size_t len;

	unsigned char buffer[TUNTAP_BUFSIZE];
	unsigned int buffer_len = TUNTAP_BUFSIZE;

	struct in_addr addr;

	uint32_t*src_ip;
	uint32_t*dest_ip;

	trace(LOG_INFO, "Initializing routing thread\n");

	while((ret = read(fd, buffer, buffer_len)))
	{
		if(ret < 0 || ret > buffer_len)
		{
			trace(LOG_ERR, "read failed: %s (%d)\n", strerror(errno), errno);
			return NULL;
		}
		len = ret;
		trace(LOG_DEBUG, "Read %zu bytes\n", len);

		/* Get packet destination IP if tun or source IP if raw */
		if(type == TUN)
		{
			dest_ip = (uint32_t*) &buffer[20];
			addr.s_addr = *dest_ip;
			trace(LOG_DEBUG, "Packet destination : %s\n", inet_ntoa(addr));
		}
		else
		{
			src_ip = (uint32_t*) &buffer[12];
			addr.s_addr = *src_ip;
			trace(LOG_DEBUG, "Packet source : %s\n", inet_ntoa(addr));
		}

		for(i = 0; i < clients_max_nr; i++)
		{
			/* Find associated client */
			if(clients[i].is_init)
			{
				/* Send to fake raw or tun device */
				if(type == TUN)
				{
					if(addr.s_addr == clients[i].local_address.s_addr)
					{
						ret = write(clients[i].tunnel.fake_tun[1], buffer, len);
						if(ret < 0)
						{
							trace(LOG_WARNING, "failed to send %zu-byte packet to "
							      "TUN interface: %s (%d)", len, strerror(errno),
							      errno);
						}
						else if(ret != len)
						{
							trace(LOG_WARNING, "partial write: only %d bytes of the "
							      "%zu-byte packet were sent to the TUN interface",
							      ret, len);
						}
						break;
					}
				}
				else
				{
					if(addr.s_addr == clients[i].tunnel.dest_address.s_addr)
					{
						ret = write(clients[i].tunnel.fake_raw[1], buffer, len);
						if(ret < 0)
						{
							trace(LOG_WARNING, "failed to send %zu-byte packet to "
							      "the underlying interface: %s (%d)", len,
							      strerror(errno), errno);
						}
						else if(ret != len)
						{
							trace(LOG_WARNING, "partial write: only %d bytes of the "
							      "%zu-byte packet were sent to the underlying "
							      "interface", ret, len);
						}
						break;
					}
				}
			}
		}
	}

	return NULL;
}


void dump_opts(struct server_opts opts)
{
	struct in_addr addr;
	addr.s_addr = opts.local_address;

	trace(LOG_INFO, "Max clients : %zu", opts.clients_max_nr);
	trace(LOG_INFO, "Port        : %d", opts.port);
	trace(LOG_INFO, "P12 file    : %s", opts.pkcs12_f);
	trace(LOG_INFO, "Pidfile     : %s", opts.pidfile_path);
	trace(LOG_INFO, "Tunnel params :");
	trace(LOG_INFO, " . Local IP  : %s/%zu", inet_ntoa(addr), opts.netmask);
	trace(LOG_INFO, " . Packing   : %d", opts.params.packing);
	trace(LOG_INFO, " . Max cid   : %zu", opts.params.max_cid);
	trace(LOG_INFO, " . Unid      : %d", opts.params.is_unidirectional);
	trace(LOG_INFO, " . Keepalive : %zu", opts.params.keepalive_timeout);
}


static void dump_stats_client(struct client *const client)
{
	int ret;

	ret = pthread_mutex_lock(&client->tunnel.status_lock);
	if(ret != 0)
	{
		trace(LOG_ERR, "dump_stats_client: failed to acquire lock for client");
		assert(0);
		goto error;
	}

	client_tracep(client, LOG_INFO, "--------------------------------------------");
	switch(client->tunnel.status)
	{
		case IPROHC_TUNNEL_CONNECTING:
			client_tracep(client, LOG_INFO, "status: connecting");
			break;
		case IPROHC_TUNNEL_CONNECTED:
			client_tracep(client, LOG_INFO, "status: connected");
			break;
		case IPROHC_TUNNEL_PENDING_DELETE:
			client_tracep(client, LOG_INFO, "status: pending delete");
			break;
		default:
			client_tracep(client, LOG_INFO, "status: unknown (%d)",
			              client->tunnel.status);
			break;
	}
	if(client->tunnel.status == IPROHC_TUNNEL_CONNECTED)
	{
		int i;

		client_tracep(client, LOG_INFO, "packing: %d", client->packing);
		client_tracep(client, LOG_INFO, "stats:");
		client_tracep(client, LOG_INFO, "  failed decompression:          %d",
		              client->tunnel.stats.decomp_failed);
		client_tracep(client, LOG_INFO, "  total  decompression:          %d",
		              client->tunnel.stats.decomp_total);
		client_tracep(client, LOG_INFO, "  failed compression:            %d",
		              client->tunnel.stats.comp_failed);
		client_tracep(client, LOG_INFO, "  total  compression:            %d",
		              client->tunnel.stats.comp_total);
		client_tracep(client, LOG_INFO, "  failed depacketization:        %d",
		              client->tunnel.stats.unpack_failed);
		client_tracep(client, LOG_INFO, "  total received packets on raw: %d",
		              client->tunnel.stats.total_received);
		client_tracep(client, LOG_INFO, "  total compressed header size:  %d bytes",
		              client->tunnel.stats.head_comp_size);
		client_tracep(client, LOG_INFO, "  total compressed packet size:  %d bytes",
		              client->tunnel.stats.total_comp_size);
		client_tracep(client, LOG_INFO, "  total header size before comp: %d bytes",
		              client->tunnel.stats.head_uncomp_size);
		client_tracep(client, LOG_INFO, "  total packet size before comp: %d bytes",
		              client->tunnel.stats.total_uncomp_size);
		client_tracep(client, LOG_INFO, "stats packing:");
		for(i = 1; i < client->tunnel.stats.n_stats_packing; i++)
		{
			client_tracep(client, LOG_INFO, "  %d packets: %d", i,
			              client->tunnel.stats.stats_packing[i]);
		}
	}
	client_tracep(client, LOG_INFO, "--------------------------------------------");

	ret = pthread_mutex_unlock(&client->tunnel.status_lock);
	if(ret != 0)
	{
		trace(LOG_ERR, "dump_stats_client: failed to acquire lock for client");
		assert(0);
		goto error;
	}

error:
	;
}


/*
 * Fonction called on SIGUSR1 to dump statistics to log
 */
void dump_stats(int sig)
{
	clients_do_dump_stats = true;
}


/*
 * Fonction called on SIGUSR2 to switch between LOG_INFO and LOG_DEBUG for log_max_priority
 */
void switch_log_max(int sig)
{
	if(log_max_priority == LOG_DEBUG)
	{
		log_max_priority = LOG_INFO;
		trace(LOG_INFO, "Debugging disabled");
	}
	else
	{
		log_max_priority = LOG_DEBUG;
		trace(LOG_DEBUG, "Debugging enabled");
	}
}


static void usage(void)
{
	printf("IP/ROHC server: establish tunnels requested by IP/ROHC clients\n"
	       "\n"
	       "Usage: iprohc_server -b itfname [opts]\n"
	       "   or: iprohc_server -h|--help\n"
	       "   or: iprohc_server -v|--version\n"
	       "\n"
	       "Options:\n"
	       "Mandatory options:\n"
	       "  -b, --basedev ITF   Name of the underlying interface\n"
	       "\n"
	       "Other options:\n"
	       "  -c, --conf PATH     Path to configuration file\n"
	       "                      (default: /etc/iprohc_server.conf)\n"
	       "  -d, --debug         Enable debuging\n"
	       "  -h, --help          Print this help message\n"
	       "  -v, --version       Print the software version\n"
	       "\n"
	       "Examples:\n"
	       "\n"
	       "Start the IP/ROHC server with default configuration file, compute\n"
	       "tunnel MTU based on network interface eth0:\n"
	       "  iprohc_server -b eth0\n"
	       "\n"
	       "Start the IP/ROHC server with the given configuration file, compute\n"
	       "tunnel MTU based on network interface wlan:\n"
	       "  iprohc_server -b wlan -c /etc/iprohc/server.cnf\n"
	       "\n"
	       "Print software version:\n"
	       "  iprohc_server --version\n"
	       "\n"
	       "Print usage help:\n"
	       "  iprohc_server --help\n"
	       "\n"
	       "Report bugs to <%s>.\n",
	       PACKAGE_BUGREPORT);

}


int alive;
void quit(int sig)
{
	trace(LOG_NOTICE, "SIGTERM or SIGINT received");
	alive = 0;
}


#ifdef STATS_COLLECTD
#include "stats.h"

int collect_server_stats(struct timeval now)
{
	lcc_connection_t *conn;
	lcc_identifier_t id = { "localhost", "iprohc", "server", "", "" };
	int nb_clients = 0;
	int j;

	if(lcc_connect(COLLECTD_PATH, &conn) < 0)
	{
		trace(LOG_ERR, "Unable to connect to collectd");
		return -1;
	}


	for(j = 0; j < server_opts.clients_max_nr; j++)
	{
		if(clients[j] != NULL && clients[j].tunnel.alive >= 0)
		{
			nb_clients++;
		}
	}

	if(collect_submit(conn, id, now, "gauge", "nb_clients", nb_clients)     < 0)
	{
		goto error;
	}

	LCC_DESTROY(conn);
	return 0;

error:
	trace(LOG_ERR, "Unable to submit to collectd");
	LCC_DESTROY(conn);
	return -1;
}


#endif

int main(int argc, char *argv[])
{
	int exit_status = 1;

	struct client *clients = NULL;
	size_t clients_nr = 0;
	size_t range_len;

	size_t client_id;
	int serv_socket;

	int tun, raw;
	int tun_itf_id;
	size_t tun_itf_mtu;
	size_t basedev_mtu;

	struct route_args route_args_tun;
	struct route_args route_args_raw;
	pthread_t tun_route_thread;
	pthread_t raw_route_thread;

	fd_set rdfs;
	int max;
	int j;

	int ret;
	sigset_t sigmask;

	gnutls_dh_params_t dh_params;

	struct server_opts server_opts;
	FILE*pid;

#ifdef STATS_COLLECTD
	struct timeval last_stats;
	struct timeval stats_timeout;
	stats_timeout.tv_sec = 10;
#endif

	bool is_ok;
	int c;
	char conf_file[1024];
	strcpy(conf_file, "/etc/iprohc_server.conf");

	/* Initialize logger */
	openlog("iprohc_server", LOG_PID, LOG_DAEMON);

	/* Signal for stats and log */
	signal(SIGINT,  quit);
	signal(SIGTERM, quit);
	signal(SIGHUP, SIG_IGN); /* used to stop client threads */
	signal(SIGPIPE, SIG_IGN); /* don't stop if TCP connection was unexpectedly closed */
	signal(SIGUSR1, dump_stats);
	signal(SIGUSR2, switch_log_max);

	/*
	 * Parsing options
	 */

	/* Default values */
	server_opts.clients_max_nr = 50;
	server_opts.port = 3126;
	server_opts.pkcs12_f[0] = '\0';
	server_opts.pidfile_path[0]  = '\0';
	memset(server_opts.basedev, 0, IFNAMSIZ);
	server_opts.local_address = inet_addr("192.168.99.1");
	server_opts.netmask = 24;

	server_opts.params.packing             = 5;
	server_opts.params.max_cid             = 14;
	server_opts.params.is_unidirectional   = 1;
	server_opts.params.wlsb_window_width   = 23;
	server_opts.params.refresh             = 9;
	server_opts.params.keepalive_timeout   = 60;
	server_opts.params.rohc_compat_version = 1;

	struct option options[] = {
		{ "conf",    required_argument, NULL, 'c' },
		{ "basedev", required_argument, NULL, 'b' },
		{ "debug",   no_argument,       NULL, 'd' },
		{ "help",    no_argument,       NULL, 'h' },
		{ "version", no_argument,       NULL, 'v' },
		{NULL, 0, 0, 0}
	};
	int option_index = 0;
	do
	{
		c = getopt_long(argc, argv, "c:b:hvd", options, &option_index);
		switch(c)
		{
			case 'c':
				trace(LOG_DEBUG, "Using file : %s", optarg);
				strncpy(conf_file, optarg, 1024);
				conf_file[1023] = '\0';
				break;
			case 'b':
				trace(LOG_DEBUG, "underlying interface: %s", optarg);
				if(strlen(optarg) >= IFNAMSIZ)
				{
					trace(LOG_ERR, "underlying interface name too long");
					goto error;
				}
				if(if_nametoindex(optarg) <= 0)
				{
					trace(LOG_ERR, "underlying interface '%s' does not exist",
					      optarg);
					goto error;
				}
				strncpy(server_opts.basedev, optarg, IFNAMSIZ);
				break;
			case 'd':
				log_max_priority = LOG_DEBUG;
				trace(LOG_DEBUG, "Debugging enabled");
				break;
			case 'h':
				usage();
				goto error;
			case 'v':
				printf("IP/ROHC server, version %s%s", PACKAGE_VERSION,
				       PACKAGE_REVNO);
				goto error;
		}
	}
	while(c != -1);

	if(parse_config(conf_file, &server_opts) < 0)
	{
		trace(LOG_ERR, "Unable to parse configuration file '%s', exiting...",
		      conf_file);
		exit_status = 2;
		goto error;
	}

	range_len = iprohc_get_ipv4_range_width(ntohl(server_opts.local_address),
	                                        server_opts.netmask);
	if(server_opts.clients_max_nr > range_len)
	{
		trace(LOG_ERR, "invalid configuration: not enough IP addresses for %zu "
		      "clients: only %zu IP addresses available in %u.%u.%u.%u/%zu",
		      server_opts.clients_max_nr, range_len,
		      (ntohl(server_opts.local_address) >> 24) & 0xff,
		      (ntohl(server_opts.local_address) >> 16) & 0xff,
		      (ntohl(server_opts.local_address) >>  8) & 0xff,
		      (ntohl(server_opts.local_address) >>  0) & 0xff,
		      server_opts.netmask);
		goto error;
	}
	trace(LOG_INFO, "%zu IP addresses available for %zu clients in IP range "
	      "%u.%u.%u.%u/%zu", range_len, server_opts.clients_max_nr,
	      (ntohl(server_opts.local_address) >> 24) & 0xff,
	      (ntohl(server_opts.local_address) >> 16) & 0xff,
	      (ntohl(server_opts.local_address) >>  8) & 0xff,
	      (ntohl(server_opts.local_address) >>  0) & 0xff,
	      server_opts.netmask);

	if(strcmp(server_opts.basedev, "") == 0)
	{
		trace(LOG_ERR, "wrong usage: underlying interface name is mandatory, "
		      "use the --basedev or -b option to specify it");
		goto error;
	}

	if(strcmp(server_opts.pkcs12_f, "") == 0)
	{
		trace(LOG_ERR, "PKCS12 file required");
		exit_status = 2;
		goto error;
	}

	dump_opts(server_opts);

	if(strcmp(server_opts.pidfile_path, "") == 0)
	{
		trace(LOG_WARNING, "No pidfile specified");
	}
	else
	{
		pid = fopen(server_opts.pidfile_path, "w");
		if(pid == NULL)
		{
			trace(LOG_ERR, "failed to open pidfile '%s': %s (%d)",
			      server_opts.pidfile_path, strerror(errno), errno);
			goto error;
		}
		fprintf(pid, "%d\n", getpid());
		fclose(pid);
	}


	/*
	 * Initialize contexts for clients
	 */

	clients = calloc(server_opts.clients_max_nr, sizeof(struct client));
	if(clients == NULL)
	{
		trace(LOG_ERR, "failed to allocate memory for the contexts of %zu "
		      "clients", server_opts.clients_max_nr);
		goto remove_pidfile;
	}
	clients_nr = 0;


	/*
	 * GnuTLS stuff
	 */

	trace(LOG_INFO, "load server certificate from file '%s'",
			server_opts.pkcs12_f);
	gnutls_global_init();
	gnutls_certificate_allocate_credentials(&(server_opts.xcred));
	gnutls_priority_init(&(server_opts.priority_cache), "NORMAL", NULL);
	if(!load_p12(server_opts.xcred, server_opts.pkcs12_f, ""))
	{
		if(!load_p12(server_opts.xcred, server_opts.pkcs12_f, NULL))
		{
			trace(LOG_ERR, "failed to load server certificate from file '%s'",
					server_opts.pkcs12_f);
			goto free_client_contexts;
		}
	}

	trace(LOG_INFO, "generate Diffie–Hellman parameters (it takes a few seconds)");
	if(!generate_dh_params(&dh_params))
	{
		trace(LOG_ERR, "failed to generate Diffie-Hellman parameters");
		goto free_client_contexts;
	}
	gnutls_certificate_set_dh_params(server_opts.xcred, dh_params);


	/*
	 * Create TCP socket
	 */
	trace(LOG_INFO, "listen on TCP 0.0.0.0:%d", server_opts.port);
	serv_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(serv_socket < 0)
	{
		trace(LOG_ERR, "failed to create TCP socket: %s (%d)",
				strerror(errno), errno);
		goto free_dh;
	}
	int on = 1;
	ret = setsockopt(serv_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to allow the TCP socket to re-use address: %s (%d)",
				strerror(errno), errno);
		goto close_tcp;
	}

	struct   sockaddr_in servaddr;
	servaddr.sin_family    = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port    = htons(server_opts.port);

	ret = bind(serv_socket, (struct sockaddr*)&servaddr, sizeof(servaddr));
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to bind on TCP/%d: %s (%d)", server_opts.port,
				strerror(errno), errno);
		goto close_tcp;
	}

	ret = listen(serv_socket, 10);
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to put TCP/%d socket in listen mode: %s (%d)",
				server_opts.port, strerror(errno), errno);
		goto close_tcp;
	}

	max = serv_socket;

	/* TUN create */
	trace(LOG_INFO, "create TUN interface");
	tun = create_tun("tun_ipip", server_opts.basedev,
	                 &tun_itf_id, &basedev_mtu, &tun_itf_mtu);
	if(tun < 0)
	{
		trace(LOG_ERR, "failed to create TUN device");
		goto close_tcp;
	}

	is_ok = set_ip4(tun_itf_id, server_opts.local_address, 24);
	if(!is_ok)
	{
		trace(LOG_ERR, "failed to set IPv4 address on TUN interface");
		goto delete_tun;
	}

	/* TUN routing thread */
	trace(LOG_INFO, "start TUN routing thread");
	route_args_tun.fd = tun;
	route_args_tun.clients = clients;
	route_args_tun.clients_max_nr = server_opts.clients_max_nr;
	route_args_tun.type = TUN;
	ret = pthread_create(&tun_route_thread, NULL, route, (void*)&route_args_tun);
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to create the TUN routing thread: %s (%d)",
				strerror(ret), ret);
		goto delete_tun;
	}

	/* RAW create */
	trace(LOG_INFO, "create RAW socket");
	raw = create_raw();
	if(raw < 0)
	{
		trace(LOG_ERR, "failed to create RAW socket");
		goto stop_tun_thread;
	}

	/* RAW routing thread */
	trace(LOG_INFO, "start RAW routing thread");
	route_args_raw.fd = raw;
	route_args_raw.clients = clients;
	route_args_tun.clients_max_nr = server_opts.clients_max_nr;
	route_args_raw.type = RAW;
	ret = pthread_create(&raw_route_thread, NULL, route, (void*)&route_args_raw);
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to create the RAW routing thread: %s (%d)",
				strerror(ret), ret);
		goto delete_raw;
	}

	/* stop writing logs on stderr */
	iprohc_log_stderr = false;

	struct timespec timeout;
	struct timeval now;

	/* mask signals during interface polling */
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGKILL);
	sigaddset(&sigmask, SIGUSR1);
	sigaddset(&sigmask, SIGUSR2);

	/* Start listening and looping on TCP socket */
	trace(LOG_INFO, "server is now ready to accept requests from clients");
#ifdef STATS_COLLECTD
	gettimeofday(&last_stats, NULL);
#endif
	alive = 1;
	while(alive)
	{
		gettimeofday(&now, NULL);
		FD_ZERO(&rdfs);
		FD_SET(serv_socket, &rdfs);
		max = serv_socket;

		/* Add client to select readfds */
		for(j = 0; j < server_opts.clients_max_nr; j++)
		{
			if(clients[j].is_init)
			{
				ret = pthread_mutex_lock(&(clients[j].tunnel.status_lock));
				if(ret != 0)
				{
					trace(LOG_ERR, "failed to acquire lock for client #%d", j);
					assert(0);
					goto delete_raw;
				}

				if(clients[j].tunnel.status >= IPROHC_TUNNEL_CONNECTING)
				{
					FD_SET(clients[j].tcp_socket, &rdfs);
					max = (clients[j].tcp_socket > max) ? clients[j].tcp_socket : max;
				}

				ret = pthread_mutex_unlock(&(clients[j].tunnel.status_lock));
				if(ret != 0)
				{
					trace(LOG_ERR, "failed to release lock for client #%d", j);
					assert(0);
					goto delete_raw;
				}
			}
		}

		/* Reset timeout */
		timeout.tv_sec = 1;
		timeout.tv_nsec = 0;

		if(pselect(max + 1, &rdfs, NULL, NULL, &timeout, &sigmask) == -1)
		{
			trace(LOG_ERR, "pselect failed: %s (%d)", strerror(errno), errno);
			continue;
		}

		/* Read on serv_socket : new client */
		if(FD_ISSET(serv_socket, &rdfs))
		{
			if(clients_nr >= server_opts.clients_max_nr)
			{
				int conn;
				struct sockaddr_in src_addr;
				socklen_t src_addr_len = sizeof(src_addr);

				trace(LOG_ERR, "no more clients accepted, maximum %zu reached",
				      server_opts.clients_max_nr);

				/* reject connection */
				conn = accept(serv_socket, (struct sockaddr*)&src_addr, &src_addr_len);
				if(conn < 0)
				{
					trace(LOG_ERR, "failed to accept new connection: %s (%d)",
					      strerror(errno), errno);
				}
				else
				{
					close(conn);
				}
			}
			else
			{
				size_t client_id;

				for(client_id = 0; client_id < server_opts.clients_max_nr &&
				    clients[client_id].is_init; client_id++)
				{
				}
				assert(client_id < server_opts.clients_max_nr);
				trace(LOG_INFO, "will store client %zu/%zu at index %zu",
				      clients_nr + 1, server_opts.clients_max_nr, client_id);

				ret = new_client(serv_socket, tun, tun_itf_mtu, basedev_mtu,
				                 &clients[client_id], client_id, server_opts);
				if(ret < 0)
				{
					trace(LOG_ERR, "new_client returned %d\n", ret);
					/* TODO : HANDLE THAT */
				}
				else
				{
					assert(clients_nr >= 0);
					assert(clients_nr < server_opts.clients_max_nr);
					clients_nr++;
				}
			}
		}

		/* Test read on each client socket */
		for(j = 0; j < server_opts.clients_max_nr; j++)
		{
			iprohc_tunnel_status_t client_status;

			if(!clients[j].is_init)
			{
				continue;
			}

			ret = pthread_mutex_lock(&(clients[j].tunnel.status_lock));
			if(ret != 0)
			{
				trace(LOG_ERR, "failed to acquire lock for client #%d", j);
				assert(0);
				goto delete_raw;
			}
			client_status = clients[j].tunnel.status;
			ret = pthread_mutex_unlock(&(clients[j].tunnel.status_lock));
			if(ret != 0)
			{
				trace(LOG_ERR, "failed to release lock for client #%d", j);
				assert(0);
				goto delete_raw;
			}

			if(client_status == IPROHC_TUNNEL_CONNECTED &&
			   (clients[j].last_keepalive.tv_sec == -1 ||
			    clients[j].last_keepalive.tv_sec +
			    ceil(clients[j].tunnel.params.keepalive_timeout / 3) < now.tv_sec))
			{
				/* send keepalive */
				char command[1] = { C_KEEPALIVE };
				trace(LOG_DEBUG, "Keepalive !");
				gnutls_record_send(clients[j].tls_session, command, 1);

				ret = pthread_mutex_lock(&clients[j].tunnel.status_lock);
				if(ret != 0)
				{
					trace(LOG_ERR, "failed to acquire lock for client #%d", j);
					assert(0);
					goto delete_raw;
				}
				gettimeofday(&(clients[j].last_keepalive), NULL);
				ret = pthread_mutex_unlock(&clients[j].tunnel.status_lock);
				if(ret != 0)
				{
					trace(LOG_ERR, "failed to release lock for client #%d", j);
					assert(0);
					goto delete_raw;
				}
			}
			else if(client_status == IPROHC_TUNNEL_PENDING_DELETE)
			{
				ret = pthread_mutex_trylock(&clients[j].tunnel.client_lock);
				if(ret == 0)
				{
					/* free dead client */
					trace(LOG_INFO, "remove context of client #%d", j);
					dump_stats_client(&(clients[j]));
					gnutls_bye(clients[j].tls_session, GNUTLS_SHUT_WR);
					/* delete client */
					del_client(&(clients[j]));

					assert(clients_nr > 0);
					assert(clients_nr <= server_opts.clients_max_nr);
					clients_nr--;
					trace(LOG_INFO, "only %zu/%zu clients remaining", clients_nr,
					      server_opts.clients_max_nr);
					assert(clients_nr >= 0);
					assert(clients_nr < server_opts.clients_max_nr);
				}
			}
			else if(FD_ISSET(clients[j].tcp_socket, &rdfs))
			{
				/* handle request */
				ret = handle_client_request(&(clients[j]));
				if(ret < 0)
				{
					if(client_status == IPROHC_TUNNEL_CONNECTED)
					{
						client_trace(clients[j], LOG_NOTICE, "client #%d was "
						             "disconnected, stop its thread", j);
						stop_client_tunnel(&(clients[j]));
					}
					else if(client_status == IPROHC_TUNNEL_CONNECTING)
					{
						client_trace(clients[j], LOG_NOTICE, "failed to connect "
						             "client #%d, aborting", j);

						ret = pthread_mutex_lock(&clients[j].tunnel.status_lock);
						if(ret != 0)
						{
							trace(LOG_ERR, "failed to acquire lock for client #%d", j);
							assert(0);
							goto delete_raw;
						}
						clients[j].tunnel.status = IPROHC_TUNNEL_PENDING_DELETE;
						ret = pthread_mutex_unlock(&clients[j].tunnel.status_lock);
						if(ret != 0)
						{
							trace(LOG_ERR, "failed to release lock for client #%d", j);
							assert(0);
							goto delete_raw;
						}
					}
				}
				else
				{
					ret = pthread_mutex_lock(&clients[j].tunnel.status_lock);
					if(ret != 0)
					{
						trace(LOG_ERR, "failed to acquire lock for client #%d", j);
						assert(0);
						goto delete_raw;
					}
					gettimeofday(&(clients[j].tunnel.last_keepalive), NULL);
					ret = pthread_mutex_unlock(&clients[j].tunnel.status_lock);
					if(ret != 0)
					{
						trace(LOG_ERR, "failed to release lock for client #%d", j);
						assert(0);
						goto delete_raw;
					}
				}
			}
		}

#ifdef STATS_COLLECTD
		if(now.tv_sec > last_stats.tv_sec + stats_timeout.tv_sec)
		{
			if(collect_server_stats(now) < 0)
			{
				trace(LOG_ERR, "Unable to commit server stats");
			}
			gettimeofday(&last_stats, NULL);
		}
#endif

		/* if SIGUSR1 was received, then dump stats */
		if(clients_do_dump_stats)
		{
			for(j = 0; j < server_opts.clients_max_nr; j++)
			{
				if(clients[j].is_init)
				{
					dump_stats_client(&(clients[j]));
				}
			}
			clients_do_dump_stats = false;
		}
	}
	trace(LOG_INFO, "someone asked to stop server");

	/* release all clients */
	trace(LOG_INFO, "release resources of connected clients...");
	for(client_id = 0; client_id < server_opts.clients_max_nr; client_id++)
	{
		if(clients[client_id].is_init)
		{
			/* close RAW socketpair */
			close(clients[client_id].tunnel.fake_raw[0]);
			clients[client_id].tunnel.fake_raw[0] = -1;
			close(clients[client_id].tunnel.fake_raw[1]);
			clients[client_id].tunnel.fake_raw[1] = -1;
			/* close RAW socket */
			close(clients[client_id].tunnel.raw_socket);
			clients[client_id].tunnel.raw_socket = -1;
			/* close TUN socketpair */
			close(clients[client_id].tunnel.fake_tun[0]);
			clients[client_id].tunnel.fake_tun[0] = -1;
			close(clients[client_id].tunnel.fake_tun[1]);
			clients[client_id].tunnel.fake_tun[1] = -1;
			/* close TUN interface */
			close(clients[client_id].tunnel.tun);
			clients[client_id].tunnel.tun = -1;
			/* close TLS session */
			gnutls_deinit(clients[client_id].tls_session);
			/* close TCP connection */
			close(clients[client_id].tcp_socket);
			/* free client context */
			clients[client_id].is_init = false;
		}
	}

	trace(LOG_INFO, "release TLS resources...");
	gnutls_certificate_free_credentials(server_opts.xcred);
	gnutls_priority_deinit(server_opts.priority_cache);
	gnutls_global_deinit();

	/* everything went fine */
	exit_status = 0;

	trace(LOG_INFO, "cancel RAW routing thread...");
	pthread_cancel(raw_route_thread);
	pthread_join(raw_route_thread, NULL);
delete_raw:
	trace(LOG_INFO, "close RAW socket...");
	close(raw);
stop_tun_thread:
	trace(LOG_INFO, "cancel TUN routing thread...");
	pthread_cancel(tun_route_thread);
	pthread_join(tun_route_thread, NULL);
delete_tun:
	trace(LOG_INFO, "close TUN interface...");
	close(tun);
close_tcp:
	trace(LOG_INFO, "close TCP server socket...");
	close(serv_socket);
free_dh:
	gnutls_dh_params_deinit(dh_params);
free_client_contexts:
	free(clients);
remove_pidfile:
	if(strcmp(server_opts.pidfile_path, "") != 0)
	{
		trace(LOG_INFO, "remove pidfile '%s'", server_opts.pidfile_path);
		unlink(server_opts.pidfile_path);
	}
error:
	if(exit_status == 0)
	{
		trace(LOG_INFO, "server stops with exit code %d", exit_status);
	}
	else
	{
		trace(LOG_NOTICE, "server stops with exit code %d", exit_status);
	}
	trace(LOG_INFO, "close syslog session");
	closelog();
	return exit_status;
}


/**
 * @brief Compute the width of the given IP range
 *
 * @param addr        The local IP address of the server (in host byte order)
 * @param netmasklen  The length (in bits) of the network mask
 * @return            The number of IP addresses available in the IP range
 */
static size_t iprohc_get_ipv4_range_width(const uint32_t addr,
                                          const size_t netmasklen)
{
	size_t range_len = (1 << (32 - netmasklen));
	const uint32_t netmask = (0xffffffff << (32 - netmasklen));

	/* if a.b.c.0 is in IP range, it cannot be used */
	if(((addr & netmask) & 0xff) == 0)
	{
		range_len--;
	}

	return range_len;
}


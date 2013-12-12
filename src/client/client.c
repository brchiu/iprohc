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

/*
client.c -- Implements client side of the ROHC IP-IP tunnel

The client will initiate the TCP connection and maintain it
while alive.

The sequence is as described below :
 * Initialization of the TCP socket
 * Connection request on TCP socket (message with C_CONNECT)
 * When C_CONNECTOK received, with server parameters, create the
   raw socket, the tun and initialize a rohc_tunnel
 * Answer keepalive message of the server with a keepalive message.

Returns :
 * 0 : Finished successfully (SIG*)
 * 1 : Server disconnected
 * 2 : Unable to connect
*/

#include "client_session.h"
#include "tun_helpers.h"
#include "messages.h"
#include "tls.h"
#include "log.h"
#include "utils.h"

#include "config.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <gnutls/gnutls.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netdb.h>
#include <assert.h>
#include <sys/signalfd.h>

int log_max_priority = LOG_INFO;
bool iprohc_log_stderr = true;


static void usage(void)
{
	printf("IP/ROHC client: establish a tunnel with an instance of IP/ROHC server\n"
	       "\n"
	       "You must be root to run the IP/ROHC tunnel client.\n"
	       "\n"
	       "Usage: iprohc_client -r remoteaddr -b itfname -i itfname -P pkcs12file [options]\n"
	       "   or: iprohc_client -h|--help\n"
	       "   or: iprohc_client -v|--version\n"
	       "\n"
	       "Options:\n"
	       "Mandatory options:\n"
	       "  -b, --basedev ITF   The name of the underlying interface\n"
	       "  -i, --dev ITF       The name of the interface that will be\n"
	       "                      created\n"
	       "  -P, --p12 PATH      The path to the PKCS#12 file containing\n"
	       "                      server CA, client key and client crt\n"
	       "  -r, --remote ADDR   The address of the remote server\n"
	       "\n"
	       "Other options:\n"
	       "  -d, --debug         Enable debuging\n"
	       "  -h, --help          Print this help message\n"
	       "  -k, --packing NUM   Override packing level sent by server\n"
	       "  -p, --port NUM      The port of the remote server\n"
	       "  -u, --up PATH       Path to a shell script that will be run\n"
	       "                      when tunnel is ready\n"
	       "  -v, --version       Print the software version\n"
	       "\n"
	       "Examples:\n"
	       "\n"
	       "Establish an IP/ROHC tunnel with remote server located at 192.168.1.14\n"
	       "through the local network interface eth0:\n"
	       "  iprohc_client -r 192.168.1.14 -b eth0 -i iprohc -P ./client.p12\n"
	       "\n"
	       "Establish an IP/ROHC tunnel with server 10.2.5.3 through the local\n"
	       "network interface eth2 and run the ./set_routes.sh script once tunnel\n"
	       "is established:\n"
	       "  iprohc_client -r 10.2.5.3 -b eth2 -i iprohc -P ./certificate \\\n"
	       "                -u ./set_routes.sh\n"
	       "\n"
	       "Print software version:\n"
	       "  iprohc_client --version\n"
	       "\n"
	       "Print usage help:\n"
	       "  iprohc_client --help\n"
	       "\n"
	       "Report bugs to <%s>.\n",
	       PACKAGE_BUGREPORT);
}


int main(int argc, char *argv[])
{
	int exit_status = 1;
	char serv_addr[PATH_MAX + 1];
	char port[6]  = {'3','1','2','6', '\0', '\0'};
	unsigned char buf[1024];
	int c;

	char pkcs12_f[PATH_MAX + 1];
	unsigned int verify_status;

	struct sockaddr_in local_addr;
	socklen_t local_addr_len;
	struct sockaddr_in remote_addr;
	int ctrl_sock = -1;

	int ret;
	bool is_ok;

	size_t basedev_mtu;
	size_t tun_itf_mtu;

	struct iprohc_client_session client;

	int signal_fd;
	sigset_t mask;
	bool is_client_alive;

	memset(client.tun_name, 0, IFNAMSIZ);
	memset(client.basedev, 0, IFNAMSIZ);
	memset(client.up_script_path, 0, PATH_MAX + 1);
	client.packing = 0;
	serv_addr[0] = '\0';
	pkcs12_f[0] = '\0';

	/* Initialize logger */
	openlog("iprohc_client", LOG_PID, LOG_DAEMON);


	/*
	 * Parsing options
	 */

	struct option options[] = {
		{ "dev",     required_argument, NULL, 'i' },
		{ "basedev", required_argument, NULL, 'b' },
		{ "remote",  required_argument, NULL, 'r' },
		{ "port",    required_argument, NULL, 'p' },
		{ "p12",     required_argument, NULL, 'P' },
		{ "packing", required_argument, NULL, 'k' },
		{ "up",      required_argument, NULL, 'u' },
		{ "debug",   no_argument, NULL, 'd' },
		{ "help",    no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'v' },
		{NULL, 0, 0, 0}
	};

	do
	{
		c = getopt_long(argc, argv, "i:b:r:p:u:P:hvk:d", options, NULL);
		switch(c)
		{
			case 'd':
				log_max_priority = LOG_DEBUG;
				trace(LOG_DEBUG, "Debbuging enabled");
				break;
		}
	}
	while(c != -1);

	optind = 1;
	do
	{
		c = getopt_long(argc, argv, "i:b:r:p:u:P:hvk:d", options, NULL);
		switch(c)
		{
			case 'i':
				trace(LOG_DEBUG, "TUN interface: %s", optarg);
				if(strlen(optarg) >= IFNAMSIZ)
				{
					trace(LOG_ERR, "TUN interface name too long");
					goto error;
				}
				strncpy(client.tun_name, optarg, IFNAMSIZ);
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
				strncpy(client.basedev, optarg, IFNAMSIZ);
				break;
			case 'r':
				if(strlen(optarg) > PATH_MAX)
				{
					trace(LOG_ERR, "remote address is too long");
					goto error;
				}
				trace(LOG_DEBUG, "Remote address: %s", optarg);
				strncpy(serv_addr, optarg, PATH_MAX);
				break;
			case 'p':
				strncpy(port, optarg, 6);
				port[5] = '\0';
				trace(LOG_DEBUG, "Remote port : %s", port);
				break;
			case 'P':
				if(strlen(optarg) > PATH_MAX)
				{
					trace(LOG_ERR, "path of PKCS12 file is too long");
					goto error;
				}
				strncpy(pkcs12_f, optarg, PATH_MAX);
				trace(LOG_DEBUG, "PKCS12 file: %s", pkcs12_f);
				break;
			case 'u':
				trace(LOG_DEBUG, "Up script path: %s", optarg);
				if(strlen(optarg) > PATH_MAX)
				{
					trace(LOG_ERR, "Up script path too long");
					goto error;
				}
				strncpy(client.up_script_path, optarg, PATH_MAX);
				break;
			case 'k':
				client.packing = atoi(optarg);
				trace(LOG_DEBUG, "Using forced packing : %d\n", client.packing);
				break;
			case 'h':
				usage();
				goto error;
			case 'v':
				printf("IP/ROHC client, version %s%s\n", PACKAGE_VERSION,
				       PACKAGE_REVNO);
				goto error;
		}
	}
	while(c != -1);

	if(strcmp(serv_addr, "") == 0)
	{
		trace(LOG_ERR, "wrong usage: remote address is mandatory, "
		      "use the --remote or -r option to specify it");
		goto error;
	}

	if(strcmp(client.tun_name, "") == 0)
	{
		trace(LOG_ERR, "wrong usage: TUN interface name is mandatory, "
		      "use the --dev or -i option to specify it");
		goto error;
	}

	if(strcmp(client.basedev, "") == 0)
	{
		trace(LOG_ERR, "wrong usage: underlying interface name is mandatory, "
		      "use the --basedev or -b option to specify it");
		goto error;
	}

	if(strcmp(pkcs12_f, "") == 0)
	{
		trace(LOG_ERR, "PKCS12 file required");
		trace(LOG_ERR, "wrong usage: PKCS12 file is mandatory, "
		      "use the --p12 or -P option to specify it");
		goto error;
	}


	/*
	 * Handle signals for stats and log
	 */

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGQUIT);

	/* block signals to handle them through signal fd */
	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to block UNIX signals: %s (%d)",
		      strerror(errno), errno);
		goto error;
	}

	/* create signal fd */
	signal_fd = signalfd(-1, &mask, 0);
	if(signal_fd < 0)
	{
		trace(LOG_ERR, "failed to create signal fd: %s (%d)",
		      strerror(errno), errno);
		goto error;
	}


	/*
	 * Initialize client context
	 */

	/* load certificates and key for TLS session */
	gnutls_global_init();
	gnutls_certificate_allocate_credentials(&(client.tls_cred));
	ret = load_p12(client.tls_cred, pkcs12_f, NULL);
	if(ret < 0)
	{
		/* try with empty password */
		ret = load_p12(client.tls_cred, pkcs12_f, "");
		if(ret < 0)
		{
			trace(LOG_ERR, "failed to load certificate: %s (%d)",
			      gnutls_strerror(ret), ret);
			goto tls_deinit;
		}
	}

	/* create the TUN interface */
	client.tun = create_tun(client.tun_name, client.basedev,
	                         &client.tun_itf_id, &basedev_mtu, &tun_itf_mtu);
	if(client.tun < 0)
	{
		trace(LOG_ERR, "Unable to create TUN device");
		goto tls_deinit;
	}

	/* set RAW  */
	client.raw = create_raw();
	if(client.raw < 0)
	{
		trace(LOG_ERR, "Unable to create RAW socket");
		goto delete_tun;
	}


	/*
	 * DNS query
	 */

	struct addrinfo *result, *rp;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;    /* Allow IPv4 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;           /* Any protocol */

	ret = getaddrinfo(serv_addr, port, &hints, &result);
	if(ret != 0)
	{
		trace(LOG_ERR, "Unable to connect to %s: %s (%d)", serv_addr,
		      gai_strerror(ret), ret);
		goto delete_raw;
	}


	/*
	 * Creation of TCP socket to negotiate parameters and maintain it
	 */

	if(result == NULL) /* no address available */
	{
		trace(LOG_ERR, "failed connect to server: no address available");
		goto free_addrinfo;
	}
	for(rp = result; rp != NULL; rp = rp->ai_next)
	{
		ctrl_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(ctrl_sock < 0)
		{
			continue;
		}

		if(connect(ctrl_sock, rp->ai_addr, rp->ai_addrlen) != -1)
		{
			break; /* success */
		}
		/* failure */
		close(ctrl_sock);
		ctrl_sock = -1;
	}
	if(rp == NULL || ctrl_sock < 0) /* no address succeeded */
	{
		trace(LOG_ERR, "failed to connect to server: %s (%d)",
				strerror(errno), errno);
		goto free_addrinfo;
	}
	memcpy(&remote_addr, rp->ai_addr, sizeof(struct sockaddr_in));

	/* retrieve the local address and port used to contact the server
	 * (will be used to filter ingress data traffic later on) */
	local_addr_len = sizeof(struct sockaddr_in);
	memset(&local_addr, 0, local_addr_len);
	ret = getsockname(ctrl_sock, (struct sockaddr *) &local_addr, &local_addr_len);
	if(ret != 0)
	{
		trace(LOG_ERR, "failed to determine the local IP address used to "
				"contact the server: %s (%d)", strerror(errno), errno);
		goto close_tcp;
	}
	trace(LOG_INFO, "local address %u.%u.%u.%u:%u is used to contact server",
			(ntohl(local_addr.sin_addr.s_addr) >> 24) & 0xff,
			(ntohl(local_addr.sin_addr.s_addr) >> 16) & 0xff,
			(ntohl(local_addr.sin_addr.s_addr) >>  8) & 0xff,
			(ntohl(local_addr.sin_addr.s_addr) >>  0) & 0xff,
			ntohs(local_addr.sin_port));

	/*
	 * Initialize session context
	 */

	if(!iprohc_session_new(&(client.session), GNUTLS_CLIENT, client.tls_cred,
	                       NULL, ctrl_sock, local_addr.sin_addr, remote_addr,
	                       client.raw, client.tun, basedev_mtu, tun_itf_mtu, 0))
	{
		trace(LOG_ERR, "failed to init session context");
		goto close_tcp;
	}
	ctrl_sock = -1; /* avoid double close() */

	/* stop writing logs on stderr */
	iprohc_log_stderr = false;


	/*
	 * TLS handshake
	 */

	/* Get rid of warning, it's a "bug" of GnuTLS
	 * (cf. http://lists.gnu.org/archive/html/help-gnutls/2006-03/msg00020.html) */
	gnutls_transport_set_ptr_nowarn(client.session.tls_session,
	                                client.session.tcp_socket);

	/* perform TLS handshake with server */
	do
	{
		ret = gnutls_handshake(client.session.tls_session);
	}
	while(ret < 0 && gnutls_error_is_fatal(ret) == 0);
	if(ret < 0)
	{
		trace(LOG_ERR, "TLS handshake failed : %s", gnutls_strerror(ret));
		exit_status = 2;
		goto free_session;
	}
	trace(LOG_INFO, "TLS handshake succeeded");

	/* check server certificate */
	if(gnutls_certificate_verify_peers2(client.session.tls_session, &verify_status) < 0)
	{
		trace(LOG_ERR, "TLS verify failed : %s", gnutls_strerror(ret));
		exit_status = -3;
		goto close_tls;
	}

	if((verify_status & GNUTLS_CERT_INVALID) &&
	   (verify_status != (GNUTLS_CERT_INSECURE_ALGORITHM | GNUTLS_CERT_INVALID)))
	{
		trace(LOG_ERR, "certificate cannot be verified (status %u)",
		      verify_status);
		if(verify_status & GNUTLS_CERT_REVOKED)
		{
			trace(LOG_ERR, " - Revoked certificate");
		}
		if(verify_status & GNUTLS_CERT_SIGNER_NOT_FOUND)
		{
			trace(LOG_ERR, " - Unable to trust certificate issuer");
		}
		if(verify_status & GNUTLS_CERT_SIGNER_NOT_CA)
		{
			trace(LOG_ERR, " - Certificate issuer is not a CA");
		}
#ifdef GNUTLS_CERT_NOT_ACTIVATED
		if(verify_status & GNUTLS_CERT_NOT_ACTIVATED)
		{
			trace(LOG_ERR, " - The certificate is not activated");
		}
#endif
#ifdef GNUTLS_CERT_EXPIRED
		if(verify_status & GNUTLS_CERT_EXPIRED)
		{
			trace(LOG_ERR, " - The certificate has expired");
		}
#endif
		exit_status = -3;
		goto close_tls;
	}
	trace(LOG_INFO, "client certificate accepted");

	/* ask for connection to server */
	{
		unsigned char command[1024];
		size_t command_len;
		size_t tlv_len;

		command[0] = C_CONNECT;
		command_len = 1;

		trace(LOG_INFO, "send connect message to server");
		is_ok = gen_connrequest(client.packing, command + 1, &tlv_len);
		if(!is_ok)
		{
			trace(LOG_ERR, "failed to generate the connect messsage for server");
			goto close_tls;
		}
		command_len += tlv_len;

		/* Emit a simple connect message */
		size_t emitted_len = 0;
		do
		{
			ret = gnutls_record_send(client.session.tls_session,
			                         command + emitted_len,
			                         command_len - emitted_len);
			if(ret < 0)
			{
				trace(LOG_ERR, "failed to send message to server over TLS (%d)", ret);
				goto close_tls;
			}
			emitted_len += ret;
		}
		while(emitted_len < command_len);
	}


	/*
	 * Main loop
	 */

	/* Wait for answer and other messages, close when socket is close */
	trace(LOG_INFO, "wait for connect answer from server");
	is_client_alive = true;
	while(is_client_alive)
	{
		size_t timeout_orig;
		struct timeval timeout;
		int max_fd = 0;
		fd_set rdfs;

		timeout_orig = 80;
		if(client.session.status == IPROHC_SESSION_CONNECTED)
		{
			timeout_orig = client.session.tunnel.params.keepalive_timeout * 2;
		}
		timeout.tv_sec = timeout_orig;
		timeout.tv_usec = 0;

		FD_ZERO(&rdfs);
		FD_SET(client.session.tcp_socket, &rdfs);
		max_fd = max(client.session.tcp_socket, max_fd);
		FD_SET(signal_fd, &rdfs);
		max_fd = max(signal_fd, max_fd);

		ret = select(max_fd + 1, &rdfs, NULL, NULL, &timeout);
		if(ret < 0)
		{
			if(errno == EINTR)
			{
				/* interrupted by a signal */
				continue;
			}
			trace(LOG_ERR, "select failed: %s (%d)", strerror(errno), errno);
			goto close_tls;
		}
		else if(ret == 0)
		{
			/* timeout reached */
			trace(LOG_WARNING, "timeout (%zu seconds) reached while waiting for "
			      "message on TCP connection, give up", timeout_orig);
			goto close_tls;
		}

		/* UNIX signal received? */
		if(FD_ISSET(signal_fd, &rdfs))
		{
	   	struct signalfd_siginfo signal_infos;

			ret = read(signal_fd, &signal_infos, sizeof(struct signalfd_siginfo));
			if(ret < 0)
			{
				trace(LOG_ERR, "failed to retrieve information about the received "
				      "UNIX signal: %s (%d)", strerror(errno), errno);
				continue;
			}
			else if(ret != sizeof(struct signalfd_siginfo))
			{
				trace(LOG_ERR, "failed to retrieve information about the received "
				      "UNIX signal: only %d bytes expected while %zu bytes received",
				      ret, sizeof(struct signalfd_siginfo));
				continue;
			}

			switch(signal_infos.ssi_signo)
			{
				case SIGINT:
				case SIGTERM:
				case SIGQUIT:
				{
					if(signal_infos.ssi_pid > 0)
					{
						/* killed by known process */
						trace(LOG_NOTICE, "process with PID %d run by user with UID "
						      "%d asked the IP/ROHC client to shutdown",
						      signal_infos.ssi_pid, signal_infos.ssi_uid);
					}
					else
					{
						/* killed by unknown process */
						trace(LOG_NOTICE, "user with UID %d asked the IP/ROHC client "
						      "to shutdown", signal_infos.ssi_uid);
					}
					is_client_alive = false;
					continue;
				}
				default:
				{
					trace(LOG_NOTICE, "ignore unexpected signal %d",
					      signal_infos.ssi_signo);
					break;
				}
			}
		}

		if(FD_ISSET(client.session.tcp_socket, &rdfs))
		{
			ret = gnutls_record_recv(client.session.tls_session, buf, 1024);
			if(ret < 0)
			{
				trace(LOG_ERR, "failed to receive data from server on TLS "
				      "session: %s (%d)", gnutls_strerror(ret), ret);
				goto close_tls;
			}
			else if(ret == 0)
			{
				trace(LOG_ERR, "TLS session was interrupted by server");
				goto close_tls;
			}
			if(!handle_message(&client, buf, ret))
			{
				trace(LOG_ERR, "failed to handle message received from server");
				goto close_tls;
			}
		}
	}

	trace(LOG_INFO, "client interrupted, interrupt established session");

	/* send disconnect message to server */
	if(!client_send_disconnect_msg(client.session.tls_session))
	{
		trace(LOG_WARNING, "failed to cleanly close the session with server");
	}

	exit_status = 0;

close_tls:
	trace(LOG_INFO, "close TLS session");
	gnutls_bye(client.session.tls_session, GNUTLS_SHUT_RDWR);
free_session:
	trace(LOG_INFO, "close session");
	if(!iprohc_session_free(&(client.session)))
	{
		trace(LOG_ERR, "failed to reset session context");
	}
close_tcp:
	if(ctrl_sock >= 0)
	{
		trace(LOG_INFO, "close TCP connection");
		close(ctrl_sock);
	}
free_addrinfo:
	freeaddrinfo(result);
delete_raw:
	close(client.raw);
delete_tun:
	close(client.tun);
tls_deinit:
	trace(LOG_INFO, "free TLS resources");
	gnutls_certificate_free_credentials(client.tls_cred);
	gnutls_global_deinit();
/*close_signal_fd:*/
	close(signal_fd);
error:
	closelog();
	return exit_status;
}


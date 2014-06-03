#include<stdlib.h>
#include<stdio.h>
#include<errno.h>
#include<string.h>


#include <getopt.h>

#include "xfutils.h"
#include "xfsocket.h"

#include "wbclientconn.h"

char* listen_port = "5433";
char* master_host = "localhost";
char* master_port = "5432";

void XlogFilterMain()
{
	// set up signals for child reaper, etc.
	// open socket for listening
	XfSocket server = OpenServerSocket(listen_port);
	XfConn conn;

	conn = ConnCreate(server);
	conn->master_host = master_host;
	conn->master_port = master_port;

	WbCCInitConnection(conn);

	WbCCPerformAuthentication(conn);

	WbCCCommandLoop(conn);

	CloseConn(conn);

	CloseSocket(server);
}

const char* progname;

static void usage()
{
	printf("%s proxys PostgreSQL streaming replication connections and optionally does filtering\n\n", progname);
	printf("Options:\n");
	printf("  -?, --help                Print this message\n");
	printf("  -h, --host=HOST           Connect to master on this host. Default localhost\n");
	printf("  -p, --port=PORT           Run proxy on this port. Default 5433\n");

}

int
main(int argc, char **argv)
{
	int c;
	progname = "xlogfilter";

	while (1)
	{
		static struct option long_options[] =
		{
				{"port", required_argument, 0, 'p'},
				{"host", required_argument, 0, 'h'},
				{"masterport", required_argument, 0, 'P'},
				{"help", no_argument, 0, '?'},
				{0,0,0,0}
		};
		int option_index = 0;

		c = getopt_long(argc, argv, "p:h?",
				long_options, &option_index);

		if (c == -1)
			break;

		switch (c)
		{
		case 'p':
			listen_port = xfstrdup(optarg);
			break;
		case 'h':
			master_host = xfstrdup(optarg);
			break;
		case 'P':
			master_port = xfstrdup(optarg);
			break;
		case '?':
			usage();
			exit(0);
			break;
		default:
			fprintf(stderr, "Invalid arguments\n");
			exit(1);
		}
	}


	XlogFilterMain();
	return 0;
}

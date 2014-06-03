#include "wbclientconn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#include "xfsocket.h"
#include "xfutils.h"
#include "wbfilter.h"
#include "wbmasterconn.h"

#include "parser/parser.h"

#define MAX_CONNINFO_LEN 4000
#define NAPTIME 100

typedef struct {
	int qtype;
	XfMessage *msg;
} XfCommand;

static int WbCCProcessStartupPacket(XfConn conn, bool SSLdone);
static int WbCCReadCommand(XfConn conn, XfCommand *cmd);
static void WbCCSendReadyForQuery(XfConn conn);
static MasterConn* WbCCOpenConnectionToMaster(XfConn conn);
static void ForbiddenInWalBouncer();
static void WbCCBeginReportingGUCOptions(XfConn conn, MasterConn* master);
static void WbCCReportGuc(XfConn conn, MasterConn* master, char *name);
static void WbCCExecCommand(XfConn conn, MasterConn *master, char *query_string);
static void WbCCExecIdentifySystem(XfConn conn, MasterConn *master);
static void WbCCExecStartPhysical(XfConn conn, MasterConn *master, ReplicationCommand *cmd);
static void WbCCExecTimeline();
static Oid* WbCCFindTablespaceOids(XfConn conn);
//static void WbCCSendWALRecord(XfConn conn, char *data, int len, XLogRecPtr sentPtr, TimestampTz lastSend);
//static void WbCCSendEndOfWal(XfConn conn);
static void WbCCProcessRepliesIfAny(XfConn conn);
static void WbCCProcessReplyMessage(XfConn conn);
static void WbCCProcessStandbyReplyMessage(XfConn conn, XfMessage *msg);
static void WbCCSendKeepalive(XfConn conn, bool request_reply);
static void WbCCProcessStandbyHSFeedbackMessage(XfConn conn, XfMessage *msg);
static void WbCCSendCopyBothResponse(XfConn conn);
static void WbCCSendWalBlock(XfConn conn, ReplMessage *msg, FilterData *fl);



void
WbCCInitConnection(XfConn conn)
{
	xf_info("Received conn on fd %d", conn->fd);

	//FIXME: need to timeout here
	// setup error log destination
	// copy socket info out here
	if (WbCCProcessStartupPacket(conn, false) != STATUS_OK)
		error("Error while processing startup packet");
}

void
WbCCPerformAuthentication(XfConn conn)
{
	int status = STATUS_ERROR;

	status = STATUS_OK;

	if (status == STATUS_OK)
	{
		xf_info("Send auth packet");
		ConnBeginMessage(conn, 'R');
		ConnSendInt(conn, (int32) AUTH_REQ_OK, sizeof(int32));
		ConnEndMessage(conn);
		ConnFlush(conn);
	}
	else
	{
		error("auth failed");
	}
}

static int
WbCCProcessStartupPacket(XfConn conn, bool SSLdone)
{
	int32 len;
	void *buf;
	ProtocolVersion proto;

	if (ConnGetBytes(conn, (char*) &len, 4) == EOF)
	{
		log_error("Incomplete startup packet");
		return STATUS_ERROR;
	}

	len = ntohl(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		log_error("Invalid length of startup packet");
		return STATUS_ERROR;
	}

	/*
	 * Allocate at least the size of an old-style startup packet, plus one
	 * extra byte, and make sure all are zeroes.  This ensures we will have
	 * null termination of all strings, in both fixed- and variable-length
	 * packet layouts.
	 */
	if (len <= (int32) sizeof(StartupPacket))
		buf = xfalloc0(sizeof(StartupPacket) + 1);
	else
		buf = xfalloc0(len + 1);

	if (ConnGetBytes(conn, buf, len) == EOF)
	{
		log_error("incomplete startup packet");
		return STATUS_ERROR;
	}

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */
	conn->proto = proto = ntohl(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		error("Cancel not supported");
		/* Not really an error, but we don't want to proceed further */
		return STATUS_ERROR;
	}

	if (proto == NEGOTIATE_SSL_CODE && !SSLdone)
	{
		char		SSLok;

#ifdef USE_SSL
		/* No SSL when disabled or on Unix sockets */
		if (!EnableSSL || IS_AF_UNIX(port->laddr.addr.ss_family))
			SSLok = 'N';
		else
			SSLok = 'S';		/* Support for SSL */
#else
		SSLok = 'N';			/* No support for SSL */
#endif

retry1:
		if (send(conn->fd, &SSLok, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry1;	/* if interrupted, just retry */
			error("failed to send SSL negotiation response");
			return STATUS_ERROR;	/* close the connection */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif
		/* regular startup packet, cancel, etc packet should follow... */
		/* but not another SSL negotiation request */
		return WbCCProcessStartupPacket(conn, true);
	}

	/* Could add additional special packet types here */

	/* Check we can handle the protocol the frontend is using. */

	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) ||
		(PG_PROTOCOL_MAJOR(proto) == PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) &&
		 PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST)))
		error("Unsupported frontend protocol");

	/*
	 * Now fetch parameters out of startup packet and save them into the Port
	 * structure.  All data structures attached to the Port struct must be
	 * allocated in TopMemoryContext so that they will remain available in a
	 * running backend (even after PostmasterContext is destroyed).  We need
	 * not worry about leaking this storage on failure, since we aren't in the
	 * postmaster process anymore.
	 */
	//oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	conn->guc_options = xfalloc0(len);
	conn->gucs_len = 0;
	conn->database_name = NULL;
	conn->user_name = NULL;
	conn->cmdline_options = NULL;
	conn->include_tablespaces = NULL;

	{
		int32		offset = sizeof(ProtocolVersion);
		bool am_walsender = false;
		/*
		 * Scan packet body for name/option pairs.  We can assume any string
		 * beginning within the packet body is null-terminated, thanks to
		 * zeroing extra byte above.
		 */
		while (offset < len)
		{
			char	   *nameptr = ((char *) buf) + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* found packet terminator */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* missing value, will complain below */
			valptr = ((char *) buf) + valoffset;

			if (strcmp(nameptr, "database") == 0)
				conn->database_name = xfstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				conn->user_name = xfstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				conn->cmdline_options = xfstrdup(valptr);
			else if (strcmp(nameptr, "replication") == 0)
			{
				/*
				 * Due to backward compatibility concerns the replication
				 * parameter is a hybrid beast which allows the value to be
				 * either boolean or the string 'database'. The latter
				 * connects to a specific database which is e.g. required for
				 * logical decoding.
				 */
				if (strcmp(valptr, "database") == 0
					|| strcasecmp(valptr, "on")
					|| strcasecmp(valptr, "yes")
					|| strcasecmp(valptr, "true")
					|| strcmp(valptr, "1"))
				{
					am_walsender = true;
				}
				else if (strcasecmp(valptr, "off")
						|| strcasecmp(valptr, "no")
						|| strcasecmp(valptr, "0"))
					error("This is a WAL proxy that only accepts replication connections");
				else
					error("invalid value for parameter \"replication\"");
			}
			else if (strcmp(nameptr, "application_name") == 0)
			{
				conn->include_tablespaces = xfstrdup(valptr);
			}
			else
			{
				int guclen = (valoffset - offset) + strlen(valptr) + 1;
				/* Assume it's a generic GUC option */
				memcpy(conn->guc_options + conn->gucs_len, nameptr,
						guclen);
				conn->gucs_len += guclen;

			}
			offset = valoffset + strlen(valptr) + 1;
		}

		if (!am_walsender)
			error("This is a WAL proxy that only accepts replication connections");

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 */
		if (offset != len - 1)
			error("invalid startup packet layout: expected terminator as last byte");
	}

	/* Check a user name was given. */
	if (conn->user_name == NULL || conn->user_name[0] == '\0')
		error("no PostgreSQL user name specified in startup packet");

	return STATUS_OK;
}


void
WbCCCommandLoop(XfConn conn)
{
	int firstchar;
	bool send_ready_for_query = true;
	MasterConn* master = WbCCOpenConnectionToMaster(conn);

	WbCCBeginReportingGUCOptions(conn, master);

	// Cancel message
	ConnBeginMessage(conn, 'K');
	ConnSendInt(conn, 0, 4); // PID
	ConnSendInt(conn, 0, 4); // Cancel key
	ConnEndMessage(conn);

	// set up error handling

	for (;;)
	{
		XfCommand cmd;
		cmd.msg = NULL;
		cmd.qtype = 0;
		if (send_ready_for_query)
		{
			WbCCSendReadyForQuery(conn);
			send_ready_for_query = false;
		}

		firstchar = WbCCReadCommand(conn, &cmd);
		xf_info("after read command\n");
		switch (firstchar)
		{
			case 'Q':
				{
					WbCCExecCommand(conn, master, cmd.msg->data);
					send_ready_for_query = true;

					/*char *query_string = pq_getmsgstgring(msg);
					pq_getmsgend(msg);

					ExecCommand(cmd->);

					send_ready_for_query = true;*/
				}
				break;
			case 'P':
			case 'B':
			case 'E':
			case 'F':
			case 'C':
			case 'D':
				ForbiddenInWalBouncer();
				break;
			case 'H':
				ConnFlush(conn);
				break;
			case 'S':
				send_ready_for_query = true;
				break;
			case 'X':
			case EOF:
				// TODO: do something here?
				if (cmd.msg)
					ConnFreeMessage(cmd.msg);
				return;
			case 'd':
			case 'c':
			case 'f':
				break;
			default:
				error("invalid frontend message type");
		}
		if (cmd.msg)
			ConnFreeMessage(cmd.msg);
	}
}

static void
ForbiddenInWalBouncer()
{
	error("Invalid command for walsender");
}



static int
WbCCReadCommand(XfConn conn, XfCommand *cmd)
{
	int qtype;
	cmd->qtype = qtype = ConnGetByte(conn);

	if (qtype == EOF)
		return EOF;

	if (ConnGetMessage(conn, &(cmd->msg)))
		return EOF;

	xf_info("Command %c payload %d\n", qtype, cmd->msg->len);

	return qtype;
}

static void
WbCCSendReadyForQuery(XfConn conn)
{
	ConnBeginMessage(conn, 'Z');
	ConnSendInt(conn, 'I', 1);
	ConnEndMessage(conn);
	ConnFlush(conn);
}

static MasterConn*
WbCCOpenConnectionToMaster(XfConn conn)
{
	MasterConn* master;
	char conninfo[MAX_CONNINFO_LEN+1];
	char *buf = conninfo;
	char *buf_end = &(conninfo[MAX_CONNINFO_LEN]);

	memset(conninfo, 0, sizeof(conninfo));

	if (conn->master_host) {
		buf += snprintf(buf, buf_end - buf, "host=%s ", conn->master_host);
	}

	if (conn->master_port)
		buf +=  snprintf(buf, buf_end - buf, "port=%s ", conn->master_port);

	if (conn->user_name)
		buf += snprintf(buf, buf_end - buf, "user=%s ", conn->user_name);

	buf += snprintf(buf, buf_end - buf, "dbname=replication replication=true application_name=walbouncer");

	xf_info("Start connecting to %s\n", conninfo);
	master = WbMcOpenConnection(conninfo);
	xf_info("Connected to master\n");
	return master;
}

static void
WbCCBeginReportingGUCOptions(XfConn conn, MasterConn* master)
{
	//conn->sendBufMsgLenPtr
	 WbCCReportGuc(conn, master, "server_version");
	 WbCCReportGuc(conn, master, "server_encoding");
	 WbCCReportGuc(conn, master, "client_encoding");
	 WbCCReportGuc(conn, master, "application_name");
	 WbCCReportGuc(conn, master, "is_superuser");
	 WbCCReportGuc(conn, master, "session_authorization");
	 WbCCReportGuc(conn, master, "DateStyle");
	 WbCCReportGuc(conn, master, "IntervalStyle");
	 WbCCReportGuc(conn, master, "TimeZone");
	 WbCCReportGuc(conn, master, "integer_datetimes");
	 WbCCReportGuc(conn, master, "standard_conforming_strings");
}

static void
WbCCReportGuc(XfConn conn, MasterConn* master, char *name)
{
	const char *value = WbMcParameterStatus(master, name);
	if (!value)
		return;
	ConnBeginMessage(conn, 'S');
	ConnSendString(conn, name);
	ConnSendString(conn, value);
	ConnEndMessage(conn);
}

static void
WbCCExecCommand(XfConn conn, MasterConn *master, char *query_string)
{
	int parse_rc;
	ReplicationCommand *cmd;

	replication_scanner_init(query_string);
	parse_rc = replication_yyparse();

	if (parse_rc != 0)
		error("Parse failed");

	cmd = replication_parse_result;

	xf_info("Query: %s\n", query_string);

	switch (cmd->command)
	{
		case REPL_IDENTIFY_SYSTEM:
			WbCCExecIdentifySystem(conn, master);
			break;
		case REPL_BASE_BACKUP:
		case REPL_CREATE_SLOT:
		case REPL_DROP_SLOT:
			error("Command not supported");
			break;
		case REPL_START_PHYSICAL:
			WbCCExecStartPhysical(conn, master, cmd);
			break;
		case REPL_START_LOGICAL:
			error("Command not supported");
			break;
		case REPL_TIMELINE:
			WbCCExecTimeline();
			//TODO
			break;
	}


	ConnBeginMessage(conn, 'C');
	ConnSendString(conn, "SELECT");
	ConnEndMessage(conn);
	xffree(cmd);
}

/* TODO: move these to PG version specific config file */
#define TEXTOID 25
#define INT4OID 23

static void
WbCCExecIdentifySystem(XfConn conn, MasterConn *master)
{
	// query master server, pass through data
	char *primary_sysid;
	char *primary_tli;
	char *primary_xpos;
	char *dbname = NULL;

	if (!WbMcIdentifySystem(master,
			&primary_sysid,
			&primary_tli,
			&primary_xpos))
		error("Identify system failed.");

	xf_info("System identifier: %s\n", primary_sysid);
	xf_info("TLI: %s\n", primary_tli);
	xf_info("Xpos: %s\n", primary_xpos);
	xf_info("dbname: %s\n", dbname);

	//TODO: parse out tli and xpos for our use

	/* Send a RowDescription message */
	ConnBeginMessage(conn, 'T');
	ConnSendInt(conn, 4, 2);		/* 4 fields */

	/* first field */
	ConnSendString(conn, "systemid");	/* col name */
	ConnSendInt(conn, 0, 4);		/* table oid */
	ConnSendInt(conn, 0, 2);		/* attnum */
	ConnSendInt(conn, TEXTOID, 4);		/* type oid */
	ConnSendInt(conn, -1, 2);	/* typlen */
	ConnSendInt(conn, 0, 4);		/* typmod */
	ConnSendInt(conn, 0, 2);		/* format code */

	/* second field */
	ConnSendString(conn, "timeline");	/* col name */
	ConnSendInt(conn, 0, 4);		/* table oid */
	ConnSendInt(conn, 0, 2);		/* attnum */
	ConnSendInt(conn, INT4OID, 4);		/* type oid */
	ConnSendInt(conn, 4, 2);		/* typlen */
	ConnSendInt(conn, 0, 4);		/* typmod */
	ConnSendInt(conn, 0, 2);		/* format code */

	/* third field */
	ConnSendString(conn, "xlogpos");		/* col name */
	ConnSendInt(conn, 0, 4);		/* table oid */
	ConnSendInt(conn, 0, 2);		/* attnum */
	ConnSendInt(conn, TEXTOID, 4);		/* type oid */
	ConnSendInt(conn, -1, 2);	/* typlen */
	ConnSendInt(conn, 0, 4);		/* typmod */
	ConnSendInt(conn, 0, 2);		/* format code */

	/* fourth field */
	ConnSendString(conn, "dbname");		/* col name */
	ConnSendInt(conn, 0, 4);		/* table oid */
	ConnSendInt(conn, 0, 2);		/* attnum */
	ConnSendInt(conn, TEXTOID, 4);		/* type oid */
	ConnSendInt(conn, -1, 2);	/* typlen */
	ConnSendInt(conn, 0, 4);		/* typmod */
	ConnSendInt(conn, 0, 2);		/* format code */
	ConnEndMessage(conn);

	/* Send a DataRow message */
	ConnBeginMessage(conn, 'D');
	ConnSendInt(conn, 4, 2);		/* # of columns */
	ConnSendInt(conn, strlen(primary_sysid), 4); /* col1 len */
	ConnSendBytes(conn, primary_sysid, strlen(primary_sysid));
	ConnSendInt(conn, strlen(primary_tli), 4);	/* col2 len */
	ConnSendBytes(conn, (char *) primary_tli, strlen(primary_tli));
	ConnSendInt(conn, strlen(primary_xpos), 4);	/* col3 len */
	ConnSendBytes(conn, (char *) primary_xpos, strlen(primary_xpos));
	/* send NULL if not connected to a database */
	if (dbname)
	{
		ConnSendInt(conn, strlen(dbname), 4);	/* col4 len */
		ConnSendBytes(conn, (char *) dbname, strlen(dbname));
	}
	else
	{
		ConnSendInt(conn, -1, 4);	/* col4 len, NULL */
	}

	ConnEndMessage(conn);

	xffree(primary_sysid);
	xffree(primary_tli);
	xffree(primary_xpos);
}

static void
WbCCExecStartPhysical(XfConn conn, MasterConn *master, ReplicationCommand *cmd)
{
	bool endofwal = false;
	XLogRecPtr startReceivingFrom;
	ReplMessage *msg = xfalloc(sizeof(ReplMessage));
	FilterData *fl = WbFCreateProcessingState(cmd->startpoint);


	/* TODO: refactor this into wbfilter */
	if (conn->include_tablespaces)
	{
		xf_info("Including tablespaces: %s", conn->include_tablespaces);
		fl->include_tablespaces = WbCCFindTablespaceOids(conn);
	} else {
		fl->include_tablespaces = NULL;
	}


	startReceivingFrom = cmd->startpoint;
again:
	WbMcStartStreaming(master, startReceivingFrom, cmd->timeline);

	WbCCSendCopyBothResponse(conn);

	while (!endofwal)
	{
		WbCCProcessRepliesIfAny(conn);

		ConnFlush(conn);

		if (conn->copyDoneSent && conn->copyDoneReceived)
			break;

		if (WbMcReceiveWalMessage(master, NAPTIME, msg))
		{
			do {
				if (msg->type == MSG_END_OF_WAL)
				{
					// TODO: handle end of wal
					xf_info("End of WAL\n");
					endofwal = true;
					break;
				}
				if (msg->type == MSG_WAL_DATA)
				{
					XLogRecPtr restartPos;
					if (!WbFProcessWalDataBlock(msg, fl, &restartPos))
					{
						TimeLineID tli;
						WbMcEndStreaming(master, &tli);
						Assert(tli == 0);
						startReceivingFrom = restartPos;
						goto again;
					}
					WbCCSendWalBlock(conn, msg, fl);
				}
			} while (WbMcReceiveWalMessage(master, 0, msg));
		}
	}
	{
		TimeLineID tli;
		WbMcEndStreaming(master, &tli);
	}

	// query master server for data
	// send CopyBoth to client
	//     'W' b0 h0
	// flush
	// enter loop
	//     fetch data from master
	//		handle master keepalives
	//     process replies from client
	//			copy done
	//				send 'c'
	//			data
	//				replication status
	//				hot standby feedback
	//			exit
	//	   filter data
	//		check for timeout
	//	   push data out
	//			need to ensure aligned on record or page boundary
	//			'd' 'w' l(dataStart) l(walEnd) l(sendTime) s[WALdata]
	//			send keepalive if necessary
	//				'd' 'k' l(sentPtr) l(sentTime) b(RequestReply)
	// sendTimelineIsHistoric
	//	'T' h(2)
	//		s("next_tli") i(0) h(0) i(INT8OID) h(-1) i(0) h(2)
	//		s("next_tli_startpos") i(0) h(0) i(TEXTOID) h(-1) i(0) h(2)
	//	'D' h(2)
	//		i(strlen(tli_str)) p(tli_str)
	//		i(strlen(startpos_str)) p(startpos_str)
	// pq_puttextmessage('C', "START_STREAMING")
	WbFFreeProcessingState(fl);
	xffree(msg);
}

static void
WbCCExecTimeline()
{
	error("Timeline");
	// query master server, write out copy data
}

static Oid*
WbCCFindTablespaceOids(XfConn conn)
{
	// TODO: take in other options
	char conninfo[MAX_CONNINFO_LEN+1];
	char *buf = conninfo;
	char *buf_end = &(conninfo[MAX_CONNINFO_LEN]);

	memset(conninfo, 0, sizeof(conninfo));

	if (conn->master_host) {
		buf += snprintf(buf, buf_end - buf, "host=%s ", conn->master_host);
	}

	if (conn->master_port)
		buf +=  snprintf(buf, buf_end - buf, "port=%s ", conn->master_port);

	if (conn->user_name)
		buf += snprintf(buf, buf_end - buf, "user=%s ", conn->user_name);

	buf += snprintf(buf, buf_end - buf, "dbname=postgres application_name=walbouncer");

	return WbMcResolveTablespaceOids(conninfo, conn->include_tablespaces);
}
/* TODO: Probably not necessary
static void
WbCCSendWALRecord(XfConn conn, char *data, int len, XLogRecPtr sentPtr, TimestampTz lastSend)
{
	xf_info("Sending out %d bytes of WAL\n", len);
	ConnBeginMessage(conn, 'd');
	ConnSendBytes(conn, data, len);
	ConnEndMessage(conn);
	conn->sentPtr = sentPtr;
	conn->lastSend = lastSend;
	ConnFlush(conn);
}*/

static void
WbCCSendEndOfWal(XfConn conn)
{
	ConnBeginMessage(conn, 'c');
	ConnEndMessage(conn);
	//ConnFlush(conn);
	conn->copyDoneSent = true;
}

static void
WbCCProcessRepliesIfAny(XfConn conn)
{
	char firstchar;
	int r;

	// TODO: record last receive timestamp here

	for (;;)
	{
		r = ConnGetByteIfAvailable(conn, &firstchar);
		if (r < 0)
		{
			error("Unexpected EOF from receiver");
		}
		if (r == 0)
			break;

		if (conn->copyDoneReceived && firstchar != 'X')
			error("Unexpected standby message type \"%c\", after receiving CopyDone",
					firstchar);

		switch (firstchar)
		{
			case 'd':
				WbCCProcessReplyMessage(conn);
				break;
			case 'c':
				if (!conn->copyDoneSent)
					WbCCSendEndOfWal(conn);
				// consume the CopyData message
				{
					XfMessage *msg;
					if (ConnGetMessage(conn, &msg))
						error("Unexpected EOF on standby connection");
					ConnFreeMessage(msg);
				}
				conn->copyDoneReceived = true;
				break;
			case 'X':
				error("Standby is closing the socket");
			default:
				error("Invalid standby message");
		}
	}
}

static void
WbCCProcessReplyMessage(XfConn conn)
{
	XfMessage *msg;
	if (ConnGetMessage(conn, &msg))
		error("unexpected EOF from receiver");

	switch (msg->data[0])
	{
		case 'r':
			WbCCProcessStandbyReplyMessage(conn, msg);
			break;
		case 'h':
			WbCCProcessStandbyHSFeedbackMessage(conn, msg);
			break;
		default:
			error("Unexpected message type");
	}

	ConnFreeMessage(msg);
}

static void
WbCCProcessStandbyReplyMessage(XfConn conn, XfMessage *msg)
{
	XLogRecPtr	writePtr,
				flushPtr,
				applyPtr;
	bool		replyRequested;

	/* the caller already consumed the msgtype byte */
	writePtr = fromnetwork64(msg->data + 1);
	flushPtr = fromnetwork64(msg->data + 9);
	applyPtr = fromnetwork64(msg->data + 17);
	(void) fromnetwork64(msg->data + 25);		/* sendTime; not used ATM */
	replyRequested = msg->data[33];

	xf_info("Standby reply msg: write %X/%X flush %X/%X apply %X/%X%s\n",
		 (uint32) (writePtr >> 32), (uint32) writePtr,
		 (uint32) (flushPtr >> 32), (uint32) flushPtr,
		 (uint32) (applyPtr >> 32), (uint32) applyPtr,
		 replyRequested ? " (reply requested)" : "");

	/* Send a reply if the standby requested one. */
	if (replyRequested)
		WbCCSendKeepalive(conn, false);

	//TODO: send reply message forward
}

static void
WbCCSendKeepalive(XfConn conn, bool request_reply)
{
	xf_info("sending keepalive message %X/%X%s\n",
			(uint32) (conn->sentPtr>>32),
			(uint32) conn->sentPtr,
			request_reply ? " (reply requested)" : "");

	ConnBeginMessage(conn, 'd');
	ConnSendInt(conn, 'k', 1);
	ConnSendInt64(conn, conn->sentPtr);
	ConnSendInt64(conn, conn->lastSend);
	ConnSendInt(conn, request_reply ? 1 : 0, 1);
	ConnEndMessage(conn);
	ConnFlush(conn);
}

static void
WbCCProcessStandbyHSFeedbackMessage(XfConn conn, XfMessage *msg)
{
	TransactionId feedbackXmin;
	uint32		feedbackEpoch;
	/*
	 * Decipher the reply message. The caller already consumed the msgtype
	 * byte.
	 */
	(void) fromnetwork64(msg->data + 1);		/* sendTime; not used ATM */
	feedbackXmin = fromnetwork32(msg->data + 9);
	feedbackEpoch = fromnetwork32(msg->data + 13);

	xf_info("hot standby feedback xmin %u epoch %u\n",
		 feedbackXmin,
		 feedbackEpoch);

	// TODO: arrange for the feedback to be forwarded to the master
}


static void
WbCCSendCopyBothResponse(XfConn conn)
{
	/* Send a CopyBothResponse message, and start streaming */
	ConnBeginMessage(conn, 'W');
	ConnSendInt(conn, 0, 1);
	ConnSendInt(conn, 0, 2);
	ConnEndMessage(conn);
	ConnFlush(conn);
}

static void
WbCCSendWalBlock(XfConn conn, ReplMessage *msg, FilterData *fl)
{
	XLogRecPtr dataStart;
	int msgOffset = 0;
	int buffered = 0;
	int unsentLen = 0;
	char unsentBuf[FL_BUFFER_LEN];

	// Take a local copy of the unsent buffer in case we need to rewrite it
	if (fl->unsentBufferLen) {
		unsentLen = fl->unsentBufferLen;
		memcpy(unsentBuf, fl->unsentBuffer, unsentLen);
		xf_info("Sending %d bytes of unbuffered data", unsentLen);
	}

	//'d' 'w' l(dataStart) l(walEnd) l(sendTime) s[WALdata]
	if (fl->state == FS_BUFFER_RECORD || fl->state == FS_BUFFER_FILENODE)
	{
		// Chomp the buffered data off of what we send
		buffered = fl->bufferLen;
		// Stash it away into fl state, we will send it with the next block
		fl->unsentBufferLen = fl->bufferLen;
		memcpy(fl->unsentBuffer, fl->buffer, fl->bufferLen);
		// Make note that record starts in the unsent buffer for rewriting
		fl->recordStart = -1;
		xf_info("Buffering %d bytes of data", buffered);

	} else {
		// Clear out unsent buffer
		fl->unsentBufferLen = 0;
	}

	// Don't send anything if we are not synchronized, we will see this data again after replication restart
	if (!fl->synchronized)
	{
		xf_info("Skipping sending data.");
		return;
	}

	// Include the previously unsent data
	dataStart = msg->dataStart - unsentLen;

	if (fl->requestedStartPos > dataStart) {
		if (fl->requestedStartPos > (msg->dataStart + msg->dataLen))
		{
			xf_info("Skipping whole WAL message as not requested");
			return;
		}
		msgOffset = fl->requestedStartPos - dataStart;
		dataStart = fl->requestedStartPos;
		Assert(msgOffset < (msg->dataLen + unsentLen));
		xf_info("Chomping WAL message down to size at %d", msgOffset);
	}

	xf_info("Sending data start %X/%X", FormatRecPtr(dataStart));

	ConnBeginMessage(conn, 'd');
	ConnSendInt(conn, 'w', 1);
	ConnSendInt64(conn, dataStart);
	ConnSendInt64(conn, msg->walEnd - buffered);
	ConnSendInt64(conn, msg->sendTime);
	xf_info("Sending out %d bytes of WAL\n", msg->dataLen - msgOffset - buffered);

	if (unsentLen && msgOffset < unsentLen) {
		xf_info("Unsent data contents at offset %d, %d bytes:", msgOffset, unsentLen-msgOffset);
		hexdump(unsentBuf + msgOffset, unsentLen);
		ConnSendBytes(conn, unsentBuf + msgOffset, unsentLen-msgOffset);
		msgOffset = msgOffset < unsentLen ? 0 : msgOffset - unsentLen;
	}

	ConnSendBytes(conn, msg->data + msgOffset, msg->dataLen - msgOffset - buffered);
	ConnEndMessage(conn);

	conn->sentPtr = msg->dataStart + msg->dataLen - buffered;
	conn->lastSend = msg->sendTime;
	ConnFlush(conn);
}
#ifndef	_WB_MASTERCONN_H
#define _WB_MASTERCONN_H 1

#include "xfglobals.h"

typedef enum {
	MSG_NOTHING,
	MSG_END_OF_WAL,
	MSG_WAL_DATA,
	MSG_KEEPALIVE
} WalMsgType;

typedef struct {
	WalMsgType type;
	XLogRecPtr walEnd;
	TimestampTz sendTime;
	bool replyRequested;
	XLogRecPtr dataStart;

	int dataPtr;
	int dataLen;
	int nextPageBoundary;

	char *data;

} ReplMessage;

typedef struct MasterConn MasterConn;

MasterConn* WbMcOpenConnection(const char *conninfo);
bool WbMcStartStreaming(MasterConn *master, XLogRecPtr pos, TimeLineID tli);
void WbMcEndStreaming(MasterConn *master, TimeLineID *next_tli);
bool WbMcReceiveWalMessage(MasterConn *master, int timeout, ReplMessage *msg);
bool WbMcIdentifySystem(MasterConn* master,
		char** primary_sysid, char** primary_tli, char** primary_xpos);
Oid * WbMcResolveTablespaceOids(const char *conninfo, const char* tablespace_names);
const char *WbMcParameterStatus(MasterConn *master, char *name);
#endif

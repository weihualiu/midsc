#include <sccli.h>
#include <scsrv.h>
#include <json_pack.h>
#include "scpool.h"

#include "logs.stu"

typedef struct {
	char devid[17];
	char operid[17];
	T_Connect *server;
	char ShowID[100];
	char DBLABEL[81];
	JSON_OBJECT err_json;
	T_Connect *conn;
	char *data;
} GDA;

int Transfer(T_Connect *conn, T_NetHead *NetHead);
int login(T_Connect *conn,T_NetHead *NetHead);
int dmmgr(T_Connect *conn,T_NetHead *head);
int dmapp(T_Connect *conn,T_NetHead *head);
int notify(T_Connect *conn,T_NetHead *head);

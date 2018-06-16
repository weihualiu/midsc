#include <ctype.h>
#include <unistd.h>
#include "midsc.h"
#include <bignum.h>
#include <dw.h>
#include <sys/resource.h>

/***************************************************
 * Obj.txt:
 * OBJNO server|port|nextobjno|nextMTU|nextFamily|
 ***************************************************/
void quit(int);
int uabin(char *p);
char *phex(char *p,int len);
extern char *getenv();

int net_login(T_Connect *conn, T_NetHead *NetHead);

extern u_int family[];

srvfunc Function[]={
	{login,"login"},			/*0*/
	{Echo,"Echo"},
	{dmapp,"dmapp"},
	{dmmgr,"dmmgr"},
//	{notify,"notify"},
	{0,0}
};
static char myshowid[200];

static void freevar(void *p)
{
T_SRV_Var *srvp=(T_SRV_Var *)p;
GDA *gp;
	if(srvp) {
		gp=(GDA *)srvp->var;
		if(gp && gp->server) {
		T_CLI_Var *clip;
ShowLog(1,"%s:TCB:%d,poolno=%d,tid=%lu",__FUNCTION__,srvp->TCB_no,srvp->poolno,pthread_self());
			clip=(T_CLI_Var *)gp->server->Var;
			if(!clip) return;
			clip->Errno=-1;//要求挂断连接
			release_SC_connect(&gp->server,srvp->poolno);
		}
	}
}

void netinit(T_Connect *conn,T_NetHead *head)
{
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)srvp->var;
char addr[16],*envp;
	
	envp=getenv("SENDSIZE");
	if(envp && isdigit(*envp)) conn->MTU=atoi(envp);
	envp=getenv("TIMEOUT");
	if(envp && isdigit(*envp)) srvp->o_timeout=conn->timeout=60*atoi(envp);
	else srvp->o_timeout=conn->timeout=0;
	peeraddr(conn->Socket,addr);
	ShowLog(2,"连接 %s,TCB:%d,timeout=%d",addr,srvp->TCB_no,conn->timeout);
	gp->server=NULL;
	gp->data=NULL;
	gp->err_json=NULL;
	gp->conn=NULL;
	conn->only_do=Transfer;
	conn->freevar=freevar;
}

int main(int ac,char *av[])
{
int i;
struct rlimit sLimit;

//设置可以core dumpped
	sLimit.rlim_cur = -1;
	sLimit.rlim_max = -1;
	i=setrlimit(RLIMIT_CORE,(const struct rlimit *)&sLimit);

	if(ac>1){
		envcfg(av[1]);
	}
	sprintf(myshowid,"%s:%d",sc_basename(av[0]),getpid());
	Showid=myshowid;
	i=scpool_init(0);
	if(i<=0) {
		ShowLog(1,"scpool_init err=%d",i);
		return 1;
	}
	ShowLog(2,"scpool %d",i);
	TPOOL_srv(netinit,quit,scpool_check,sizeof(GDA));
	quit(0);
	return 0;
}

void quit(int n)
{
	ShowLog(0,"quit %d!\n",n);
/***********************************
 * close logfile
 ***********************************/
	if(n<0) n=-n;
	wpool_free();
	scpool_free();
	tpool_free();
	ShowLog(-1,0);
	exit(n&255);
}

#ifdef __cplusplus
extern "C"
#endif
void set_showid(void *ctx)
{
GDA *gp=(GDA *)ctx;
pthread_t tid=pthread_self();
	if(!ctx) return;
	mthr_showid_add(tid,gp->ShowID);
}

int bind_sc(int TCBno,T_Connect *conn)
{
GDA *gp;

	gp=(GDA *)get_TCB_ctx(TCBno);
	if(gp==NULL) return -1;
	if(gp->server) {
		ShowLog(1,"%s:TCB:%d server not empty!",__FUNCTION__,TCBno);
		return -2;
	}
//	ShowLog(5,"%s:TCB:%d,conn=%016X,showid=%s",__FUNCTION__,TCBno,(long)conn,gp->ShowID);
	gp->server=conn;
	return 0;
}
int unbind_sc(int TCBno)
{
GDA *gp;

	gp=(GDA *)get_TCB_ctx(TCBno);
	if(gp==NULL) return -1;
	gp->server=NULL;
	return 0;
}

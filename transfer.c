/****************************************
 * 一个串珠模型 
 ***************************************/
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include "midsc.h"

extern void quit(int n);
static int from_server(T_Connect *client,T_NetHead *NetHead);

int from_client(T_Connect *client,T_NetHead *NetHead)
{
T_SRV_Var *srvp=(T_SRV_Var *)client->Var;
GDA *gp=(GDA *)srvp->var;
int i,ret;
T_Connect *server; 
char tmp[256];
int proto_num;
unsigned int t_cont;

	unset_callback(srvp->TCB_no);
	server=gp->server;
	if(!server) {
		ShowLog(1,"%s:server fault!",__FUNCTION__);
		return -1;
	}
	i=get_event_status(srvp->TCB_no);
	if(i!=EPOLLIN) { 
		sprintf(tmp,"客户端状态超时,event=0X%08X", i);
clierr:
		client->status=0;
		((T_CLI_Var *)gp->server->Var)->Errno=-1;//状态夭折了，必须关闭连接
		release_SC_connect(&gp->server,srvp->poolno);
		ShowLog(1,"%s:tid=%lX,TCB:%d at clierr,%s",__FUNCTION__,
			srvp->tid,srvp->TCB_no,tmp);
		return -1;
	}
	i=RecvPack(client,NetHead);
	if(i<0){
		sprintf(tmp,"Recv client fail ret=%d,errno=%d,%s",
			i,errno,strerror(errno));
		goto clierr;
	} 
	t_cont=NetHead->ERRNO2;
	proto_num=NetHead->PROTO_NUM;
	i=SendPack(server,NetHead);
	ShowLog(3,"%s:TCB:%d,tid=%lX,Send to serv proto_num=0X%04X,t_cont=%d,SendPack ret=%d,pos=%08X",
		__FUNCTION__,srvp->TCB_no,srvp->tid,proto_num,t_cont,i,gp->server->pos);
	if(i){
		ret=0;
		sprintf(tmp,"Send to %s fail ret=%d,errno=%d,%s",
			server->Host,i,errno,strerror(errno));
		client->status=0;
		((T_CLI_Var *)gp->server->Var)->Errno=-1;//状态夭折了，必须关闭连接
		release_SC_connect(&gp->server,srvp->poolno);
		if(t_cont!=PACK_NOANSER) {
			NetHead->ERRNO1=errno;
			NetHead->ERRNO2=PACK_NOANSER;
			NetHead->O_NODE=LocalAddr(client->Socket,NULL);
			NetHead->data=tmp;
			NetHead->PKG_LEN=strlen(NetHead->data);
			i=SendPack(client,NetHead);
		}
		ShowLog(1,"%s:tid=%lX,TCB:%d at errret,%s",__FUNCTION__,
			srvp->tid,srvp->TCB_no,tmp);
		return ret;
	} 
	if(t_cont==PACK_NOANSER) {
		client->status=0;
		release_SC_connect(&gp->server,srvp->poolno);
		ShowLog(2,"%s:tid=%lX,TCB:%d,PACK_NOANSER",__FUNCTION__,srvp->tid,srvp->TCB_no);
		return 0;
	}  
	if(t_cont==PACK_CONTINUE) {
		set_callback(srvp->TCB_no,from_client,60);
		return 0;
	}
	if(t_cont != PACK_STATUS) client->status=0;
//下一个珠子是:from_server,下一个链子是：主任务队列
	i=set_event(srvp->TCB_no,server->Socket,from_server,60);
	if(i) {
		release_SC_connect(&gp->server,srvp->poolno);
		ShowLog(1,"%s:TCB:%d set_event error %d",__FUNCTION__,
			srvp->TCB_no,i);
		return -1;
	}
	return THREAD_ESCAPE;//释放本线程
}

//第三个珠子；从服务器收包发往客户端
static int from_server(T_Connect *client,T_NetHead *NetHead)
{
int i,mflg=0;
unsigned t_cont;
char tmp[256];
T_SRV_Var *srvp=(T_SRV_Var *)client->Var;
int ret=0;
int proto_num=NetHead->PROTO_NUM&0xFFFF;
	if(!srvp) {
		NetHead->ERRNO1=-1;
		NetHead->ERRNO2=-1;
		sprintf(tmp,"srvp is null");
		ret=-1;
err2:
		ShowLog(1,"%s:tid=%lX,%s",__FUNCTION__,pthread_self(),tmp);
		client->CryptFlg &= ~UNDO_ZIP;
		NetHead->data=tmp;
		NetHead->PKG_LEN=strlen(NetHead->data);
		NetHead->PKG_REC_NUM=0;
		NetHead->O_NODE=LocalAddr(client->Socket,0);
		client->CryptFlg &= ~UNDO_ZIP;
		i=SendPack(client,NetHead);
		return ret;
	}
GDA *gp=(GDA *)srvp->var;

	if(!gp) {
		sprintf(tmp,"ctx is null");
err1:
		NetHead->ERRNO2=-1;
		if(-2==clr_event(srvp->TCB_no)) {
			ShowLog(1,"%s:TCB:%d gp empty 清除事件错!",__FUNCTION__,srvp->TCB_no);
		}
		NetHead->ERRNO1=errno;
 	   	ShowLog(1,"%s:tid=%lX,TCB:%d %s",__FUNCTION__,srvp->tid,srvp->TCB_no,tmp);
		ret=-1;
		goto err2;
	}
T_Connect *server=gp->server;
	if(!server) {
		sprintf(tmp,"event fd=%d, server is null!", get_event_fd(srvp->TCB_no));
		ret=-1;
		goto err1;
	}
	i=get_event_status(srvp->TCB_no);
	if(i!=EPOLLIN) { 
		sprintf(tmp,"TCB_no=%d,呼叫服务器%s/%s返回超时,event=0X%08X",
			srvp->TCB_no,gp->server->Host,gp->server->Service,i);
		NetHead->ERRNO1=PACK_NOANSER;
		NetHead->ERRNO2=-1;
		((T_CLI_Var *)server->Var)->Errno=-1;
err:
		if(-2==clr_event(srvp->TCB_no)) {
			ShowLog(1,"%s:TCB:%d err 清除事件错!",__FUNCTION__,srvp->TCB_no);
		}
		release_SC_connect(&gp->server,srvp->poolno);
		ret=0;
		goto err2;
	}
	i=RecvPack(server,NetHead);
	if(i<0){
		sprintf(tmp,"recv from server %s fail i=%d,errno=%d,%s",
			server->Host,i,errno,strerror(errno));
		NetHead->ERRNO1=PACK_NOANSER;
		NetHead->ERRNO2=errno;
		((T_CLI_Var *)gp->server->Var)->Errno=-1;
		goto err;
	} 
	t_cont=NetHead->ERRNO2;
	if(t_cont==PACK_STATUS) client->status=1;//由服务器发起的状态，必须由客户端维持和撤销.
	else if(t_cont==PACK_NOANSER) client->status=0;//服务器也可以撤销状态.
	if(t_cont!=PACK_CONTINUE || client->status) {
//下一个珠子是:SDBC标准接收,下一个链子是：主任务队列
		i=clr_event(srvp->TCB_no);
		if(-2==i) {
			ShowLog(1,"%s:TCB:%d,清除事件错!",__FUNCTION__,srvp->TCB_no);
		}
		else if(client->status) { //有状态，不释放资源继续收client 
//下一个珠子是:from_client,下一个链子是：主任务队列
			ShowLog(3,"%s:TCB:%d,tid=%lX,Status Recv from server %s PROTO_NUM=0X%04X,"
		  		"ERRNO1=%d,ERRNO2=%d,PKG_LEN=%d,T_LEN=%d,t_cont=%08X,pos=%08X",
					__FUNCTION__,srvp->TCB_no,srvp->tid,
					server->Host,proto_num, NetHead->ERRNO1, NetHead->ERRNO2,
					NetHead->PKG_LEN,NetHead->T_LEN,t_cont, gp->server->pos);
			if(0!=(i=SendPack(client,NetHead)) ||
			   0!=(i=set_event(srvp->TCB_no,client->Socket,from_client,60))) {
				ShowLog(1,"%s:TCB:%d set_event error %d",__FUNCTION__,
			 		srvp->TCB_no,i);
				release_SC_connect(&gp->server,srvp->poolno);
				return -1;
			}
			return THREAD_ESCAPE;//释放本线程
		}
//整个串珠的流程已经完成,释放公共资源。
		if(NetHead->PKG_LEN) {//RecvBuffer给client留着。
			server->RecvLen=0;
			server->RecvBuffer=NULL;
			mflg=1;
		}
		release_SC_connect(&gp->server,srvp->poolno);
	}
	i=SendPack(client,NetHead);
	ShowLog(3,"%s:TCB:%d,tid=%lX,Recv from server %s PROTO_NUM=0X%04X,"
		  "ERRNO1=%d,ERRNO2=%d,PKG_LEN=%d,T_LEN=%d,t_cont=%08X,pos=%08X",
			__FUNCTION__,srvp->TCB_no,srvp->tid,
			server->Host,proto_num, NetHead->ERRNO1, NetHead->ERRNO2,
			NetHead->PKG_LEN,NetHead->T_LEN,t_cont,server->pos);

	if(mflg) {//现在释放server的RecvBuffer。
		free(NetHead->data);
		NetHead->data=NULL;
		NetHead->PKG_LEN=0;
	}
	return 0;
}
//第二个珠子：从客户端收包，发往服务器
int do_Transfer(T_Connect *client,T_NetHead *NetHead)
{
T_SRV_Var *srvp=(T_SRV_Var *)client->Var;
	if(!srvp) {//该客户端已被关闭
		ShowLog(1,"%s:该客户端已被关闭",__FUNCTION__);
		return -1;
	}
GDA *gp=(GDA *)srvp->var;
int i;
T_Connect *server; 
char tmp[256];
unsigned int t_cont;

	unset_callback(srvp->TCB_no);
	server=gp->server;
	if(!server) {
		sprintf(tmp,"Server connect fault tid=%lX!TCB_no=%d",
			pthread_self(),srvp->TCB_no);
		NetHead->ERRNO1=PACK_NOANSER;
		NetHead->ERRNO2=-1;
		NetHead->O_NODE=LocalAddr(client->Socket,NULL);
		NetHead->data=tmp;
		NetHead->PKG_LEN=strlen(NetHead->data);
		i=SendPack(client,NetHead);
		ShowLog(1,"%s:%s",__FUNCTION__,tmp);
		return 0;
	}
	if(client->CryptFlg & UNDO_ZIP) server->CryptFlg |= UNDO_ZIP;
	t_cont=NetHead->ERRNO2;
	i=SendPack(server,NetHead);
	ShowLog(3,"%s:TCB:%d,tid=%lX,Send to serv proto_num=0X%04X,t_cont=%d,SendPack ret=%d,pos=%08X",
		__FUNCTION__,srvp->TCB_no,srvp->tid,NetHead->PROTO_NUM&0xFFFF,t_cont,i,server->pos);
	if(i){
		sprintf(tmp,"Send to %s fail ret=%d,errno=%d,%s",
			server->Host,i,errno,strerror(errno));
		((T_CLI_Var *)gp->server->Var)->Errno=-1;
		release_SC_connect(&gp->server,srvp->poolno);
		if(t_cont != PACK_NOANSER) {
			NetHead->ERRNO1=errno;
			NetHead->ERRNO2=PACK_NOANSER;
			NetHead->O_NODE=LocalAddr(client->Socket,NULL);
			NetHead->data=tmp;
			NetHead->PKG_LEN=strlen(NetHead->data);
			client->CryptFlg &= ~UNDO_ZIP;
			i=SendPack(client,NetHead);
		}
		ShowLog(1,"%s:tid=%lX,TCB:%d at errret,%s",__FUNCTION__,
			srvp->tid,srvp->TCB_no,tmp);
		return 0;
	} 
	if(t_cont==PACK_CONTINUE) {
		set_callback(srvp->TCB_no,from_client,60);
		return 0;
	}
	if(t_cont==PACK_NOANSER) {
		ShowLog(2,"%s:tid=%lX,TCB:%d,PACK_NOANSER",__FUNCTION__,srvp->tid,srvp->TCB_no);
		release_SC_connect(&gp->server,srvp->poolno);
		return 0;
	}  
//下一个珠子是:from_server,下一个链子是：主任务队列
	i=set_event(srvp->TCB_no,server->Socket,from_server,120);
	if(i) {
		ShowLog(1,"%s:TCB:%d set_event error %d",__FUNCTION__,
			srvp->TCB_no,i);
		return -1;
	}
	return THREAD_ESCAPE;//释放本线程
}
//第一个珠子：来自SDBC主接收程序，取连接池
int Transfer(T_Connect *client,T_NetHead *NetHead)
{
T_SRV_Var *srvp=(T_SRV_Var *)client->Var;
GDA *gp;
int ret,TCBno;
char tmp[256];
	
	if(!srvp) {
		sprintf(tmp,"srvp is null");
		NetHead->ERRNO1=-1;
	} else {
		gp=(GDA *)srvp->var;
		if(NetHead->ERRNO2==PACK_STATUS)
			client->status=1;
		TCBno=srvp->TCB_no;
//下一个珠子是:do_Transfer,下一个链子是：连接池等待队列
		ret=scpool_MGR(TCBno,srvp->poolno,&gp->server,do_Transfer);
		if(0==ret) return do_Transfer(client,NetHead);//池已经有了，直接调用
		if(ret==1) return THREAD_ESCAPE; //释放本线程
		sprintf(tmp,"get connect pool fail!");
		NetHead->ERRNO2=PACK_NOANSER;
	}

	NetHead->ERRNO1=-1;
   	ShowLog(1,"%s:%s",__FUNCTION__,tmp);
	NetHead->O_NODE=LocalAddr(client->Socket,NULL);
	NetHead->data=tmp;
	NetHead->PKG_LEN=strlen(NetHead->data);
	ret=SendPack(client,NetHead);
	return 0;
}


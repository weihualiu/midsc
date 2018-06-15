#include <unistd.h>
#include "midsc.h"
#include <BB_tree.h>
#include "rqueue.h"

static pthread_rwlock_t treelock = PTHREAD_RWLOCK_INITIALIZER;
static T_Tree *task_tree=NULL;

static volatile int busy=0;

static int not_cmp(void *s,void *d,int len)
{
dlq_node *sp,*dp;
	sp=(dlq_node *)s;
	dp=(dlq_node *)d;
	if(sp->TCB_no > dp->TCB_no) return 1;
	if(sp->TCB_no != dp->TCB_no) return -1;
	return 0;
}

static int not_dup(T_Tree *s,void *Content,int len)
{
dlq_node *sp,*dp;
	sp=(dlq_node *)s->Content;
	dp=(dlq_node *)Content;
	sp->num += dp->num;
	return 0;
}

static int myfinish(dlq_node *np,int flg)
{
dlq_node *dp;
T_Tree *treep;
int tflg=0;
	pthread_rwlock_wrlock(&treelock);
	treep=BB_Tree_Find(task_tree,np,sizeof(dlq_node),not_cmp);
	if(!treep) {
		pthread_rwlock_unlock(&treelock);
		free(np);
		return -1;
	}
	dp=(dlq_node *)treep->Content;
	if(!dp) {
		pthread_rwlock_unlock(&treelock);
		ShowLog(1,"%s:dp empty!",__FUNCTION__);
		return -1;
	}
	if(--dp->num == 0) {
		task_tree=BB_Tree_Del(task_tree,dp,sizeof(dlq_node),not_cmp,NULL,&tflg);
		ShowLog(2,"%s:TCB_no=%u finish",__FUNCTION__,dp->TCB_no);
		tflg=1;
	}
	pthread_rwlock_unlock(&treelock);
	free(np);
	return tflg;
}

int recv_server(dlq_node *ctxp)
{
T_CLI_Var *clip;
T_SRV_Var *srvp;
GDA *gp;
T_NetHead Head;
int ret,TCB_no;
char msg[512];
T_Connect *conn,*sconn;

	if(!ctxp) {
		ShowLog(1,"%s:ctxp is null",__FUNCTION__);
		return -1;
	}
	TCB_no=ctxp->TCB_no;
	ShowLog(2,"%s:TCB_no=%d",__FUNCTION__,TCB_no);
	sconn=get_TCB_connect(TCB_no);
	srvp=(T_SRV_Var *)sconn->Var;
	gp=(GDA *)srvp->var;
	if(!gp->err_json) gp->err_json=json_object_new_array();
	if(!ctxp->rs) { //timeout
		Head.ERRNO1=-1;
		Head.ERRNO2=0;
		json_object_array_add(gp->err_json,jerr(-1,"TIMEOUT"));
		if(ctxp->finish) ret=ctxp->finish(ctxp,-1);
		if(ret==1) {
			JSON_OBJECT json=json_object_new_object();
			json_object_object_add(json,"status",gp->err_json);
			gp->err_json=NULL;
			return_error(sconn,&Head,json_object_to_json_string(json));
			if(!busy) TCB_add(NULL,TCB_no);
			json_object_put(json);
		}
		return 1;
	}
	ctxp->rs->timeout_deal=NULL;
	conn=&ctxp->rs->Conn;
	clip=&ctxp->rs->cli;
	conn->only_do=NULL;

	ret=RecvPack(conn,&Head);
	if(ret<0) {
		json_object_array_add(gp->err_json,jerr(errno,strerror(errno)));
		clip->Errno=-1;
		clip->var=NULL;
		release_SC_resource(&ctxp->rs);
//rollback
		if(ctxp->finish) ret=ctxp->finish(ctxp,-1);
	} else {
		sprintf(msg,"%s:%s",conn->Host,Head.data);
		clip->var=NULL;
		release_SC_resource(&ctxp->rs);
		json_object_array_add(gp->err_json,jerr(Head.ERRNO1,msg));
//commit;
		if(ctxp->finish) ret=ctxp->finish(ctxp,Head.ERRNO1);
		ShowLog(3,"%s:%s,ERRNO1=%d,ret=%d,busy=%d",__FUNCTION__,msg,Head.ERRNO1,ret,busy);
	}
	if(ret==1) {
		JSON_OBJECT json=json_object_new_object(); json_object_object_add(json,"status",gp->err_json);
		gp->err_json=NULL;
		Head.ERRNO1=ret;
		Head.ERRNO2=0;
		return_error(sconn,&Head,json_object_to_json_string(json));
		if(!busy) TCB_add(NULL,TCB_no);//可能notify那边还没完，就重入了。
		json_object_put(json);
	}
	return 0;
}

static int myrs_timeout(resource *rs)
{
dlq_node *dlp;
	rs->timeout_deal=NULL;
	rs->Conn.timeout=0;
	dlp=(dlq_node *)rs->cli.var;
	if(dlp) {
		to_epoll(dlp,EPOLL_CTL_DEL,0);
	}
	rs->cli.Errno=-1;
	release_SC_resource(&rs);
	if(dlp) {
		dlp->rs=NULL;
		recv_server(dlp);
	}
	return 0;
}

int notify(T_Connect *conn,T_NetHead *head)
{
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)srvp->var;
int flg=0,i,ret,Dnode,pathnum,poolno;
const char *Rpc,*Data;
JSON_OBJECT json,json1,result;
T_NetHead Head;
T_CLI_Var *clip;
char msg[512];
int stat=head->ERRNO2;
dlq_node * ctxp;
resource *rs;

	if(!gp->err_json) gp->err_json=json_object_new_array();
	if(head->PKG_LEN<=0) {
		if(head->ERRNO2 != PACK_NOANSER) {
			json_object_array_add(gp->err_json,jerr(-2,"data empty!"));
			head->ERRNO1=-1;
err1:
	                result=json_object_new_object();
			json_object_object_add(result,"status",gp->err_json);
			gp->err_json=NULL;
			head->ERRNO2=conn->status?PACK_NOANSER:0;
			head->PKG_REC_NUM=0;
			return_error(conn,head,json_object_to_json_string(result));
			ShowLog(1,"%s:%s",__FUNCTION__,head->data);
			json_object_put(result);
			return 0;
		}
		ShowLog(1,"%s:data empty!",__FUNCTION__);
		return 0;
	}
	json=json_tokener_parse(head->data);
	if(!json) {
		json_object_array_add(gp->err_json,jerr(-3,"data format err!"));
		head->ERRNO1=-2;
		goto err1;
	}
	json1=json_object_object_get(json,"Dnode");
	if(!json1) {
		json_object_put(json);
		json_object_array_add(gp->err_json,jerr(-3,"format err,missing Dnode!"));
		head->ERRNO1=-2;
		goto err1;
	}
	Dnode=json_object_get_int(json1);
	poolno=get_scpool_no(Dnode);
	if(poolno<0) {
		sprintf(msg,"no such Dnode %d",Dnode);
		json_object_put(json);
		json_object_array_add(gp->err_json,jerr(-4,msg));
		head->ERRNO1=-2;
		goto err1;
	}
	pathnum=getPathNum(poolno);//这个dnode有多少个服务器
	if(pathnum < 1) {
		json_object_array_add(gp->err_json,jerr(-5,"pathnum empty!"));
		head->ERRNO1=-5;
		goto err1;
	}
	json1=json_object_object_get(json,"Rpc");
	if(!json1) {
		json_object_put(json);
		json_object_array_add(gp->err_json,jerr(-3,"format err,missing Rpc!"));
		head->ERRNO1=-2;
		goto err1;
	}
	Rpc=json_object_get_string(json1);
	json1=json_object_object_get(json,"Data");
	if(!json1) {
		json_object_put(json);
		json_object_array_add(gp->err_json,jerr(-3,"format err,missing Rpc!"));
		head->ERRNO1=-2;
		goto err1;
	}
	Data=json_object_get_string(json1);

	busy=1;
	for(i=0;i<pathnum;i++) {
		rs=get_path_resource(poolno,i,1);
		if(!rs||rs==(resource *)-1) continue;
		ShowLog(5,"%s:connect[%d] got!",__FUNCTION__,i);
//建立context，呼叫该节点
		rs->Conn.CryptFlg &= ~UNDO_ZIP;
		clip=&rs->cli;
		Head.PROTO_NUM=get_srv_no(clip,Rpc);
		if(1==Head.PROTO_NUM) {
			release_SC_resource(&rs);
			sprintf(msg,"Dnode[%d][%d] no such Rpc %s",Dnode,i,Rpc);
			json_object_array_add(gp->err_json,jerr(103,msg));
			continue;
		}
ShowLog(5,"%s:Rpc=%s,PROTO_NUM=%d",__FUNCTION__,Rpc,Head.PROTO_NUM);
		Head.D_NODE=0;  //不用
		Head.ERRNO1=0;
		Head.ERRNO2=stat;
		Head.PKG_REC_NUM=0;
		Head.O_NODE=clip->ctx_id;
		Head.data=(char *)Data;
		Head.PKG_LEN=strlen(Head.data);
		Head.ERRNO1=SendPack(&rs->Conn,&Head);
		if(!Head.ERRNO1&&stat != PACK_NOANSER) {
			ShowLog(5,"%s:notify[%d] sended!",__FUNCTION__,i);
			rs->Conn.timeout=60; //超时
			rs->timeout_deal=myrs_timeout;
			ctxp=(dlq_node *)malloc(sizeof(dlq_node));
			if(!ctxp) {
				rs->cli.Errno=-1;
				release_SC_resource(&rs);
				json_object_array_add(gp->err_json,jerr(MEMERR,"ctxp empty!"));
				continue;
			}
			memset(ctxp,0,sizeof(ctxp));
			rs->cli.var=ctxp;
			ctxp->TCB_no=srvp->TCB_no;
			ctxp->rs=rs;
			ctxp->timeout=ctxp->rs->Conn.timeout;
			ctxp->callback=recv_server;
			ctxp->finish=myfinish;
			ctxp->num=1;
			pthread_rwlock_wrlock(&treelock);
			task_tree=BB_Tree_Add(task_tree,ctxp,sizeof(dlq_node),not_cmp,not_dup);
			pthread_rwlock_unlock(&treelock);

			clip->var=ctxp;//登记发起者的context
			ret=to_epoll(ctxp,EPOLL_CTL_ADD,0);
			flg++;
		}
	}
	busy=0;
	json_object_put(json);
ShowLog(5,"%s:flg=%d return",__FUNCTION__,flg);
	if(flg>0) {
		return THREAD_ESCAPE;
	}
	if(stat != PACK_NOANSER) {
		json_object_array_add(gp->err_json,jerr(-21,"server empty!"));
		head->ERRNO1=-21;
		goto err1;
	}
	return 0;
}

					task->AIO_flg=0;
					task->AIO_flg=0;

/**************************************************
 * SDBC连接池管理
 **************************************************/
#include <ctype.h>
#include <sys/utsname.h>
#include <sys/epoll.h>
#include <bignum.h>
#include <arpa/inet.h>
#include <sccli.h>
#include <scsrv.h>
#include "scpool.h"
#include "scry.h"
#include "midsc.h"

#include <logs.tpl>
static int log_level=0;

T_PkgType SCPOOL_tpl[]={
        {CH_INT,sizeof(int),"d_node",0,-1},
        {CH_CHAR,17,"DEVID"},
        {CH_CHAR,256,"LABEL"},
        {CH_CHAR,17,"UID"},
        {CH_CHAR,14,"PWD"},
        {CH_INT,sizeof(int),"NUM"},
        {CH_INT,sizeof(int),"NEXT_d_node"},
        {CH_CHAR,81,"HOST"},
        {CH_CHAR,21,"PORT"},
        {CH_INT,sizeof(int),"MTU"},
        {CH_CHAR,172,"family"},
        {-1,0,0,0}
};

extern T_PkgType SCPOOL_tpl[];
typedef struct {
        int d_node;
        char DEVID[17];
        char LABEL[256];
        char UID[17];
        char PWD[14];
        int NUM;
        int NEXT_d_node;
        char HOST[81];
        char PORT[21];
        int MTU;
        char family[172];
} SCPOOL_stu;

typedef struct {
	int next;
	int TCBno;
	T_Connect Conn;
	T_CLI_Var cli;
	INT64 timestamp;
} resource;

typedef struct {
	pthread_mutex_t mut;
	pthread_cond_t cond;
	int d_node;
	int resource_num;
	SCPOOL_stu log;
	u_int family[32];
	svc_table svc_tbl;
	resource *lnk;
	int free_q;
	char DBLABEL[81];
}pool;

static int SCPOOLNUM=0;
static pool *scpool=NULL;
//释放连接池
void scpool_free()
{
int i,n;

	if(!scpool) return;
	for(n=0;n<SCPOOLNUM;n++) {
		pthread_cond_destroy(&scpool[n].cond);
		pthread_mutex_destroy(&scpool[n].mut);
		if(scpool[n].lnk) {
			for(i=0;i<scpool[n].resource_num;i++) {
			    if(scpool[n].lnk[i].Conn.Socket > -1) {
				disconnect(&scpool[n].lnk[i].Conn);
			    }
			    scpool[n].lnk[i].cli.ctx_id=0;
			}
			free(scpool[n].lnk);
		}
	}
	free(scpool);
	scpool=NULL;
}

//初始化连接池
int scpool_init()
{
int n,i,ret;
char *p,buf[512];
INT64 now;
FILE *fd;
JSON_OBJECT cfg,json;
SCPOOL_stu node;

	if(scpool) return 0;
	p=getenv("SCPOOLCFG");
	if(!p||!*p) {
		ShowLog(1,"%s:缺少环境变量SCPOOLCFG!",__FUNCTION__);
		return -1;
	}
	fd=fopen((const char *)p,"r");
	if(!fd) {
		ShowLog(1,"%s:CFGFILE %s open err=%d,%s",__FUNCTION__,
			p,errno,strerror(errno));
		return -2;
	}
	cfg=json_object_new_array();
	while(!ferror(fd)) {
		fgets(buf,sizeof(buf),fd);
		if(feof(fd)) break;
		TRIM(buf);
		if(!*buf || *buf=='#') continue;
		ret=net_dispack(&node,buf,SCPOOL_tpl);
		if(ret<=0) continue;
		json=json_object_new_object();
		struct_to_json(json,&node,SCPOOL_tpl,0);
		json_object_array_add(cfg,json);
	}
	fclose(fd);
	SCPOOLNUM=json_object_array_length(cfg);
	if(SCPOOLNUM <=0 ) {
		json_object_put(cfg);
		ShowLog(1,"%s:empty SCPOOL",__FUNCTION__);
		return -3;
	}
	scpool=(pool *)malloc(SCPOOLNUM * sizeof(pool));
	if(!scpool) {
		json_object_put(cfg);
		SCPOOLNUM=0;
		return MEMERR;
	}

	p=getenv("SCPOOL_LOGLEVEL");
	if(p && isdigit(*p)) log_level=atoi(p);

	now=now_usec();
    for(n=0;n<SCPOOLNUM;n++) {

	if(0!=(i=pthread_mutex_init(&scpool[n].mut,NULL))) {
		ShowLog(1,"%s:mutex_init err %s",__FUNCTION__,
			strerror(i));
		json_object_put(cfg);
		return -12;
	}

	if(0!=(i=pthread_cond_init(&scpool[n].cond,NULL))) {
		ShowLog(1,"%s:cond init  err %s",__FUNCTION__,
			strerror(i));
		json_object_put(cfg);
		return -13;
	}
	json=json_object_array_get_idx(cfg,n);
	json_to_struct(&scpool[n].log,json,SCPOOL_tpl);
	scpool[n].d_node=scpool[n].log.d_node;
	scpool[n].svc_tbl.srvn=-1;
	*scpool[n].DBLABEL=0;
	scpool[n].resource_num=scpool[n].log.NUM>0?scpool[n].log.NUM:1;
	scpool[n].lnk=(resource *)malloc(scpool[n].resource_num * sizeof(resource));
	if(!scpool[n].lnk) {
		ShowLog(1,"%s:malloc lnk error!",__FUNCTION__);
		scpool[n].resource_num=0;
		continue;
	}
	scpool[n].free_q=scpool[n].resource_num-1;
	for(i=0;i<scpool[n].resource_num;i++) {
		scpool[n].lnk[i].TCBno=-1;
		Init_CLI_Var(&scpool[n].lnk[i].cli);
		scpool[n].lnk[i].cli.Errno=-1;
		scpool[n].lnk[i].cli.NativeError=i;
		scpool[n].lnk[i].cli.ctx_id=0;
		initconnect(&scpool[n].lnk[i].Conn);
		strcpy(scpool[n].lnk[i].Conn.Host,scpool[n].log.HOST);
		strcpy(scpool[n].lnk[i].Conn.Service,scpool[n].log.PORT);
		scpool[n].lnk[i].Conn.pos=i;
		if(*scpool[n].log.family)
			str_a64n(32,scpool[n].log.family,scpool[n].family);
		if(i<scpool[n].resource_num-1) scpool[n].lnk[i].next=i+1;
                else scpool[n].lnk[i].next=0;
		scpool[n].lnk[i].timestamp=now;
	}
	ShowLog(2,"scpool[%d],link num=%d",n,scpool[n].resource_num);
    }
	json_object_put(cfg);
	return SCPOOLNUM;
}
static int lnk_no(pool *pl,T_Connect *conn)
{
int i,e;
resource *rs=pl->lnk;

        if(conn != &pl->lnk[conn->pos].Conn) {
         	ShowLog(1,"%s:conn not equal pos=%d",__FUNCTION__,
           	        	conn->pos);
        } else return conn->pos;
	e=pl->resource_num-1;
	for(i=0;i<=e;i++,rs++) {
		if(conn == &rs->Conn) {
			conn->pos=i;
			return i;
		}
	}
	return -1;

}
static int get_lnk_no(pool *pl)
{
int i,*ip,*np;
resource *rs;

        if(pl->free_q<0) return -1;
	ip=&pl->free_q;
	rs=&pl->lnk[*ip];
        i=rs->next;
	np=&pl->lnk[i].next;
        if(i==*ip) *ip=-1;
        else rs->next=*np;
        *np=-1;
        return i;
}
//将pl[i]加入free_q
static void add_lnk(pool *pl,int i)
{
int *np,*ip=&pl->lnk[i].next;
        if(*ip>=0) return;//已经在队列中
	np=&pl->free_q;
        if(*np < 0) {
                *np=i;
                *ip=i;
	} else { //插入队头
	resource *rs=&pl->lnk[*np];
                *ip=rs->next;
                rs->next=i;
		if(pl->lnk[i].Conn.Socket<0) {
			*np=i;//坏连接排队尾
		}
        }
}

static int log_ret(T_Connect *conn,T_NetHead *head)
{
int ret,event;
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
resource *rs;
pool *pl;
T_Connect *Conn;
T_NetHead Head;
sdbcfunc f;
log_stu logs;

	event=get_event_status(srvp->TCB_no);
	clr_event(srvp->TCB_no);
//ShowLog(5,"%s:tid=%lu,poolno=%d,entry TCB:%d",__FUNCTION__,pthread_self(),srvp->poolno,srvp->TCB_no);
	if(srvp->poolno < 0) {
		ShowLog(1,"%s:tid=%lu,poolno=%d,entry TCB:%d",__FUNCTION__,
			pthread_self(),srvp->poolno,srvp->TCB_no);
		unbind_sc(srvp->TCB_no);
		return -1;
	}
	pl=&scpool[srvp->poolno];
	ret=conn->pos;
	rs=&pl->lnk[ret];
	Conn=&rs->Conn;
	f=Conn->only_do;
	if(f!=(sdbcfunc)1) set_callback(srvp->TCB_no,f,Conn->timeout);
	Conn->only_do=NULL;
        if(event!=EPOLLIN) {
                ShowLog(1,"%s:呼叫服务器返回超时,rs[%d],event=%08X",__FUNCTION__,ret,event);
		unbind_sc(srvp->TCB_no);
		rs->cli.Errno=-1;
                release_SC_connect(&Conn,srvp->TCB_no,srvp->poolno);
		TCB_add(NULL,srvp->TCB_no); //加入到主任务队列
		return THREAD_ESCAPE;
        }

	ret=RecvPack(Conn,&Head);
	if(ret) { //网络失败
		rs->cli.Errno=errno;
		stptok(strerror(errno),rs->cli.ErrMsg,sizeof(rs->cli.ErrMsg),0);
		unbind_sc(srvp->TCB_no);
		ShowLog(1,"%s:network error %d,%s",__FUNCTION__,rs->cli.Errno,rs->cli.ErrMsg);
		rs->cli.Errno=-1;
		release_SC_connect(&Conn,srvp->TCB_no,srvp->poolno);
		TCB_add(NULL,srvp->TCB_no); //加入到主任务队列
		return THREAD_ESCAPE;
	}
	if(Head.ERRNO1 || Head.ERRNO2) {  //login失败
		ShowLog(1,"%s:HOST=%s,login error ERRNO1=%d,ERRNO2=%d,%s",__FUNCTION__,
			Conn->Host,Head.ERRNO1,Head.ERRNO2,Head.data);
errret:
		unbind_sc(srvp->TCB_no);
		stptok(Head.data,rs->cli.ErrMsg,sizeof(rs->cli.ErrMsg),0);
		rs->cli.Errno=-1;
		release_SC_connect(&Conn,srvp->TCB_no,srvp->poolno);
		TCB_add(NULL,srvp->TCB_no); //加入到主任务队列
		return THREAD_ESCAPE;
	}
	memset(&logs,0,sizeof(logs));
	net_dispack(&logs,Head.data,log_tpl);
	strcpy(rs->cli.DBOWN,logs.DBOWN);
	strcpy(rs->cli.UID,logs.DBUSER);
	if(!*pl->DBLABEL) strcpy(pl->DBLABEL,logs.DBLABEL);
//session id for end server
	rs->cli.ctx_id=atoi(logs.DBLABEL);

//取服务名
	rs->cli.svc_tbl=&pl->svc_tbl;
	pthread_mutex_lock(&pl->mut);
	if(pl->svc_tbl.srvn <= 0) {
	    ret=init_svc_no(&rs->Conn);
	    if(ret) { //取服务名失败
		pthread_mutex_unlock(&pl->mut);
		ShowLog(1,"%s:HOST=%s,init_svc_no error ERRNO1=%d,ERRNO2=%d,%s",__FUNCTION__,
			Conn->Host,Head.ERRNO1,Head.ERRNO2,Head.data);
		goto errret;
	    }
	} else {
		pl->svc_tbl.usage++;
                Conn->freevar=(void (*)(void *)) free_srv_list;
        }

	pthread_mutex_unlock(&pl->mut);
	rs->cli.Errno=0;
	*rs->cli.ErrMsg=0;
	return f(conn,head);
}

static int to_log_ret(T_Connect *conn,T_NetHead *head)
{
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
//从rdy队列转到epoll队列
	unset_callback(srvp->TCB_no);
int ret;
resource *rs;
pool *pl;
	if(srvp->poolno<0) {
		ShowLog(1,"%s:tid=%lu,poolno=%d,entry TCB:%d",__FUNCTION__,
			pthread_self(),srvp->poolno,srvp->TCB_no);
		unbind_sc(srvp->TCB_no);
		return -1;
	}
	pl=&scpool[srvp->poolno];

	ret=conn->pos;//从sc_connect传递过来
	rs=&pl->lnk[ret];
//任务将回到epoll队列
	set_event(srvp->TCB_no,rs->Conn.Socket,log_ret,60);
	return THREAD_ESCAPE;
}

//连接
static int sc_connect(pool *p1,resource *rs)
{
int ret=-1;
T_NetHead Head;
struct utsname ubuf;
char buf[200],*p;
T_Connect *cli_conn;
char finger[200];

	ret=Net_Connect(&rs->Conn,&rs->cli,*p1->log.family?p1->family:NULL);
	if(ret) {
		rs->cli.Errno=errno;
		stptok(strerror(errno),rs->cli.ErrMsg,sizeof(rs->cli.ErrMsg),0);
		return -1;
	}

	p=getenv("TIMEOUT");
	if(p && isdigit(*p)) {
		Head.ERRNO2=60*atoi(p);
	} else Head.ERRNO2=0;
	p=getenv("TCPTIMEOUT");
	if(p && isdigit(*p)) {
		rs->Conn.timeout=atoi(p);
	} else rs->Conn.timeout=0;

//login
	Head.O_NODE=LocalAddr(rs->Conn.Socket,finger);
	fingerprint(finger);
	uname(&ubuf);
	p=buf;
	p+=sprintf(p,"%s|%s|%s,%s|||",p1->log.DEVID,p1->log.LABEL,
		ubuf.nodename,finger);
	rs->Conn.MTU=p1->log.MTU;
	Head.PROTO_NUM=0;
	Head.D_NODE=p1->log.NEXT_d_node;
	Head.ERRNO1=rs->Conn.MTU;
	Head.PKG_REC_NUM=-1; //不要求ctx_id
	Head.data=buf;
	Head.PKG_LEN=strlen(Head.data);
	ret=SendPack(&rs->Conn,&Head);
ShowLog(5,"%s:Send %s ret%d",__FUNCTION__,Head.data,ret);
//任务将回到rdy队列
	cli_conn=get_TCB_connect(rs->TCBno);
	cli_conn->pos=rs->Conn.pos;//向下传递pos
	rs->Conn.only_do=set_callback(rs->TCBno,to_log_ret,rs->Conn.timeout);
	if(!rs->Conn.only_do) {
		rs->Conn.only_do=(sdbcfunc)1;
	}//没有进行TCB_add(),回去后做。
	return 0;
}

//取连接
T_Connect * get_SC_connect(int TCBno,int n,int flg)
{
int i,ret;
pool *pl;
resource *rs;
pthread_t tid=pthread_self();

	if(!scpool || n<0 || n>=SCPOOLNUM) return NULL;
	pl=&scpool[n];
	if(!pl->lnk) {
		ShowLog(1,"%s:无效的连接池[%d]",__FUNCTION__,n);
		return NULL;
	}
	if(0!=pthread_mutex_lock(&pl->mut))  return (T_Connect *)-1;
	while(0>(i=get_lnk_no(pl))) {
	    if(flg) {	//flg !=0,don't wait
		pthread_mutex_unlock(&pl->mut);
		return NULL;
	    }
//	    if(log_level) ShowLog(log_level,"%s tid=%lu pool[%d] suspend",
//		__FUNCTION__,pthread_self(),n);
	    pthread_cond_wait(&pl->cond,&pl->mut); //没有资源，等待
//	    if(log_level) ShowLog(log_level,"%s tid=%lu pool[%d] weakup",
//		__FUNCTION__,pthread_self(),n);
	}
	pthread_mutex_unlock(&pl->mut);
	rs=&pl->lnk[i];
	rs->cli.NativeError=i;
	rs->TCBno=TCBno;
	if(rs->Conn.Socket<0 || rs->cli.Errno<0) {
		ret=sc_connect(pl,rs);
		if(ret) {
			ShowLog(1,"%s:scpool[%d].%d 连接%s/%s错:err=%d,%s",
				__FUNCTION__,n,i,pl->log.HOST,pl->log.PORT,
				rs->cli.Errno, rs->cli.ErrMsg);
			rs->TCBno=-1;
			rs->cli.Errno=-1;
			pthread_mutex_lock(&pl->mut);
			add_lnk(pl,i);
			pthread_mutex_unlock(&pl->mut);
			return (T_Connect *)-1;
		}
	}
	rs->timestamp=now_usec();
	if(log_level) ShowLog(log_level,"%s tid=%lu,TCB:%d,pool[%d].%d,USEC=%llu",__FUNCTION__,
			tid,TCBno,n,i,rs->timestamp);
	rs->cli.Errno=0;
	*rs->cli.ErrMsg=0;
	return &rs->Conn;
}
//归还连接
void release_SC_connect(T_Connect **Connect,int TCBno,int n)
{
int i;
pthread_t tid=pthread_self();
pool *pl;
resource *rs;
T_CLI_Var *clip;
//ShowLog(5,"%s:tid=%lu,TCB:%d,poolno=%d",__FUNCTION__,pthread_self(),TCBno,n);
	if(!Connect || !scpool || n<0 || n>=SCPOOLNUM) {
		ShowLog(1,"%s:TCB:%d,poolno=%d,错误的参数",__FUNCTION__,TCBno,n);
		return;
	}
	if(!*Connect) {
		ShowLog(1,"%s:TCB:%d,Conn is Empty!",__FUNCTION__,TCBno);
		return;
	}
	(*Connect)->CryptFlg &= ~UNDO_ZIP;
	clip=(T_CLI_Var *)((*Connect)->Var);
	pl=&scpool[n];
	i=lnk_no(pl,*Connect);
	if(i>=0) {
		rs=&pl->lnk[i];
if(clip != &rs->cli) {
	ShowLog(1,"%s:TCB:%d,clip not equal!",__FUNCTION__,TCBno);
}
		if(clip->Errno==-1) {  //连接失效
			ShowLog(1,"%s:scpool[%d].%d to fail!",__FUNCTION__,n,i);
			disconnect(&rs->Conn);
		}
		rs->TCBno=-1;
		clip->Errno=0;
		*clip->ErrMsg=0;

		pthread_mutex_lock(&pl->mut);
		add_lnk(pl,i);
		pthread_mutex_unlock(&pl->mut);
		pthread_cond_signal(&pl->cond); //如果有等待连接的线程就唤醒它
  		rs->timestamp=now_usec();
		*Connect=NULL;
		if(log_level) ShowLog(log_level,"%s tid=%lu,TCB:%d,pool[%d].%d,USEC=%llu",
					__FUNCTION__,tid,TCBno,n,i,rs->timestamp);
		return;
	}
	ShowLog(1,"%s:在pool[%d]中未发现该TCBno:%d",__FUNCTION__,n,TCBno);
	clip->Errno=0;
	*clip->ErrMsg=0;
	*Connect=NULL;
}
//连接池监控
void scpool_check()
{
int n,i,num;
pool *pl;
resource *rs;
INT64 now;
char buf[40];
T_Connect *conn=NULL;

        if(!scpool) return;
        now=now_usec();
        pl=scpool;

        for(n=0;n<SCPOOLNUM;n++,pl++) {
                if(!pl->lnk) continue;
                rs=pl->lnk;
                num=pl->resource_num;
//              if(log_level) ShowLog(log_level,"%s:scpool[%d],num=%d",__FUNCTION__,n,num);
                pthread_mutex_lock(&pl->mut);
                for(i=0;i<num;i++,rs++) if(rs->TCBno<0) {
                        if(rs->Conn.Socket>-1 && (now-rs->timestamp)>299000000) {
//空闲时间太长了
//                              if(0!=pthread_mutex_lock(&pl->mut)) continue;
ShowLog(log_level,"%s:scpool[%d].%d,Socket=%d,to free",__FUNCTION__,n,i,rs->Conn.Socket);
//                              rs->Conn.Var=NULL; //引起内存泄漏？
                                disconnect(&rs->Conn);
                                rs->cli.Errno=-1;
                                if(log_level)
                                        ShowLog(log_level,"%s:Close SCpool[%d].%d,since %s",__FUNCTION__,
                                        n,i,rusecstrfmt(buf,rs->timestamp,YEAR_TO_USEC));
                        }
                } else {
                        if(rs->Conn.Socket>-1 && (now-rs->timestamp)>299000000) {
//占用时间太长了
                              if(-1==get_TCB_status(rs->TCBno)) {
                                //TCB已经结束，释放之
                                        unbind_sc(rs->TCBno);
                                        ShowLog(1,"%s:scpool[%d].%d TCB:%d to release",
                                                __FUNCTION__,n,i,rs->TCBno);
                                        rs->cli.Errno=-1;
                                        conn=&rs->Conn;
                			pthread_mutex_unlock(&pl->mut);
                                        release_SC_connect(&conn,rs->TCBno,n);
                			pthread_mutex_lock(&pl->mut);
                                } else {
                                    if(log_level) ShowLog(log_level,"%s:scpool[%d].lnk[%d] used by TCB:%d,since %s",
                                        __FUNCTION__,n,i,rs->TCBno,
                                        rusecstrfmt(buf,rs->timestamp,YEAR_TO_USEC));
                                }
                        }
                }
                pthread_mutex_unlock(&pl->mut);
        }
}

/**
 * 根据d_node取连接池号
 * 失败返回-1
 */
int get_scpool_no(int d_node)
{
int n;
	if(!scpool) return -1;
	for(n=0;n<SCPOOLNUM;n++) {
		if(scpool[n].d_node==d_node) return n;
	}
	return -1;
}

int get_scpoolnum()
{
	return SCPOOLNUM;
}

char *get_SC_DBLABEL(int poolno)
{
	if(!scpool) return NULL;
	if(poolno<0 || poolno>=SCPOOLNUM) return NULL;
	return scpool[poolno].DBLABEL;
}

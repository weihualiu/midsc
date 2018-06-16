static int log_ret(T_Connect *conn,T_NetHead *head)
{
int ret,event;
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
resource *rs;
path_lnk *pl;
T_Connect *Conn;
T_NetHead Head;
sdbcfunc f;
log_stu logs;

	event=get_event_status(srvp->TCB_no);
	clr_event(srvp->TCB_no);
//ShowLog(5,"%s:tid=%lX,poolno=%d,entry TCB:%d",__FUNCTION__,pthread_self(),srvp->poolno,srvp->TCB_no);
	if(srvp->poolno < 0) {
		ShowLog(1,"%s:tid=%lX,poolno=%d,entry TCB:%d",__FUNCTION__,
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
	if(pl->svc_tbl.srvn == 0) {
	    pl->svc_tbl.srvn=-1; //乐观锁
	    ret=init_svc_no(&rs->Conn);
	    if(ret) { //取服务名失败
		ShowLog(1,"%s:HOST=%s,init_svc_no error ERRNO1=%d,ERRNO2=%d,%s",__FUNCTION__,
			Conn->Host,Head.ERRNO1,Head.ERRNO2,Head.data);
		goto errret;
	    }
	} else {
		pl->svc_tbl.usage++;
                Conn->freevar=(void (*)(void *)) free_srv_list;
        }

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
		ShowLog(1,"%s:tid=%lX,poolno=%d,entry TCB:%d",__FUNCTION__,
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

	p=getenv("TCPTIMEOUT");
	if(p && isdigit(*p)) {
		Head.ERRNO2=60*atoi(p);
	} else Head.ERRNO2=0;

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


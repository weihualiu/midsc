#include <unistd.h>
#include <sys/epoll.h>
#include "midsc.h"
#include <dw.h>
#include <scry.h>
#include <enigma.h>
#include <crc.h>
#include <crc32.h>
#include <arpa/inet.h>
#include "scpool.h"

extern srvfunc Function[];

static int login_finish(T_Connect *conn,T_NetHead *NetHead);

int login(T_Connect *conn,T_NetHead *NetHead)
{
int ret,crc;
char tmp[200];
char *cp,*key;
char tmp1[1024],cliaddr[20];
DWS dw;
login_stu logrec;
ENIGMA2 egm;
FILE *fd;
T_SRV_Var *up;
GDA *gp;

	up=(T_SRV_Var *)conn->Var;
	gp=(GDA *)up->var;
	StrAddr(NetHead->O_NODE,cliaddr);
//ShowLog(5,"%s:TCB:%d Client IP Addr=%s,Net_login %s",__FUNCTION__,up->TCB_no,cliaddr,NetHead->data);
	net_dispack(&logrec,NetHead->data,login_tpl);
	net_pack(NetHead->data,&logrec,login_tpl);//恢复data
	strcpy(gp->devid,logrec.DEVID);
	strcpy(gp->operid,logrec.UID);
	sprintf(gp->ShowID,"%s:%s:%d",logrec.DEVID,cliaddr,up->TCB_no);
	mthr_showid_add(up->tid,gp->ShowID);

	if(NetHead->ERRNO1>0) conn->MTU=NetHead->ERRNO1;
	if(NetHead->ERRNO2>0) conn->timeout=NetHead->ERRNO2;

	cp=getenv("KEYFILE");
	if(!cp||!*cp) {
		strcpy(tmp1,"缺少环境变量 KEYFILE");
		NetHead->ERRNO1=-1;
errret:
		ShowLog(1,"%s:Error %s",__FUNCTION__,tmp1);
		NetHead->ERRNO2=-1;
		NetHead->PKG_REC_NUM=0;
		NetHead->O_NODE=LocalAddr(conn->Socket,NULL);
		NetHead->data=tmp1;
		NetHead->PKG_LEN=strlen(NetHead->data);
    		SendPack(conn,NetHead);
		return -1; // fail
	}
/* read key */
	crc=0;
reopen:
	ret=initdw(cp,&dw);
	if(ret) {
		if((errno==24)&& (++crc<5)) {
			sleep(15);
			goto reopen;
		}
		sprintf(tmp1,"Init dw error %d",ret);
		NetHead->ERRNO1=-1;
		goto errret;
	}
	crc=ssh_crc32((unsigned char *)logrec.DEVID,(u_int)strlen(logrec.DEVID));
	key=getdw(crc,&dw);
	if(!key) {
		freedw(&dw);
                sprintf(tmp1,"无效的 DEVID");
		NetHead->ERRNO1=-1;
                goto errret;
        }

//ShowLog(5,"getdw key=%s",key);
	enigma2_init(&egm,key,0);
//decode DBLABEL
	ret=a64_byte(gp->DBLABEL,logrec.LABEL);
	enigma2_decrypt(&egm,gp->DBLABEL,ret);
	gp->DBLABEL[ret]=0;
/* check CA */
	memset(gp->operid,0,sizeof(gp->operid));
	cp=getenv("CADIR");
	if(!cp||!*cp) cp=(char *)".";
    if(strcmp(gp->devid,"REGISTER")) {
	strncpy(gp->operid,logrec.UID,sizeof(gp->operid)-1);
	sprintf(tmp,"%s/%s.CA",cp,logrec.DEVID);
//ShowLog(5,"CAfile=%s,key=%s",tmp,key);
	fd=fopen(tmp,"r");
	if(!fd) {
		if(errno==2) {
		    crc=strlen(logrec.CA);
		    enigma2_encrypt(&egm,logrec.CA,crc);
		    byte_a64(tmp1,logrec.CA,crc);
//ShowLog(5,"CA=%s",tmp1);
		    fd=fopen(tmp,"w");
		    if(!fd) {
			sprintf(tmp1,"write %s err=%d",tmp,errno);
			NetHead->ERRNO1=-1;
err1:
			freedw(&dw);
			goto errret;
		    }
		    fprintf(fd,"%s\n",tmp1);
		    fclose(fd);
		} else {
			sprintf(tmp1,"open CAfile %s err=%d",tmp,errno);
			NetHead->ERRNO1=-1;
			goto err1;
		}
	} else {
		fgets(tmp1,sizeof(logrec.CA),fd);
		fclose(fd);
		TRIM(tmp1);
		ret=a64_byte(tmp,tmp1);
		enigma2_decrypt(&egm,tmp,ret);
		tmp[ret]=0;
		if(strcmp(tmp,logrec.CA)) {
			sprintf(tmp1,"CA 错误");
ShowLog(1,"%s:%s CA=%s log=%s len=%d",__FUNCTION__,tmp1,tmp,logrec.CA,ret);
			NetHead->ERRNO1=-1;
			goto err1;
		}
	}
    } else {   //未注册客户端注册
	char *p;
	char *keyD;
/* REGISTER label|CA|devfile|CHK_Code| */

ShowLog(2,"REGISTER %s",logrec.UID);
	if(!*logrec.UID) {
		sprintf(tmp1,"REGSTER is empty!");
		NetHead->ERRNO1=-1;
		goto err1;
	}
//uid=devfile
	crc=0xFFFF&gencrc((unsigned char *)logrec.UID,strlen(logrec.UID));
//pwd=CHK_Code
	sscanf(logrec.PWD,"%04X",&ret);
	ret &= 0xFFFF;
	if(ret != crc) {
		sprintf(tmp1,"REGISTER:devfile CHK Code error! ");//, crc,ret);
		NetHead->ERRNO1=-1;
		goto err1;
	}
	p=stptok(logrec.UID,logrec.DEVID,sizeof(logrec.DEVID),".");//logrec.DEVID=准备注册的DEVID
	crc=ssh_crc32((unsigned char *)logrec.DEVID,strlen(logrec.DEVID));
	keyD=getdw(crc,&dw);
	if(!keyD) {
		sprintf(tmp1,"注册失败,%s:没有这个设备！",
				logrec.DEVID);
		NetHead->ERRNO1=-1;
		goto err1;
	}
	enigma2_init(&egm,keyD,0);
	sprintf(tmp,"%s/%s.CA",cp,logrec.DEVID);
ShowLog(5,"REGISTER:%s",tmp);
	if(0!=(fd=fopen(tmp,"r"))) {
		fgets(tmp1,81,fd);
		fclose(fd);
		TRIM(tmp1);
		ret=a64_byte(tmp,tmp1);
		enigma2_decrypt(&egm,tmp,ret);
		tmp[ret]=0;
		if(strcmp(tmp,logrec.CA)) {
			sprintf(tmp1,"注册失败,%s 已被注册,使用中。",
					logrec.DEVID);
			NetHead->ERRNO1=-1;
			goto err1;
		}
	} else if(errno != 2) {
		sprintf(tmp1,"CA 错误");
		NetHead->ERRNO1=-1;
		goto err1;
	}
/*把设备特征码写入文件*/
	fd=fopen(tmp,"w");
	if(fd) {
	int len=strlen(logrec.CA);
		enigma2_encrypt(&egm,logrec.CA,len);
		byte_a64(tmp1,logrec.CA,len);
		fprintf(fd,"%s\n",tmp1);
		fclose(fd);
	}
	else ShowLog(1,"net_login:REGISTER open %s for write,err=%d,%s",
		tmp,errno,strerror(errno));

	freedw(&dw);
	sprintf(tmp,"%s/%s",cp,logrec.UID);
	fd=fopen(tmp,"r");
	if(!fd) {
		sprintf(tmp1,"REGISTER 打不开文件 %s err=%d,%s",
					logrec.CA,errno,strerror(errno));
		goto errret;
	}
	fgets(logrec.UID,sizeof(logrec.UID),fd);
	TRIM(logrec.UID);
	ShowLog(2,"REGISTER open %s",tmp);
	fclose(fd);
	cp=tmp1;
	cp+=sprintf(cp,"%s|%s|", logrec.DEVID,logrec.UID);
	cp+=sprintf(cp,"%s|",rsecstrfmt(tmp,now_sec(),YEAR_TO_SEC));
	NetHead->data=tmp1;
	NetHead->PKG_LEN=strlen(NetHead->data);
	NetHead->ERRNO1=0;
	NetHead->ERRNO2=0;
	NetHead->O_NODE=LocalAddr(conn->Socket,NULL);
	NetHead->PKG_REC_NUM=0;
    	SendPack(conn,NetHead);
	return -1;
    } //未注册客户端注册完成

	freedw(&dw);
	if(NetHead->D_NODE==0) { //要求本地服务
		log_stu logret;
		conn->only_do=NULL;
		conn->CryptFlg &= ~UNDO_ZIP;
//需要进一步的ctx认证？
		strcpy(logret.DEVID,gp->devid);
		strcpy(logret.UID,gp->operid);
		*logret.DBUSER=0;
		*logret.DBOWN=0;
		*logret.DBLABEL=0;
		rsecstrfmt(logret.Logtime,now_sec(),YEAR_TO_SEC);
		net_pack(tmp1,&logret,log_tpl);

		NetHead->ERRNO1=NetHead->ERRNO2=0;
		NetHead->data=tmp1;
		NetHead->PKG_LEN=strlen(NetHead->data);
    		SendPack(conn,NetHead);
		ShowLog(2,"%s:DEVID=%s login local succeed!",__FUNCTION__,logret.DEVID);
		return 1;
	}
//配置目标路由
	up->poolno=get_scpool_no(NetHead->D_NODE);
	if(up->poolno<0) {
		sprintf(tmp1,"非法的D_NODE %d",NetHead->D_NODE);
		NetHead->ERRNO1=-1;
		goto errret;
	}
	cp=get_SC_DBLABEL(up->poolno);
	if(cp && *cp && strcmp(cp,gp->DBLABEL)) {
		sprintf(tmp1,"DBLABEL %s != %s",gp->DBLABEL,cp);
		NetHead->ERRNO1=-1;
		goto errret;
	}
	ret=scpool_MGR(up->TCB_no,up->poolno,&gp->server,login_finish);
	if(ret==0) return login_finish(conn,NetHead);
	else if(ret==1) return THREAD_ESCAPE;
	sprintf(tmp1,"连接服务器失败!");
	NetHead->ERRNO1=PACK_NOANSER;
	goto errret;
}

extern void set_showid(void *ctx); //midsc.c
static int login_end(T_Connect *conn,T_NetHead *NetHead)
{
int ret,uz=0;
T_SRV_Var *up=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)up->var;
char tmp1[256];
T_CLI_Var *clip;

	ret=get_event_status(up->TCB_no);
        if(-2==clr_event(up->TCB_no)) {
                 ShowLog(1,"%s:TCB:%d err 清除事件错!",__FUNCTION__,up->TCB_no);
        }
        if(ret!=EPOLLIN) { 
                int efd=get_event_fd(up->TCB_no);
                sprintf(tmp1,"呼叫服务器%s/%s返回超时,event=0X%08X,fd=%d",
                        gp->server->Host,gp->server->Service,ret,efd);
                goto err2;
        }
	uz = (conn->CryptFlg & gp->server->CryptFlg) & DO_ZIP;

	ret=RecvPack(gp->server,NetHead);
	if(ret) {
		sprintf(tmp1,"server fault err=%d",errno);
err2:
		clip=(T_CLI_Var *)gp->server->Var;
		clip->Errno=-1;//close the socket
                ret=-1;
		NetHead->data=tmp1;
                NetHead->PKG_LEN=strlen(NetHead->data);
                NetHead->ERRNO1=-1;
		NetHead->ERRNO2=PACK_NOANSER;
	} else {
		ret=(NetHead->ERRNO1==0)?1:0;
	}

	release_SC_connect(&gp->server,up->poolno);

    	NetHead->ERRNO1=SendPack(conn,NetHead);
	if(uz) conn->CryptFlg |= UNDO_ZIP;
	stptok(NetHead->data,gp->ShowID,sizeof(gp->ShowID),"|");
	sprintf(gp->ShowID+strlen(gp->ShowID),":%d ",up->TCB_no);
	set_showid(gp);
	ShowLog(2,"%s:%s,ERRNO2=%d,tid=%lX",__FUNCTION__,
		NetHead->data,NetHead->ERRNO2,pthread_self());

	return ret;
}

static int login_finish(T_Connect *conn,T_NetHead *NetHead)
{
T_SRV_Var *up=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)up->var;
T_CLI_Var *clip;
char tmp[30],tmp1[256];
int uz,ret;
log_stu logret;

	unset_callback(up->TCB_no);
	if(!gp->server) {
		sprintf(tmp1,"%s:connect to server fault,TCB:%d",
			__FUNCTION__,up->TCB_no);
		ShowLog(1,"%s:Error:%s",__FUNCTION__,tmp1);
                NetHead->ERRNO2=-1;
		NetHead->ERRNO1=PACK_NOANSER;
		NetHead->O_NODE=LocalAddr(conn->Socket,NULL);
                NetHead->PKG_REC_NUM=0;
                NetHead->data=tmp1;
                NetHead->PKG_LEN=strlen(NetHead->data);
                SendPack(conn,NetHead);
                return -1; // fail
	}
	*tmp=0;
	uz = (conn->CryptFlg & gp->server->CryptFlg) & DO_ZIP;
	clip=(T_CLI_Var *)gp->server->Var;
//进行应用级认证	
	NetHead->PROTO_NUM=get_srv_no(clip,"ctx_login_svc");
	if(NetHead->PROTO_NUM > 1 && NetHead->PKG_REC_NUM!=-1) {
//异步
	  	int i=set_event(up->TCB_no,gp->server->Socket,login_end,60);
        	if(i) {
           		sprintf(tmp1,"TCB:%d set_event error %d",
               	        	up->TCB_no,i);
	        	release_SC_connect(&gp->server,up->poolno);
			NetHead->ERRNO1=-1;
			NetHead->ERRNO2=i;
			NetHead->data=tmp1;
			NetHead->PKG_LEN=strlen(NetHead->data);
			ShowLog(1,"%s:%s fault",__FUNCTION__,tmp1);
    			SendPack(conn,NetHead);
			return -1;
        	}
		ret=SendPack(gp->server,NetHead);
		if(ret==-1) {
			ShowLog(1,"%s:Send to server error",__FUNCTION__);
		}
        	return THREAD_ESCAPE;//释放本线程
	}
//不谋求ctx_id
	release_SC_connect(&gp->server,up->poolno);
	strcpy(logret.DEVID,gp->devid);
	strcpy(logret.UID,gp->operid);
	strcpy(logret.DBUSER,clip->UID);
	strcpy(logret.DBOWN,clip->DBOWN);
	rsecstrfmt(logret.Logtime,now_sec(),YEAR_TO_SEC);
	strcpy(logret.DBLABEL,get_SC_DBLABEL(up->poolno));
	net_pack(tmp1,&logret,log_tpl);

	NetHead->ERRNO1=NetHead->ERRNO2=0;
	NetHead->data=tmp1;
	NetHead->PKG_LEN=strlen(NetHead->data);
    	SendPack(conn,NetHead);
	if(uz) conn->CryptFlg |= UNDO_ZIP;
        ShowLog(2,"%s:%s Login success",__FUNCTION__,tmp1);
	return 1;
}


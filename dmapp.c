#include <dlfcn.h>
#include <arpa/inet.h>
#include <BB_tree.h>
#include "midsc.h"

extern int substitute_env(char *line);

static pthread_rwlock_t dmlock = PTHREAD_RWLOCK_INITIALIZER;

typedef struct {
	char cmd_name[128];
	void *handle;
	JSON_OBJECT (*cmd)(GDA *gp,JSON_OBJECT param,JSON_OBJECT jerr);
	void (*destruct)(GDA *gp);
	int lock;
	pthread_mutex_t mut;
	pthread_cond_t cond;
} cmd_node;

static int cmd_cmp(void *s,void *d,int len)
{
cmd_node *sp,*dp;
	sp=(cmd_node *)s;
	dp=(cmd_node *)d;
	return strcmp(sp->cmd_name,dp->cmd_name);
}

static T_Tree *cmd_tree=NULL;

int dmapp(T_Connect *conn,T_NetHead *head)
{
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)srvp->var;
JSON_OBJECT cmd_json,json,err_json,result=NULL;
char msg[2048];
int ret,event=head->PROTO_NUM;
cmd_node *nodep,node={{0},NULL,NULL,NULL,1,PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER};
T_Tree *trp;

	conn->status=(head->ERRNO2==PACK_STATUS)?1:0;
	err_json=json_object_new_array();
	json=json_tokener_parse(head->data);
	if(!json) {
err:
		sprintf(msg,"请求的JSON 格式错");
                json_object_array_add(err_json,jerr(100,msg));
		head->ERRNO1=FORMATERR;
		head->ERRNO2=conn->status>0?PACK_NOANSER:-1;
		return_error(conn,head,json_object_to_json_string(err_json));
		conn->status=0;
		ShowLog(1,"%s:%s",__FUNCTION__,json_object_to_json_string(err_json));
		json_object_put(err_json);
		return 0;
	}
	cmd_json=json_object_object_get(json,"model");
	if(!cmd_json) {
		json_object_put(json);
		goto err;
	}
	stptok(json_object_get_string(cmd_json),node.cmd_name,sizeof(node.cmd_name),NULL);

	nodep=NULL;
	pthread_rwlock_rdlock(&dmlock);
	trp=BB_Tree_Find(cmd_tree,&node,sizeof(node),cmd_cmp);
	if(!trp) {
		char *envp,so_name[200];

		envp=getenv("SO_USE");
		if(envp) sprintf(so_name,"%s/lib%s.so",envp,node.cmd_name);
		else sprintf(so_name,"lib%s.so",node.cmd_name);

		node.lock=1;
		node.handle = dlopen(so_name, RTLD_NOW);
		if(node.handle) {
			dlerror();
			*(void **) (&node.cmd) = dlsym(node.handle, node.cmd_name);
			if(!node.cmd) {
				sprintf(msg,"cmd %s err %s",node.cmd_name,dlerror());
				ShowLog(1,"%s:%s",__FUNCTION__,msg);
                		json_object_array_add(err_json,jerr(102,msg));
			}
			sprintf(so_name,"_%s",node.cmd_name);
			*(void **)&node.destruct = dlsym(node.handle, so_name);
			cmd_tree=BB_Tree_Add(cmd_tree,&node,sizeof(node),cmd_cmp,NULL);
			trp=BB_Tree_Find(cmd_tree,&node,sizeof(node),cmd_cmp);
			nodep=(cmd_node *)trp->Content;
		} else {
			sprintf(msg,"%s:load %s err %s",__FUNCTION__,so_name,dlerror());
                	json_object_array_add(err_json,jerr(101,msg));
			ShowLog(1,"%s:%s",__FUNCTION__,msg);
			node.cmd=NULL;
			nodep=&node;
		}

	} else {
		nodep=(cmd_node *)trp->Content;
		pthread_mutex_lock(&nodep->mut);
		nodep->lock++;
		pthread_mutex_unlock(&nodep->mut);
	}

	pthread_rwlock_unlock(&dmlock);

	ret=0;
	if(nodep && nodep->cmd) {
		gp->conn=conn;
		result=nodep->cmd(gp,json_object_object_get(json,"param"),err_json);
		gp->conn=NULL;
		pthread_mutex_lock(&nodep->mut);
                nodep->lock--;
                pthread_mutex_unlock(&nodep->mut);
		if(nodep->lock==0) pthread_cond_signal(&nodep->cond);
		if(result==(JSON_OBJECT)-1) { //THREAD_ESCAPE
			result=NULL;
			ret=THREAD_ESCAPE;
		}
	}

	if(!result) {
		head->data=(char *)json_object_to_json_string(err_json);
		head->ERRNO1=-1;
		head->ERRNO2=conn->status>0?PACK_NOANSER:-1;
	} else {
		json_object_object_add(result,"status",err_json);
		head->data=(char *)json_object_to_json_string(result);
		head->ERRNO1=0;
		head->ERRNO2=conn->status>0?PACK_STATUS:0;
	}

	if(ret==0 && head->ERRNO2!=PACK_NOANSER) {
		head->PKG_LEN=strlen(head->data);
		head->PROTO_NUM=PutEvent(conn,event);
        	head->O_NODE=ntohl(LocalAddr(conn->Socket,NULL));
        	SendPack(conn,head);
	}

        if(result) json_object_put(result);
        else json_object_put(err_json);
        json_object_put(json);

	return ret;
}

int dmmgr(T_Connect *conn,T_NetHead *head)
{
T_SRV_Var *srvp=(T_SRV_Var *)conn->Var;
GDA *gp=(GDA *)srvp->var;
JSON_OBJECT err_json;
char *p,*envp,so_use[128],so_lib[200],*model_name,msg[2048];
int flg=0,ret,event=head->PROTO_NUM;
cmd_node *nodep=NULL;
T_Tree *trp;
	err_json=json_object_new_array();
	conn->status=(head->ERRNO2==PACK_STATUS)?1:0;
	if(!head->PKG_LEN) {
		sprintf(msg,"data is empty!");
		head->ERRNO1=LENGERR;
		goto err1;
	}
	model_name=head->data;
	envp=getenv("SO_USE");
	if(!envp||!*envp) envp=".";
	sprintf(so_use,"%s/lib%s.so",envp,model_name);
	sprintf(msg,"mv -f %s %s.bak",so_use,so_use);

	pthread_rwlock_wrlock(&dmlock);
	ret=system(msg);
	if(ret >> 8) {
		strcat(msg,":移动原模块失败");
                json_object_array_add(err_json,jerr(105,msg));
		head->ERRNO1=SYSERR;
		goto err1;
	}
	p=msg;
	envp=getenv("SO_LIB");
	if(envp&&*envp) {
		sprintf(so_lib,"%s/lib%s.so",envp,model_name);
	} else {
		sprintf(so_lib,"$HOME/lib/lib%s.so",model_name);
		substitute_env(so_lib);
	}
	sprintf(msg,"cp -f %s %s",so_lib,so_use);

	ret=system(msg);
	if(ret >> 8) { //失败
		strcat(msg,":拷贝失败");
                json_object_array_add(err_json,jerr(105,msg));
		sprintf(msg,"mv -f %s.bak %s",so_use,so_use);
		system(msg);
		head->ERRNO1=SYSERR;
err1:
		pthread_rwlock_unlock(&dmlock);
		head->ERRNO2=conn->status>0?PACK_NOANSER:-1;
		return_error(conn,head,json_object_to_json_string(err_json));
		conn->status=0;
		ShowLog(1,"%s:%s",__FUNCTION__,json_object_to_json_string(err_json));
		json_object_put(err_json);
		return 0;
	}
	cmd_node node;
	strcpy(node.cmd_name,model_name);

	trp=BB_Tree_Find(cmd_tree,&node,sizeof(node),cmd_cmp);
	if(!trp) {
		sprintf(msg,"no such model[%s]",model_name);
                json_object_array_add(err_json,jerr(106,msg));
		head->ERRNO1=-106;
		goto err1;
	}
	nodep=(cmd_node *)trp->Content;
	pthread_mutex_lock(&nodep->mut);
	while(nodep->lock>0) {
		pthread_cond_wait(&nodep->cond,&nodep->mut);
	}
	pthread_mutex_unlock(&nodep->mut);
	if(nodep->destruct) nodep->destruct(gp);
	do {
		ret=dlclose(nodep->handle);
	} while(ret>0);
	cmd_tree=BB_Tree_Del(cmd_tree,nodep,sizeof(node),cmd_cmp,NULL,&flg);
	pthread_rwlock_unlock(&dmlock);

	if(head->ERRNO2 != PACK_NOANSER) {
	    if(ret)
		sprintf(msg,"model %s unloaded errno=%d,%s",model_name,errno,strerror(errno));
	    else
		sprintf(msg,"model %s unloaded flg=%d",model_name,flg);
	    ShowLog(2,"%s:%s",__FUNCTION__,msg);
            json_object_array_add(err_json,jerr(ret,msg));

	    head->data=(char *)json_object_to_json_string(err_json);
	    head->ERRNO1=0;
	    head->ERRNO2=0;
	    head->PKG_LEN=strlen(head->data);
	    head->PROTO_NUM=PutEvent(conn,event);
            head->O_NODE=ntohl(LocalAddr(conn->Socket,NULL));
            ret=SendPack(conn,head);
	}
        json_object_put(err_json);

	return 0;
}

//负载均衡，找一个负载最轻的池
resource * get_SC_weight()
{
int i,max_weight,w,n,m;
pool *pl;
struct timespec tims;
resource *rs;

	pthread_mutex_lock(&weightLock);
   do {
	max_weight=-1,n=-1,m=-1;
//	badtime=now_usec();
	pl=scpool;
	for(i=0;i<SCPOOLNUM;i++,pl++) {
//找权重最重的那个池
ShowLog(5,"%s:weight[%d]=%d",__FUNCTION__,i,pl->weight);
		pthread_mutex_lock(&pl->mut);
		if(pl->weight>0) {
			w=pl->weight<<9;
			w/=pl->resource_num;
			if(w>max_weight) {
				max_weight=w;
				n=i;
			}
		}
		pthread_mutex_unlock(&pl->mut);
	}
	if(n>=0) {
		rs=get_SC_resource(n,0);
		if(rs && rs != (resource *)-1) {
			pthread_mutex_unlock(&weightLock);
			return rs;
		}
		else continue;
	}
	ShowLog(4,"%s:get_SC_weight:n=%d,weight=%d",__FUNCTION__,
				n,max_weight);
	for(m=0;m<SCPOOLNUM;m++) { //测一遍故障池,看看能否恢复
		if(scpool[m].weight>=0) continue;
		rs=get_SC_resource(m,1);
		if(rs && rs != (resource *)-1) {
			pthread_mutex_unlock(&weightLock);
			return rs;
		}
	}
	clock_gettime(CLOCK_REALTIME, &tims);
	tims.tv_sec+=6;//因为归还连接并不锁weightLock，可能丢失事件，等6秒
	pthread_cond_timedwait(&weightCond,&weightLock,&tims); //实在没有了，等
	scpool_check();
    } while(1);
}

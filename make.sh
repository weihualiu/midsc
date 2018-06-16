##############################
# File Name: make.sh
# Author: liu.weihua
# email: 394806487@qq.com
# Created Time: 06/16/2018
##############################
#!/bin/sh

usage(){
	echo "Usage: $0 -sdbcdir"	
}

compile(){
	echo $1
	rm -rf build;
	mkdir build;
	cd build;
	cmake -DSDBCDIR=$1 ../;
	make;
}

case $1 in
	-sdbcdir)
		compile $2;
		;;
	*)
		usage;
	esac

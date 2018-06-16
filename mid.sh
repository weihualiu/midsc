#!/bin/sh
# SDBC 交易管理器 配置
# uasge: nohup mid.sh &
# 检查 $HOME/etc/mid.cfg $HOME/etc/route.txt 路由表配置是否正确
# 自动重启的不死鸟
while true
do
	midsc $HOME/etc/mid.cfg
	sleep 10
done

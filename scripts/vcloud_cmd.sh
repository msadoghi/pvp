#!/bin/bash
USERNAME=ubuntu
HOSTS="$1"
SCRIPT="$2"
count=0
# IDENTITY="~/Desktop/pvp/kdk.pem"
#IDENTITY="~/kdkCA.pem"
# IDENTITY="~/oracle2.key"
IDENTITY="~/kdk_oracle.key"

for HOSTNAME in ${HOSTS}; do
	ssh -i ${IDENTITY} -n -o BatchMode=yes -o StrictHostKeyChecking=no -l ${USERNAME} ${HOSTNAME} "${SCRIPT}" &
	count=`expr $count + 1`
done

while [ $count -gt 0 ]; do
	wait $pids
	count=`expr $count - 1`
done


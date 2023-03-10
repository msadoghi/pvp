#!/bin/bash
# RES_FILE --> Name of the result file.
#
USERNAME=ubuntu
# IDENTITY="~/Desktop/pvp/kdkCA.pem"
IDENTITY="~/Desktop/pvp/kdk_oracle.key"
# IDENTITY="~/kdkCA.pem"
# HOSTNAME='155.248.208.168'	# sanjose
HOSTNAME='150.136.223.66'	# ashburn
count=0
while(( $count<5 ))
do
	scp -i ${IDENTITY} -o StrictHostKeyChecking=no ${USERNAME}@$HOSTNAME:pvp/results/${count}.out ./results/
	count=`expr $count + 1`
done

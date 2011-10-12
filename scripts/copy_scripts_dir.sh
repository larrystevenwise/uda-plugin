#!/bin/bash
# written by: Idan Weinstein 6/jul/2011
# copy the executed script's dir to local tmp directory for master and slaves nodes 

SCRIPTS_LOCAL_TMP_DIR=/tmp/hadoop/scripts


echo "$0: sudo mkdir -p $SCRIPTS_LOCAL_TMP_DIR"
mkdir -p $SCRIPTS_LOCAL_TMP_DIR
echo "$0: sudo in/slaves.sh sudo mkdir -p $SCRIPTS_LOCAL_TMP_DIR"
sudo bin/slaves.sh  mkdir -p $SCRIPTS_LOCAL_TMP_DIR
echo "$0: sudo cp -rf $(dirname $0)/* $SCRIPTS_LOCAL_TMP_DIR/"
sudo cp -rf $(dirname $0)/* $SCRIPTS_LOCAL_TMP_DIR/
echo "$0: sudo bin/slaves.sh cp -rf $(dirname $0)/* $SCRIPTS_LOCAL_TMP_DIR/"
sudo bin/slaves.sh cp -rf $(dirname $0)/* $SCRIPTS_LOCAL_TMP_DIR/


# check for success
master_fails=`if  diff -q  --exclude=".*"  $(dirname $0)/ $SCRIPTS_LOCAL_TMP_DIR/ > /dev/null ; then echo 0 ; else echo 1 ; fi ;`
slaves_fails=`bin/slaves.sh if  diff -q  --exclude=".*"  $(dirname $0)/ $SCRIPTS_LOCAL_TMP_DIR/ \> /dev/null \; then echo OK \; else echo FAIL \; fi | grep -c FAIL`

if [ $((master_fails + slaves_fails))  == 0 ] ; 
then
	echo $0: scripts dir copy success
	exit 0;
else
	echo $0: scripts dir copy failure
	exit 1;
fi

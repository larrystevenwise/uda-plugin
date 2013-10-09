#!/bin/bash

checkFallback()
{
	fallbackDesc="$1"
	if [ `$sshPrefix cat $testDir/slave*/$REDUCER_LOG_PATH | grep fallback | grep -c "$fallbackDesc"` -ne 0 ];then
		retVal=0
	else
		retVal=1
	fi
}

memoryAllocationAnalyzer()
{
	rdmaSizeDest=$1
	retVal=0
	if (($SHUFFLE_CONSUMER == 0));then
		echo "export TEST_ERROR='UDA DID NOT RUN'" >> $testExports
		return 0
	fi
	local providerVersions=`$sshPrefix grep -h \"The version is\" $testDir/slave*/$PROVIDER_LOG_PATH | awk '{ split($0,a,"The version is"); split(a[2],a," "); print a[1] }' | sort -u`
	local bufferSizes=`$sshPrefix grep -h \"compression not configured: allocating\" $testDir/slave*/$REDUCER_LOG_PATH | awk '{ split($0,a," = "); split(a[2],a," "); print a[1]}' | sort -u`
	
		
	if [[ -z $bufferSizes ]];then
		return 0
	fi
	for rdmaInBytes in $bufferSizes
	do
		#rdmaInBytes=`echo \"${record}\" | awk 'BEGIN{} {print $12}'`
		echo "$echoPrefix: The allocated RDMA buffers size is ${rdmaInBytes} bytes"
		rdmaInKb=`echo "$rdmaInBytes/1024" | bc`
		echo "$echoPrefix: The allocated RDMA buffers size is ${rdmaInKb}Kb"
		
		if [[ $rdmaSizeDest == "max" ]];then
			if (($rdmaInKb != $RDMA_BUF_SIZE));then
				return 0
			fi
		elif [[ $rdmaSizeDest == "mid" ]];then
			if (($RDMA_BUF_SIZE_MIN > $rdmaInKb)) || (($rdmaInKb >= $RDMA_BUF_SIZE));then
				return 0
			fi
		else
			return 0
		fi
	done

	retVal=1		
}

#memoryAllocationForCompressionAnalyzer() 
#{
#
#"init compression configured. allocating rdmaBufferUsed=%d, uncompBufferUsed=%d totalBufferPerMof=%ld, splitPercentRdmaComp=%f", uncompBufferHardMin, totalBufferPerMof, splitPercentRdmaComp);
#
#	retVal=0
#	buffersSizesMessages="`$sshPrefix grep -rh "init compression configured. allocating" $testDir/slave*/$REDUCER_LOG_PATH`"
#	uncompBufferUsed=`echo $buffersSizesMessages | awk 'BEGIN{} {print $11}' | sort -u`
#	rdmaBufferUsed=`echo $buffersSizesMessages | awk 'BEGIN{} {print $11}' | sort -u`
	
	#echo "uncompBufferUsed values are: $uncompBufferUsed"
	#echo "rdmaBufferUsed values are: $rdmaBufferUsed"			
#	echo THE buffersSizesMessages IS: $buffersSizesMessages
#}

setupCompressionAnalyzer()
{
	compressionValid=1
	
	checkStatus "$exlTEST_STATUS"
	
	if (($retVal == 1));then
		retVal=0
		if [[ "$YARN_HADOOP_FLAG" == "1" ]]; then
			if [[ $COMPRESSION == "Snappy" ]]; then
				count=`$sshPrefix grep \"brand-new compressor\" $testDir/slave*/$REDUCER_LOG_PATH | grep -c "snappy"`
				if (($count == 0)); then
					compressionValid=0
				fi
			fi
		else 
			if [[ -z $COMPRESSION ]];then
				compressionValid=0
			elif [[ $COMPRESSION == "Snappy" ]];then
				count=`$sshPrefix  grep -c \"INFO snappy.LoadSnappy: Snappy native library loaded\" $testDir/testOutput.txt`
				echo count of snappy is: $count
				if (($count != 1));then
					compressionValid=0
				fi
			elif [[ $COMPRESSION == "Lzo" ]];then
				count=`$sshPrefix grep -c \"INFO compress.CodecPool: Got brand-new compressor\" $testDir/testOutput.txt`
				echo count of LZO is: $count
				if (($count != 1));then
					compressionValid=0
				fi
			else
				compressionValid=0
			fi
		fi
		if (($compressionValid==1));then
			echo "$echoPrefix: installation of the compression is valid"
			retVal=1
		fi
	else
		echo "$echoPrefix: the test failed - it is possible that the compression installation is corrupted"
		retVal=0
	fi
}

checkUdaIsUp()
{
	retVal=0
	local providerVersions=`$sshPrefix grep -h \"The version is\" $testDir/slave*/$PROVIDER_LOG_PATH | awk '{ split($0,a,"The version is"); split(a[2],a," "); print a[1] }' | sort -u`
	local consumerVersions=`$sshPrefix grep -h \"UDA version is\" $testDir/slave*/$REDUCER_LOG_PATH | awk '{ split($0,a,"UDA version is"); split(a[2],a," "); print a[1] }' | sort -u`
	local numOfUdaOpen=`$sshPrefix cat $testDir/slave*/$REDUCER_LOG_PATH | grep -c "init - Using UdaShuffleConsumerPlugin"`
	local numOfUdaClose=`$sshPrefix cat $testDir/slave*/$REDUCER_LOG_PATH | grep -c "====XXX Successfully closed UdaShuffleConsumerPlugin XXX===="`
		
	if [[ "$providerVersions" != "$consumerVersions" ]];then
		echo "export TEST_ERROR='UDA providers or consumers have different UDA versions'" >> $testExports
		checkUdaIsUpRetVal_rpmVersion="-1"
		return 0
	fi
	
	if [[ "$numOfUdaOpen" != "$numOfUdaClose" ]]; then
		echo "export TEST_ERROR='$numOfUdaOpen UDA sessions were initiated but only $numOfUdaClose sessions ended successfully'" >> $testExports
		return 0
	fi
	
	if (($numOfUdaOpen == 0));then
		echo "export TEST_ERROR='No UDA sessions were initiated during this test - UDA did not run!'" >> $testExports
		return 0
	fi
	
	if [[ -n $providerVersions ]] && [[ -n $consumerVersions ]] ;then
		retVal=1
	else
		echo "export TEST_ERROR='UDA providers or consumers were not loaded'" >> $testExports
		return 0
	fi
	
	checkUdaIsUpRetVal_rpmVersion=$providerVersions
}

checkConsumerIsUp()
{
	retVal=0
	
	local consumerVersions=`$sshPrefix grep -h \"UDA version is\" $testDir/slave*/$REDUCER_LOG_PATH | awk '{ split($0,a,"UDA version is"); split(a[2],a," "); print a[1] }' | sort -u`
	local consumerVersionsCount=`echo "$consumerVersions" | awk 'BEGIN{RS=FS;count=0} {if ($1 ~ /[0-9.-]+/){count++}} END{print count}'`
	
	if (($consumerVersionsCount > 1)) ;then
	echo "export TEST_ERROR='UDA consumers have more than one UDA version'" >> $testExports
		#checkUdaIsUpRetVal_rpmVersion="-1"
		return 0
	fi
	
	if [[ -n $consumerVersions ]] ;then
		retVal=1
	else
		echo "export TEST_ERROR='UDA consumers were not loaded'" >> $testExports
		return 0
	fi
	#checkUdaIsUpRetVal_rpmVersion="-1"
}

rpmInstallationAnalyzer()
{
	retVal=2
	if [[ $checkUdaIsUpRetVal_rpmVersion == $RPM_VERSION ]];then
		retVal=3
	else
		echo "export TEST_ERROR='UDA practical version is different than the configured version'" >> $testExports
	fi
}

inverseRetVal()
{
	retVal=$((1-retVal))
}

checkStatus()
{
	successCriteria=$1
	
	#eval successCriteriaValue=\$$successCriteria # getting the value of the variable with "inside" successCriteria
	#if (($successCriteriaValue == 1));then
	if (($successCriteria == 1));then
		retVal=1
	else
		retVal=0
	fi
}

managePerformanceTests()
{
	checkStatus "$exlTEST_STATUS"
	performanceTestFlag=1
}

setRetValAndExit()
{
	valToSet=$1
	if (($valToSet == 0));then
        echo "export TEST_STATUS=0" >> $testExports
	elif (($valToSet == 1));then
        echo "export TEST_STATUS=1" >> $testExports
	elif (($valToSet == 2));then
        echo "export TEST_STATUS=0" >> $testExports
        echo "export SETUP_FAILURE=1" >> $testExports
	elif (($valToSet == 3));then
        echo "export TEST_STATUS=1" >> $testExports
        echo "export SETUP_FAILURE=" >> $testExports # SETUP_FAILURE is null
	fi
	echo "export PERFORNAMCE_TEST=$performanceTestFlag" >> $testExports
	exit 0
}

checkAndReturn()
{
	if (($retVal == 0));then
		setRetValAndExit 0
	fi
}

testToAnalyzerMapper()
{
	testID=$1
	
	# VUDA-43: some temporary lines till writing general solution
	if [[ $testID == "VUDA-43" ]];then 
		checkConsumerIsUp
		checkAndReturn
		checkFallback
		inverseRetVal
		setRetValAndExit $retVal
	else
		checkStatus "$exlTEST_STATUS"
		checkAndReturn
	fi
	
	if [[ $testID == "VUDA-00" ]];then
		managePerformanceTests
		setRetValAndExit $retVal
	fi

	if [[ $testID == "VUDA-35" ]];then 
		checkUdaIsUp
		checkAndReturn
		checkFallback "illegal fetch request size of 0 or less bytes"
		inverseRetVal
		setRetValAndExit $retVal
	fi

	if [[ -n $COMPRESSION_TEST_LEVEL ]] && [[ $PROGRAM != "pi" ]] ;then	
		setupCompressionAnalyzer
		checkAndReturn
	fi
	
	case ${testID} in
		VUDA-11|VUDA-16|VUDA-34|VUDA-36|VUDA-46 )
			checkFallback
			inverseRetVal
			checkAndReturn
			checkStatus "$exlTEST_STATUS"
			setRetValAndExit $retVal
	esac
			
	checkUdaIsUp
	checkAndReturn
	checkFallback
	checkAndReturn

	case ${testID} in
		VUDA-17	) memoryAllocationAnalyzer "mid" ;;
		VUDA-19	) memoryAllocationAnalyzer "max" ;;
		VUDA-30	) setupCompressionAnalyzer;;
		VUDA-31	) rpmInstallationAnalyzer;;
		VUDA-21|VUDA-9|VUDA-12|VUDA-47	) echo "";;#checkStatus "$exlTEST_STATUS";;
		VUDA-29	) managePerformanceTests;;
		*	) echo "$testID has no Analyzer function" ;;   # Default.	
	esac
	
	setRetValAndExit $retVal
}

echoPrefix=`eval $ECHO_PATTERN`
testDir=$1
performanceTestFlag=0

# this logic found to cause problems - grep behave differantly via ssh!
#sshPrefix=""
#if [[ $RES_SERVER != `hostname` ]];then
#	sshPrefix="ssh $RES_SERVER"
#fi
sshPrefix="ssh $RES_SERVER"

testExports=$testDir/execLogExports.sh
source $testExports

testToAnalyzerMapper $TEST_IDS

echo "export PERFORNAMCE_TEST=$performanceTestFlag" >> $testExports



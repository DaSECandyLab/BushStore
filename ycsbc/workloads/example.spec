fieldcount=1
fieldlength=1024

recordcount=20000000
operationcount=16000000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true

readproportion=0.4
updateproportion=0.2
scanproportion=0.2
insertproportion=0.2

requestdistribution=zipfian
zipfianvalue=0.98
maxscanlength=100
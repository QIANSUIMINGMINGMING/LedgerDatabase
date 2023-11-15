#!/bin/bash

#============= Parameters to fill ============
nshard=1           # number of shards
nclient=10         # number of clients / machine
rtime=120          # duration to run
delay=100          # verification delay

tlen=10            # transaction length
wper=50            # writes percentage
rper=50            # reads percentage
zalpha=0           # zipf alpha

#============= Start Experiment =============
. env.sh

replicas=127.0.0.1
clients=127.0.0.1
client="verifyClient"
store="strongstore"
mode="occ"
txnrate=120

# Print out configuration being used.
echo "Configuration:"
echo "Shards: $nshard"
echo "Clients per host: $nclient"
echo "Transaction Length: $tlen"
echo "Write Percentage: $wper"
echo "Read Percentage: $rper"
echo "Get history Percentage: $((100-wper-rper))"
echo "Zipf alpha: $zalpha"

# Start all replicas and timestamp servers
echo "Starting TimeStampServer replicas.."

mkdir -p $logdir
$bindir/timeserver -c $expdir/shard.tss.config -i 0 > $logdir/tss.replica0.log 2>$logdir/tss.replica0.err &

echo "Starting shard0 replicas.."
$bindir/$store -c $expdir/shard0.config -i 0 -m $mode -e 0 -s 0 -N $nshard -n 0 -t 100 -w ycsb -k 100000 > $logdir/shard0.replica0.log 2>$logdir/shard0.replica0.err &

# Wait a bit for all replicas to start up
sleep 10

# Run the clients
echo "Running the client(s)"
count=0

for ((i=0; i<$nclient; i++))
do
    $bindir/$client -c $expdir/shard -N $nshard -d $rtime -l $tlen -w $wper -g $rper -m $mode -e 0 -s 0 -z $zalpha -t $delay -x $txnrate -i $i > $logdir/client.$i.log 2>$logdir/client.$i.err &
done

# Wait for all clients to exit
echo "Waiting for client(s) to exit"
procname=$client
check=1
while [ $check -gt 0 ]
do
    check=`pgrep -u $USER -x $procname | wc -l`
    sleep 1
done

# Kill all replicas
echo "Cleaning up"
$expdir/stop_replica.sh $expdir/shard.tss.config > /dev/null 2>&1

$expdir/stop_replica.sh $expdir/shard0.config > /dev/null 2>&1

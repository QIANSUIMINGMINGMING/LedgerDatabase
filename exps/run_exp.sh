#===== Parameters to Fill =====

# Write percentage
wperc=(50)

# Number of shards
nshards=(1)

# Number of client processes per client node
nclients=(1)

# Zipf factor
theta=(0)

# Experiment duration
rtime=60

# Verification delay
delay=100

# Transaction size
tlen=10

# ycsb or tpcc
driver=ycsb

# cpu or gpu
device=gpu

# request rate
txnrate=120
#==============================

wpers=$( IFS=$','; echo "${wperc[*]}" )
servers=$( IFS=$','; echo "${nshards[*]}" )
clients=$( IFS=$','; echo "${nclients[*]}" )
thetas=$( IFS=$','; echo "${theta[*]}" )

for i in ${wperc[@]}
do
  let rper=100-$i
  for j in ${nshards[@]}
  do
    ./load.sh $j
    for k in ${theta[@]}
    do
      for c in ${nclients[@]}
      do
        echo =============================================
        echo -e $i % writes, $j nodes, $k Zipf, $c clients
        echo =============================================
        
        sed -i -e "s/tlen=[0-9]*/tlen=${tlen}/g" run_$driver.sh
        sed -i -e "s/rtime=[0-9]*/rtime=${rtime}/g" run_$driver.sh
        sed -i -e "s/wper=[0-9]*/wper=$i/g" run_$driver.sh
        sed -i -e "s/rper=[0-9]*/rper=$rper/g" run_$driver.sh
        sed -i -e "s/nshard=[0-9]*/nshard=$j/g" run_$driver.sh
        sed -i -e "s/nclient=[0-9]*/nclient=$c/g" run_$driver.sh
        sed -i -e "s/zalpha=[0-9\.]*/zalpha=$k/g" run_$driver.sh
        sed -i -e "s/delay=[0-9\.]*/delay=${delay}/g" run_$driver.sh
        sed -i -e "s/txnrate=[0-9]*/txnrate=${txnrate}/g" run_$driver.sh
        sed -i -e "s/device=[a-z]*/device=${device}/g" run_$driver.sh

        ./clean.sh
        ./run_$driver.sh
        ./clean.sh
      done
    done
  done
done

python2 parse_$driver.py result/ $wpers $servers $clients $thetas

. env.sh
delay=0
devices=(cpu gpu)
inserts=(olc 2pi)

# rm result* -r

for n in $(seq 12); do
  for device in ${devices[@]}; do

    if [ $device == "gpu" ]; then
      for insert in ${inserts[@]}; do
        echo running delay=${delay} in $device, inserted with $insert

        sed -i -e "s/delay=[0-9][0-9]*/delay=${delay}/g" run_exp.sh
        sed -i -e "s/device=[a-z][a-z]*/device=${device}/g" run_exp.sh
        sed -i -e "s/insert=[a-z0-9][a-z]*/insert=${insert}/g" run_exp.sh

        ./run_exp.sh
        mv result result-${delay}-${device}-${insert}
        mv ${logdir}/shard0.replica0.err result-${delay}-${device}-${insert}
        ./clean.sh
      done

    else # cpu
        echo running delay=${delay} in $device

        sed -i -e "s/delay=[0-9][0-9]*/delay=${delay}/g" run_exp.sh
        sed -i -e "s/device=[a-z][a-z]*/device=${device}/g" run_exp.sh
        # sed -i -e "s/insert=[a-z][a-z]*/insert=${insert}/g" run_exp.sh

        ./run_exp.sh
        mv result result-${delay}-${device}
        mv ${logdir}/shard0.replica0.err result-${delay}-${device}
        ./clean.sh
    fi

  done
  delay=`expr $delay + 100`
done
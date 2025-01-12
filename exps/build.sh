if [ $(echo "$1" | awk '{print tolower($0)}') == 'qldb' ]
then
  qldbopt=ON
  ledgerdbopt=OFF
else
  qldbopt=OFF
  ledgerdbopt=ON
fi

. env.sh

# for host in `cat clients`
# do
#   echo ${host}
#   ssh ${host} "source ~/.profile; source ~/.bashrc; mkdir -p ${rootdir}/build; cd ${rootdir}/build; rm -rf *; cmake -DLEDGERDB=${ledgerdbopt} ..; make -j6;" &
# done

source ~/.profile; source ~/.bashrc; mkdir -p ${rootdir}/build; cd ${rootdir}/build; rm -rf *; cmake -DLEDGERDB=${ledgerdbopt} -DCMAKE_BUILD_TYPE=debug ..; make -j6
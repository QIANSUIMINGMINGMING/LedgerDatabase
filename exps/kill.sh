sudo lsof -i :44444 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :55555 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :51725 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :51729 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9


sudo lsof -i :44444 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :55555 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :51725 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
sudo lsof -i :51729 | sed -n 's/strongsto \([0-9]\+\).*/\1/p' | uniq | sudo xargs kill -9
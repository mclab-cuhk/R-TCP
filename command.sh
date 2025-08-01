sudo sysctl net.ipv4.tcp_no_metrics_save=1
sudo sysctl net.ipv4.tcp_congestion_control=cubic
sudo modprobe -r rtcp_bbr
make
sudo install rtcp_bbr.ko /lib/modules/`uname -r`
sudo depmod
sudo modprobe rtcp_bbr
sudo sysctl net.ipv4.tcp_congestion_control=rtcp_bbr
sudo dmesg -C

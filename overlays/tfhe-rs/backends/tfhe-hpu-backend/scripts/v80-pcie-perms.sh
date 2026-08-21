  #!/usr/bin/env bash
   
GROUP=${HPU_PCIE_GROUP:-hw}
if ! getent group "$GROUP" >/dev/null; then
  if [ -n "${SUDO_USER:-}" ]; then
    GROUP=$(id -gn "$SUDO_USER")
  else
    GROUP=$(id -gn)
  fi
fi
# Enable Pcie rescan
if [ -e /sys/bus/pci/rescan ]; then
  chgrp $GROUP /sys/bus/pci/rescan && chmod g+w /sys/bus/pci/rescan
fi
# Search V80 pcie boards PF0
for dev in $(lspci -nn -d 10ee:50b4 | awk '{print $1}'); do
  chgrp -R $GROUP /sys/bus/pci/devices/0000\:$dev/
  chmod -R g=u    /sys/bus/pci/devices/0000\:$dev/
done
# Search V80 pcie boards PF1
for dev in $(lspci -nn -d 10ee:50b5 | awk '{print $1}'); do
  chgrp -R $GROUP /sys/bus/pci/devices/0000\:$dev/
  chmod -R g=u    /sys/bus/pci/devices/0000\:$dev/
done

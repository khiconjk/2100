DEBUG=false

MODDIR=${0%/*}

cd "$MODDIR"

until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done

if [ -f ./sec_carrier_svc ]; then
  exec ./sec_carrier_svc
elif [ -f ./daemon ]; then
  exec ./daemon
fi

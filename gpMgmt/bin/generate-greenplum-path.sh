#!/usr/bin/env bash

cat <<"EOF"
#!/usr/bin/env bash
GPHOME="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
if [ -x $GPHOME/ext/python/bin/python ]; then
    export PYTHONHOME="$GPHOME/ext/python"
fi
PYTHONPATH="${GPHOME}/lib/python"
PATH="${GPHOME}/bin:${PYTHONHOME}/bin:${PATH}"
LD_LIBRARY_PATH="${GPHOME}/lib:${PYTHONHOME}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -e "${GPHOME}/etc/openssl.cnf" ]; then
	OPENSSL_CONF="${GPHOME}/etc/openssl.cnf"
fi

export GPHOME
export PATH
export PYTHONPATH
export LD_LIBRARY_PATH
export OPENSSL_CONF
EOF

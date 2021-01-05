#!/bin/bash -l

set -exo pipefail

CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${CWDIR}/common.bash"

function gen_env(){
	cat > /home/gpadmin/run_regression_gpcheckcloud.sh <<-EOF
	set -exo pipefail

	if [ -f /opt/gcc_env.sh ]; then
        source /opt/gcc_env.sh
    fi
	source /usr/local/greenplum-db-devel/greenplum_path.sh

	cd "\${1}/gpdb_src/gpcontrib/gpcloud/regress"
	bash gpcheckcloud_regress.sh
	EOF

	chown gpadmin:gpadmin /home/gpadmin/run_regression_gpcheckcloud.sh
	chmod a+x /home/gpadmin/run_regression_gpcheckcloud.sh
}

function set_limits() {
    case "$PLATFORM" in
		centos6)
			sed -i s/1024/unlimited/ /etc/security/limits.d/90-nproc.conf
			;;
		centos7)
			sed -i s/4096/unlimited/ /etc/security/limits.d/20-nproc.conf
			;;
		photon*)
			mkdir -p /etc/security/limits.d
			cat > /etc/security/limits.d/20-nproc.conf <<-EOF
			*          soft    nproc     unlimited
			root       soft    nproc     unlimited
			EOF
			;;
	esac
}

function run_regression_gpcheckcloud() {
	su gpadmin -c "bash /home/gpadmin/run_regression_gpcheckcloud.sh $(pwd)"
}

function setup_gpadmin_user() {
	./gpdb_src/concourse/scripts/setup_gpadmin_user.bash
}

function _main() {
	local PLATFORM=$(determine_os)
	time install_and_configure_gpdb
	time setup_gpadmin_user
	time set_limits
	time gen_env

	time run_regression_gpcheckcloud
}

_main "$@"

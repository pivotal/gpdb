#! /bin/bash
set -eo pipefail

ROOT_DIR=`pwd`

# Get ssh private key from REMOTE_KEY, which is assumed to
# be encode in base64. We can't pass the key content directly
# since newline doesn't work well for env variable.
function setup_ssh_keys() {
    # Setup ssh keys
    echo -n $REMOTE_KEY | base64 -d > ~/remote.key
    chmod 400 ~/remote.key

    eval `ssh-agent -s`
    ssh-add ~/remote.key

    # Scan for target server's public key, append port number
    ssh-keyscan -p $REMOTE_PORT $REMOTE_HOST > pubkey
    awk '{printf "[%s]:", $1 }' pubkey > tmp
    echo -n $REMOTE_PORT >> tmp
    awk '{$1 = ""; print $0; }' pubkey >> tmp

    mkdir ~/.ssh
    cat tmp >> ~/.ssh/known_hosts
}


function remote_setup() {
    # Get a session id for different commit builds.
    SESSION_ID=`ssh -T -p $REMOTE_PORT $REMOTE_USER@$REMOTE_HOST 'echo \$\$'`

    # Create working directory on remote machine.
    ssh -T -p $REMOTE_PORT $REMOTE_USER@$REMOTE_HOST > dir <<- EOF
    mkdir -p gpdb-compile/$SESSION_ID
    cd gpdb-compile/$SESSION_ID
    echo \$PWD
EOF

    GPDB_DIR=`cat dir`

    # Setup environment variables needed on remote machine.
    cat >env.sh <<- EOF
    export IVYREPO_HOST="$IVYREPO_HOST"
    export IVYREPO_REALM="$IVYREPO_REALM"
    export IVYREPO_USER="$IVYREPO_USER"
    export IVYREPO_PASSWD="$IVYREPO_PASSWD"
EOF
    scp -P $REMOTE_PORT env.sh $REMOTE_USER@$REMOTE_HOST:$GPDB_DIR/env.sh
}

function copy_to_remote() {
    scp -P $REMOTE_PORT -r gpdb_src $REMOTE_USER@$REMOTE_HOST:$GPDB_DIR/
    scp -P $REMOTE_PORT -r gpaddon_src $REMOTE_USER@$REMOTE_HOST:$GPDB_DIR/
}

function remote_compile() {
    # .profile is not automatically sourced when ssh -T to AIX
    ssh -T -p $REMOTE_PORT $REMOTE_USER@$REMOTE_HOST <<- EOF
    . .profile
    cd $GPDB_DIR
    . env.sh
    cd gpdb_src/gpAux
    make sync_tools
    make GPROOT="$GPDB_DIR" BLD_TARGETS="$BLD_TARGETS" dist
    mv *.zip $GPDB_DIR/
EOF
}

function download() {
    scp -P $REMOTE_PORT $REMOTE_USER@$REMOTE_HOST:$GPDB_DIR/*.zip $ROOT_DIR/gpdb_artifacts/
}

# Since we are cloning and building on remote machine,
# files won't be deleted as concourse container destroys.
# We have to clean everything for success build.
function cleanup() {
    ssh -T -p $REMOTE_PORT $REMOTE_USER@$REMOTE_HOST <<- EOF
    rm -rf $GPDB_DIR
EOF
}

function _main() {

    if [ -z "$REMOTE_PORT" ]; then
        REMOTE_PORT=22
    fi

    time setup_ssh_keys
    time remote_setup
    time copy_to_remote
    time remote_compile
    time download
    time cleanup
}

_main "$@"

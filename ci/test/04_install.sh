#!/usr/bin/env bash
#
# Copyright (c) 2018-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

if [[ $QEMU_USER_CMD == qemu-s390* ]]; then
  export LC_ALL=C
fi

if [ "$CI_OS_NAME" == "macos" ]; then
  sudo -H pip3 install --upgrade pip
  # shellcheck disable=SC2086
  IN_GETOPT_BIN="/usr/local/opt/gnu-getopt/bin/getopt" ${CI_RETRY_EXE} pip3 install --user $PIP_PACKAGES
fi

# Create folders that are mounted into the docker
mkdir -p "${CCACHE_DIR}"
mkdir -p "${PREVIOUS_RELEASES_DIR}"

export ASAN_OPTIONS="detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1"
export LSAN_OPTIONS="suppressions=${BASE_BUILD_DIR}/test/sanitizer_suppressions/lsan"
export TSAN_OPTIONS="suppressions=${BASE_BUILD_DIR}/test/sanitizer_suppressions/tsan:halt_on_error=1"
export UBSAN_OPTIONS="suppressions=${BASE_BUILD_DIR}/test/sanitizer_suppressions/ubsan:print_stacktrace=1:halt_on_error=1:report_error_type=1"
env | grep -E '^(BASE_|QEMU_|CCACHE_|LC_ALL|BOOST_TEST_RANDOM|DEBIAN_FRONTEND|CONFIG_SHELL|(ASAN|LSAN|TSAN|UBSAN)_OPTIONS|PREVIOUS_RELEASES_DIR))' | tee /tmp/env
if [[ $BITCOIN_CONFIG = *--with-sanitizers=*address* ]]; then # If ran with (ASan + LSan), Docker needs access to ptrace (https://github.com/google/sanitizers/issues/764)
  DOCKER_ADMIN="--cap-add SYS_PTRACE"
fi

export P_CI_DIR="$PWD"

if [ -z "$DANGER_RUN_CI_ON_HOST" ]; then
  echo "Creating $DOCKER_NAME_TAG container to run in"
  ${CI_RETRY_EXE} docker pull "$DOCKER_NAME_TAG"

  # shellcheck disable=SC2086
  DOCKER_ID=$(docker run $DOCKER_ADMIN -idt \
                  --mount type=bind,src=$BASE_ROOT_DIR,dst=/ro_base,readonly \
                  --mount type=bind,src=$CCACHE_DIR,dst=$CCACHE_DIR \
                  --mount type=bind,src=$DEPENDS_DIR,dst=$DEPENDS_DIR \
                  --mount type=bind,src=$PREVIOUS_RELEASES_DIR,dst=$PREVIOUS_RELEASES_DIR \
                  -w $BASE_ROOT_DIR \
                  --env-file /tmp/env \
                  --name $CONTAINER_NAME \
                  $DOCKER_NAME_TAG)
  export DOCKER_CI_CMD_PREFIX="docker exec $DOCKER_ID"
else
  echo "Running on host system without docker wrapper"
fi

CI_EXEC () {
  $DOCKER_CI_CMD_PREFIX bash -c "export PATH=$BASE_SCRATCH_DIR/bins/:\$PATH && cd \"$P_CI_DIR\" && $*"
}
export -f CI_EXEC

if [ -n "$DPKG_ADD_ARCH" ]; then
  CI_EXEC dpkg --add-architecture "$DPKG_ADD_ARCH"
fi

if [[ $DOCKER_NAME_TAG == *centos* ]]; then
  CI_EXEC yum -y install epel-release
  CI_EXEC yum -y install "$DOCKER_PACKAGES" "$PACKAGES"
elif [ "$CI_USE_APT_INSTALL" != "no" ]; then
  ${CI_RETRY_EXE} CI_EXEC apt-get update
  ${CI_RETRY_EXE} CI_EXEC apt-get install --no-install-recommends --no-upgrade -y "$PACKAGES" "$DOCKER_PACKAGES"
  if [ -n "$PIP_PACKAGES" ]; then
    # shellcheck disable=SC2086
    ${CI_RETRY_EXE} pip3 install --user $PIP_PACKAGES
  fi
fi

if [ "$CI_OS_NAME" == "macos" ]; then
  top -l 1 -s 0 | awk ' /PhysMem/ {print}'
  echo "Number of CPUs: $(sysctl -n hw.logicalcpu)"
else
  CI_EXEC free -m -h
  CI_EXEC echo "Number of CPUs \(nproc\):" \$\(nproc\)
  CI_EXEC echo "$(lscpu | grep Endian)"
fi
CI_EXEC echo "Free disk space:"
CI_EXEC df -h

if [ "$RUN_FUZZ_TESTS" = "true" ]; then
  export DIR_FUZZ_IN=${DIR_QA_ASSETS}/fuzz_seed_corpus/
  if [ ! -d "$DIR_FUZZ_IN" ]; then
    CI_EXEC git clone --depth=1 https://github.com/bitcoin-core/qa-assets "${DIR_QA_ASSETS}"
  fi
  (
    CI_EXEC cd "${DIR_QA_ASSETS}"
    CI_EXEC echo "Using qa-assets repo from commit ..."
    CI_EXEC git log -1
  )
elif [ "$RUN_UNIT_TESTS" = "true" ] || [ "$RUN_UNIT_TESTS_SEQUENTIAL" = "true" ]; then
  export DIR_UNIT_TEST_DATA=${DIR_QA_ASSETS}/unit_test_data/
  if [ ! -d "$DIR_UNIT_TEST_DATA" ]; then
    CI_EXEC mkdir -p "$DIR_UNIT_TEST_DATA"
    CI_EXEC curl --location --fail https://github.com/bitcoin-core/qa-assets/raw/main/unit_test_data/script_assets_test.json -o "${DIR_UNIT_TEST_DATA}/script_assets_test.json"
  fi
fi

CI_EXEC mkdir -p "${BASE_SCRATCH_DIR}/sanitizer-output/"

if [ -z "$DANGER_RUN_CI_ON_HOST" ]; then
  echo "Create $BASE_ROOT_DIR"
  CI_EXEC rsync -a /ro_base/ "$BASE_ROOT_DIR"
fi

if [ "$USE_BUSY_BOX" = "true" ]; then
  echo "Setup to use BusyBox utils"
  CI_EXEC mkdir -p "${BASE_SCRATCH_DIR}/bins/"
  # tar excluded for now because it requires passing in the exact archive type in ./depends (fixed in later BusyBox version)
  # find excluded for now because it does not recognize the -delete option in ./depends (fixed in later BusyBox version)
  # ar excluded for now because it does not recognize the -q option in ./depends (unknown if fixed)
  # shellcheck disable=SC1010
  CI_EXEC for util in \$\(busybox --list \| grep -v "^ar$" \| grep -v "^tar$" \| grep -v "^find$"\)\; do ln -s \$\(command -v busybox\) "${BASE_SCRATCH_DIR}/bins/\$util"\; done
  # Print BusyBox version
  CI_EXEC patch --help
fi

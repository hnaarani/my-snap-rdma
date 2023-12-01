#!/bin/bash -eEl
set -o pipefail

progname=$(basename $0)

function usage()
{
	cat << HEREDOC
   Usage: $progname [--pre_script "./autogen.sh;./configure"] [--build_cmd "make all"] [--ignore_files "devx gtest"]  [--verbose]
   optional arguments:
     -h, --help           			show this help message and exit
     -p, --pre_script STRING        Preparation commands to run prior running coverity
     -b, --build_script STRING      Build command to pass to coverity
     -i, --ignore_files STRING      Space separated list of files/dirs to ignore
     -d, --defects-expected STRING  Number of defects to be expected (default: 0)
     --url STRING                   Coverity Server URL
     --user STRING                  Login to Coverity Server
     --password STRING              Password to Coverity Server
     --stream STRING                Stream on Coverity Server where to upload the report
     --upload                       Upload report to Coverity Server (--url, --user, --password are required)
     --check                        Run Coverity in check mode (don't fail if defects were found)
     -v, --verbose        	    increase the verbosity of the bash script
HEREDOC
exit 0
}

while [[ "$#" -gt 0 ]]; do
    case $1 in
        -p|--pre_script) pre_cmd="$2"; shift ;;
        -b|--build_script) build_cmd="$2"; shift ;;
        -i|--ignore_files) ignore_list="$2"; shift ;;
	-d|--defects-expected) defects_expected="$2"; shift ;;
	--url) url="$2"; shift ;;
	--user) user="$2"; shift ;;
	--password) password="$2"; shift ;;
	--stream) stream="$2"; shift ;;
	--upload) upload=true;;
	--check) check=true;;
        -h|--help) usage ;;
        -v|--verbose) set +x ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

if [ ! -d .git ]; then
	echo "Error: should be run from project root"
	exit 1
fi

if [ ! -z ${upload} ]; then
	[ ! -z "$url" ] || { echo "Error: --url must be provided when --upload is set!"; exit 1; }
	[ ! -z "$user" ] || { echo "Error: --user must be provided when --upload is set!"; exit 1; }
	[ ! -z "$password" ] || { echo "Error: --password must be provided when --upload is set!"; exit 1; }
	[ ! -z "$stream" ] || { echo "Error: --stream must be provided when --upload is set!"; exit 1; }
fi


ncpus=$(cat /proc/cpuinfo|grep processor|wc -l)
DEFECTS_EXPECTED="${defects_expected:=0}"

# Current coverity version (2021.12) supports GCC <= 9
if ! command -v gcc-9 &> /dev/null; then
	echo "Error: gcc-9 is not installed!"
	exit 1
fi

export CC=gcc-9

if [ -n "${pre_cmd}" ]; then

    echo "==== Running Pre-commands ===="

    set +eE
    /bin/bash -c "$pre_cmd"
    rc=$?

    if [ $rc -ne 0 ]; then
        echo pre-commands failed
        exit 1
    fi

    set -eE
fi

cov_build="cov_build"
rm -rf $cov_build

echo "==== Running coverity ===="

export PATH=$PATH:/auto/sw_tools/Commercial/Synopsys/Coverity/latest/linux_x86_64/bin

cov-build --dir $cov_build $build_cmd

if [ -n "${ignore_list}" ]; then

    echo "==== Adding ignore list ===="

    for item in ${ignore_list}; do
        cov-manage-emit --dir ${cov_build} --tu-pattern "file('${item}')" delete ||:
    done
fi

echo "==== Running anaysis ===="

cov-analyze --jobs 1 --security \
	    --enable INTEGER_OVERFLOW \
	    --enable AUDIT.SPECULATIVE_EXECUTION_DATA_LEAK \
	    --concurrency --dir $cov_build

if [ ! -z ${upload} ]; then

    echo "==== Uploading report ===="

    cov-commit-defects --ssl --on-new-cert trust \
	    --url $url --user $user --password $password \
	    --dir $cov_build \
	    --stream $stream
fi

cov-format-errors --dir $cov_build --html-output $cov_build/html

nerrors=$(cov-format-errors --dir $cov_build --emacs-style |& tee $cov_build/coverity.log | grep 'Type:' | wc -l )

echo -e "Number of Defects: ${nerrors} (expected $DEFECTS_EXPECTED)\n"

if (( $nerrors > $DEFECTS_EXPECTED )); then
    echo "FAIL"
    echo "New defects were added."
    echo "Number of defects ($nerrors) > ($DEFECTS_EXPECTED) defects expected!"
    echo "Please fix new defects or mark them as false-positive by incrementing the DEFECTS_EXPECTED in .ci/job_matrix.yaml and/or .ci/job_matrix_dev.yaml"
elif (( $nerrors < $DEFECTS_EXPECTED )); then
    echo "FAIL"
    echo "Defects were removed without updating the expected number."
    echo "Number of defects ($nerrors) < ($DEFECTS_EXPECTED) defects expected!"
    echo "Please update DEFECTS_EXPECTED to $nerrors in .ci/job_matrix.yaml and/or .ci/job_matrix_dev.yaml"
fi

if [ ! -z ${check} ]; then
    exit 0
fi

if (( $nerrors != $DEFECTS_EXPECTED )); then 
    exit $nerrors
fi

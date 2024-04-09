#!/bin/sh
if [ $# -lt 4 ]; then
        echo "Usage: aes|noaes file-of-probe-names.txt test-kernel initramd.img"
        exit 1
fi

AESVAL="${1}"
if [ "${AESVAL}" = "aes" ]
then
	CPUTYPE="Skylake-Server-v5"
elif [ "${AESVAL}" = "noaes" ]
then
	CPUTYPE="Skylake-Server-v5,-aes"
else
        echo "Usage: aes|noaes file-of-probe-names.txt test-kernel initramd.img"
        exit 1
fi
PROBE_FILE="${2}"
TEST_KERNEL="${3}"
INITRAMD="${4}"

#
# Read the fail probe names into an array.
#
readarray -t failure_probes <"${PROBE_FILE}"

count=0
got_error=0
rm -f ./kout-fips* >&/dev/null

for i in ${failure_probes[@]}
do
	if [[ ${i} =~ ^#.* ]]
	then
		count=$((count+1))
		continue
	fi
	echo "${count}: Requesting failure on $i"
	rm -f ./kout-fips >&/dev/null
	/usr/libexec/qemu-kvm -kernel "${TEST_KERNEL}" -initrd "${INITRAMD}" \
		-nographic -append 'console=ttyS0 root=/dev/ram0 '${i}' fips=1' -m 16g -cpu "${CPUTYPE}" >& ./kout-fips </dev/null &
	subpid=$!
	got_panic=0
	#
	# Wait 60 seconds for the panic. FIXME - Is this enough ?
	#
	for j in {1..10}
	do
		sleep 6
		grep 'Kernel panic - not syncing:' ./kout-fips >&/dev/null
		if [ $? = 0 ]; then
			got_panic=1
			break
		fi
	done
	kill ${subpid}
	if [ $got_panic = 0 ]; then
		# Restore terminal state (QEMU messes it up).
		echo ""
		echo "ERROR: FIPS failure probe"
		echo "${i}"
		echo "did not panic kernel ${TEST_KERNEL}"
		mv ./kout-fips ./kout-fips.${count}
		got_error=$((got_error+1))
	else
		rm -f ./kout-fips >&/dev/null
		echo "Kernel panic (expected)"
	fi
	count=$((count+1))
done

if [ $got_error = 0 ]; then
	echo "All failure probes correctly caused kernel failure"
else
	echo "Encounted $got_error errors"
fi
exit 0

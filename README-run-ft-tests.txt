HOW TO RUN THE KERNEL FIPS FUNCTIONAL TEST SCRIPT
=================================================

1). Installing prerequisite packages
------------------------------------

On a Rocky8 x86_64 installation:

Firstly, go to the following web page:

https://docs.rockylinux.org/guides/custom-linux-kernel/

Install all the required packages up to the statement:

"That’s it for the prerequisite packages needed for actual Kernel building!"

# There are two more packages we're going to need in order
# to create a FIPS enabled init ram disk, dracut and dracut-fips.
# Install them using the following command:

sudo dnf install dracut dracut-fips

2). Building the test kernel
----------------------------

# Now, ensure you are in the root of the checked
# out kernel FIPS functional tests branch and
# type the following commands.

# Firstly, copy the config file from the running
# system into a local .config file.

cp /boot/config-`uname -r` .config

#
# Now use the kernel config system to create
# a config file for the build.

make olddefconfig

# Set up the trusted keys for the build.

sed -ri '/CONFIG_SYSTEM_TRUSTED_KEYS/s/=.+/=""/g' .config

# Create a custom kernel version for the new build.

sed  -i 's/^EXTRAVERSION.*/EXTRAVERSION = -fips-ft/'  Makefile

# Build the kernel

make -j4

3). Setting up the test environment
-----------------------------------

# Firstly, create a test directory. I prefer
# to install this inside the git kernel source
# code directory so it can be deleted easily
# when we want to restart using the git clean
# command.

mkdir FIPS_TEST_DIR

# Install the kernel modules into our test directory.

make modules_install INSTALL_MOD_PATH=FIPS_TEST_DIR

# Copy the newly built kernel into the test directory.

cp arch/x86_64/boot/bzImage FIPS_TEST_DIR/vmlinuz-4.18.0-fips-ft

# Create the gzipped symvers file for dracut.

gzip Module.symvers >FIPS_TEST_DIR/lib/modules/4.18.0-fips-ft/symvers.gz

# Now we need to create a FIPS init ram disk containing the newly
# created kernel and modules directory.

# First, chdir into our FIPS_TEST_DIR as we'll be running
# the dracut command from there.

cd FIPS_TEST_DIR

# We must run the dracut command as root, as it requires
# this for a fips init ram disk creation. Note the '\'
# continuation at the end of this line as both lines
# are needed for the dracut command.

sudo dracut -N --add fips --kver 4.18.0-fips-ft --kernel-image vmlinuz-4.18.0-fips-ft \
--kmoddir="lib/modules/4.18.0-fips-ft" ramdisk.img

# dracut creates the ram disk as root with no read
# access for another user, so we need to change ownership
# to our current user to run the tests.

sudo chown $USER ramdisk.img

4). Running the kernel FIPS functional tests.
---------------------------------------------

# Remember our current working directory is FIPS_TEST_DIR
# inside the checked out kernel source code, so we need
# to use ../ to get to the scripts and test probe files.

# The FIPS functional tests run the new kernel under qemu
# and can test an emulated x86_64 CPU with Intel AESNI instructions
# enabled as follows.

../testkernel.sh aes ../kernel-fail-probes-aes.txt vmlinuz-4.18.0-fips-ft ramdisk.img

# And can test an emulated x86_64 CPU with Intel AESNI instructions
# disabled as follows.

../testkernel.sh noaes ../kernel-fail-probes-nonaes.txt vmlinuz-4.18.0-fips-ft ramdisk.img

# Both these tests should complete without error. If you have
# problems look inside the testkernel.sh script to see what
# might not be working for you.


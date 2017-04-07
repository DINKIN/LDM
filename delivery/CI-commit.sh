# This script is intended to be executed on the continuous-integration server
# and does the following:
#     1. Builds the package;
#     2. Tests the package; and
#     3. Creates a source-distribution.
#
# Preconditions:
#     - The current working directory is the top-level source-directory; and
#     - File `release-vars.sh` is in the same directory as this script.

set -e  # exit if error

# Get the static release variables
#
. `dirname $0`/release-vars.sh

#
# Build and test the package and create a source distribution.
#
PATH=/usr/bin:$PATH
mkdir -p m4 mcast_lib/fmtp/m4
autoreconf -if
./configure --enable-debug --disable-root-actions >configure.log 2>&1

# Assumes AM_DISTCHECK_CONFIGURE_FLAGS contains all appropriate options
make distcheck
make distcheck DISTCHECK_CONFIGURE_FLAGS=--with-ulog 

# DISTCHECK_CONFIGURE_FLAGS appends to AM_DISTCHECK_CONFIGURE_FLAGS, which
# exists in automake(1) 1.13 but not in 1.11.
#COM_OPTS='--disable-root-actions'
##make distcheck DISTCHECK_CONFIGURE_FLAGS='--with-multicast '$COM_OPTS
#make distcheck DISTCHECK_CONFIGURE_FLAGS='--with-gribinsert '$COM_OPTS
#make distcheck DISTCHECK_CONFIGURE_FLAGS='--with-noaaport '$COM_OPTS
#make distcheck DISTCHECK_CONFIGURE_FLAGS='--with-noaaport --with-retrans '$COM_OPTS

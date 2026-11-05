#!/bin/sh
# The diff is against current.  The 11.0 vnd.c differs in one line of a
# hunk's context: put that in the current form for patch, then back.
#   patch-11.sh DIFF     (in the source tree)
set -eu
perl -0pi -e 's/curlwp != vnd->sc_kthread && curlwp != uvm\.pagedaemon_lwp\) \{/curlwp != vnd->sc_kthread &&\n\t\t    !uvm_lwp_is_pagedaemon(curlwp)) {/' sys/dev/vnd.c
patch -f -p1 -i "$1" </dev/null
perl -0pi -e 's/\n\t\t    !uvm_lwp_is_pagedaemon\(curlwp\)\) \{/ curlwp != uvm.pagedaemon_lwp) {/' sys/dev/vnd.c
grep -q 'curlwp != uvm.pagedaemon_lwp' sys/dev/vnd.c

#!/bin/sh
#
# Stands in for the local MTA in the identity topology.
#
# The sendmail module's whole job is to hand a composed message to a
# program on stdin and report what it made of it; what that program does
# with the message is not the ircd's business.  This one keeps it, under
# the debug volume, which is bind-mounted on the host -- so a test can
# read the message the server actually sent, token and all, instead of
# asserting that a send was attempted and hoping.
#
# Every message is appended, separated by a line no message can contain.

out=/opt/ircu/debug/sendmail.mbox

printf -- '--- message %s ---\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$out"
cat >> "$out"
printf -- '--- end (args: %s) ---\n' "$*" >> "$out"

exit 0

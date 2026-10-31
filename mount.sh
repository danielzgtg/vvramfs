#!/bin/sh
fusermount -u /tmp/vram 2>/dev/null || :
exec bin/vvramfs /tmp/vram 4G

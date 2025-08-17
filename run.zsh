#!/usr/bin/env zsh
set -e

MODE="${1:-local}"  # local | remote

RELEASE="${2:-debug}"

xmake f -m $RELEASE
xmake

client_path="build/linux/x86_64/${RELEASE}/client"
server_path="build/linux/x86_64/${RELEASE}/server"


remote_host="${REMOTE_HOST:-venus}"
remote_dir="$PWD"

if [[ "$MODE" == "local" ]]; then
  client_args="-l 0-3 --vdev=net_memif0,role=slave,socket=/tmp/memif.sock --file-prefix=client"
  server_args="-l 4-7 --vdev=net_memif0,role=master,socket=/tmp/memif.sock --file-prefix=server"
  tmux new-session -d -s sigproc \; \
  split-window -h \; \
  send-keys -t 0 "$client_path $client_args" C-m \; \
  send-keys -t 1 "$server_path $server_args" C-m \; \
  attach
else
  abs_server_path="$remote_dir/$server_path"
  abs_server_dir="${abs_server_path:h}"
  ssh "$remote_host" "mkdir -p '$abs_server_dir'"
  scp "$server_path" "$remote_host":"$abs_server_path"

  sudo $client_path -l 0-3 -a 2a:00.1
fi

tmux kill-session -t sigproc || true

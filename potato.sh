#! /bin/bash

export API_TOKEN="dev"
export DEBUG=1
export MAP_CENTER="39.5296,-119.8138"
export XDG_DATA_HOME="$(pwd)/.data"
mkdir -p $XDG_DATA_HOME
rbenv local 4.0.0
cd potato-mesh/web
./app.sh

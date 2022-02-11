#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e               # exit on error
source ./config.sh

pushd "$(dirname "$0")" # connect to root

ROOT_DIR=$(pwd)
echo "ROOT_DIR ${ROOT_DIR}"

USER_NAME=${SUDO_USER:=$USER}
USER_ID=$(id -u "${USER_NAME}")

# Set the home directory in the Docker container.
DOCKER_HOME_DIR=${DOCKER_HOME_DIR:-/home/${USER_NAME}}

HADOOP_HOME=/opt/hadoop/hadoop-${HADOOP_VERSION}

# Make sure server directory exists
mkdir -p ${ROOT_DIR}/server

# Create NameNode and DataNode mount points
mkdir -p ${ROOT_DIR}/volume/namenode
mkdir -p ${ROOT_DIR}/volume/datanode0
mkdir -p ${ROOT_DIR}/volume/logs

mkdir -p "${ROOT_DIR}/volume/status"
rm -f ${ROOT_DIR}/volume/status/*

# Can be used to transfer data to HDFS
mkdir -p ${ROOT_DIR}/data

CMD="bin/start-hadoop.sh"
RUNNING_MODE="daemon"

if [ "$#" -ge 1 ] ; then
  CMD="$*"
  RUNNING_MODE="interactive"
fi

if [ "$RUNNING_MODE" = "interactive" ]; then
  DOCKER_IT="-i -t"
fi
DOCKER_RUN="docker run --rm=true ${DOCKER_IT} \
  -v ${ROOT_DIR}/data:/data \
  -v "${ROOT_DIR}/dikeHDFS:${DOCKER_HOME_DIR}/dikeHDFS" \
  -v ${ROOT_DIR}/volume/namenode:/opt/volume/namenode \
  -v ${ROOT_DIR}/:${DOCKER_HOME_DIR}/dikeHDFSroot \
  -v ${ROOT_DIR}/volume/datanode0:/opt/volume/datanode \
  -v ${ROOT_DIR}/volume/status:/opt/volume/status \
  -v ${ROOT_DIR}/volume/logs:${HADOOP_HOME}/logs \
  -v ${ROOT_DIR}/etc/hadoop/core-site.xml:${HADOOP_HOME}/etc/hadoop/core-site.xml \
  -v ${ROOT_DIR}/etc/hadoop/hdfs-site.xml:${HADOOP_HOME}/etc/hadoop/hdfs-site.xml \
  -v ${ROOT_DIR}/scripts/start-hadoop.sh:${HADOOP_HOME}/bin/start-hadoop.sh \
  -v ${ROOT_DIR}/server:/server \
  -w ${HADOOP_HOME} \
  -e HADOOP_HOME=${HADOOP_HOME} \
  -e RUNNING_MODE=${RUNNING_MODE} \
  -e HADOOP_CONF_DIR=${HADOOP_HOME}/etc/hadoop \
  -u ${USER_ID} \
  --net host --add-host=dikehdfs:172.169.1.40 \
  --name dikehdfs --hostname dikehdfs \
  hadoop-${HADOOP_VERSION}-ndp-${USER_NAME} ${CMD}"

if [ "$RUNNING_MODE" = "interactive" ]; then
  eval "${DOCKER_RUN}"
else
  eval "${DOCKER_RUN}" &
  while [ ! -f "${ROOT_DIR}/volume/status/HADOOP_STATE" ]; do
    sleep 1  
  done

  cat "${ROOT_DIR}/volume/status/HADOOP_STATE"
  #docker exec dikehdfs /server/dikeHDFS &
fi

popd


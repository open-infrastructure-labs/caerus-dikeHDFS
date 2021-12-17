
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Dockerfile for installing the necessary dependencies for building Hadoop.
# See BUILDING.txt.

#FROM ubuntu:bionic
FROM ubuntu:focal

WORKDIR /root

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

#####
# Disable suggests/recommends
#####
RUN echo APT::Install-Recommends "0"\; > /etc/apt/apt.conf.d/10disableextras
RUN echo APT::Install-Suggests "0"\; >>  /etc/apt/apt.conf.d/10disableextras

ENV DEBIAN_FRONTEND noninteractive
ENV DEBCONF_TERSE true
ENV TZ=US/Eastern

# Install Hadoop dependencies
RUN apt-get -q update \
    && apt-get -q install -y --no-install-recommends \
    openjdk-8-jdk \
    net-tools \
    curl \
    netcat \
    gnupg \
    libsnappy-dev liblz4-dev libzstd-dev

RUN mkdir -p /opt/hadoop
ARG HADOOP_VERSION
ENV ENV_HADOOP_VERSION=$HADOOP_VERSION
ENV JAVA_HOME /usr/lib/jvm/java-8-openjdk-amd64

RUN mkdir -p /opt/hadoop
RUN curl -L https://downloads.apache.org/hadoop/common/hadoop-${ENV_HADOOP_VERSION}/hadoop-${ENV_HADOOP_VERSION}.tar.gz | tar -xz -C /opt/hadoop

# Install dikeV3 dependencies
RUN apt-get -q update \
    && apt-get -q install -y --no-install-recommends \
        python3-pip python3-venv python3-wheel

RUN pip3 install pandas sqlparse duckdb fastparquet

# SSH support
RUN apt-get -q update \
    && apt-get -q install -y --no-install-recommends \
        keychain \
        ssh \
        openssh-server 


RUN apt-get clean && rm -rf /var/lib/apt/lists/*

EXPOSE 22

COPY entry.sh /usr/local/bin/entry.sh
RUN chmod a+x /usr/local/bin/entry.sh

ENTRYPOINT ["/usr/local/bin/entry.sh"]

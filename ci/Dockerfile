# Debian 10 shares UOS 20's glibc 2.28 and Qt 5.11 baseline.
FROM debian:buster-slim@sha256:bb3dc79fddbca7e8903248ab916bb775c96ec61014b3d02b4f06043b604726dc
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen
COPY install-build-deps.sh /opt/dock-hardware-ci/
RUN bash /opt/dock-hardware-ci/install-build-deps.sh
COPY build-dtk.sh dtk-sources.tsv qt511-dtkwidget.patch /opt/dock-hardware-ci/
RUN bash /opt/dock-hardware-ci/build-dtk.sh && rm -rf /tmp/dock-hardware-dtk
WORKDIR /project

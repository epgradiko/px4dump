FROM localhost/base_alpine:3.20 as px4dump
COPY ./px4dump /tmp/px4dump
RUN apk add autoconf automake gcc git g++ make && \
    cd /tmp/px4dump && \
    ./autogen.sh && \
    ./configure && \
    make && make install
FROM scratch
COPY --from=px4dump /usr/local/bin /usr/local/bin

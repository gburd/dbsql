#!/bin/bash
cd /home/gburd/ws/dbsql/build_unix.old
make api.lo && make libdbsql-0.4.la && echo "Build successful"

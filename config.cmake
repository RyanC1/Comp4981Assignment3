set(PROJECT_NAME "Comp4981Assignment3")
set(PROJECT_VERSION "1.0.0")
set(PROJECT_DESCRIPTION "Comp4981Assignment3")
set(PROJECT_LANGUAGE "C")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Common compiler flags
set(STANDARD_FLAGS
        -D_POSIX_C_SOURCE=200809L
        -D_XOPEN_SOURCE=700
        #-D_GNU_SOURCE
        #-D_DARWIN_C_SOURCE
        #-D__BSD_VISIBLE
        -Werror
)

# Define targets
set(LIBRARY_TARGETS http)
set(EXECUTABLE_TARGETS server
                       database-query)

set(http_SOURCES
        src/http.c
        src/util.c
        src/db_util.c
)

set(http_HEADERS
)

set(http_LINK_LIBRARIES
        gdbm
        gdbm_compat
        m
)

set(server_SOURCES
        src/main.c
        src/watcher.c
        src/worker.c
        src/socket_util.c
        src/signal_util.c
        src/util.c
)

set(server_HEADERS
        include/arguments.h
        include/errors.h
        include/context.h
)

set(server_LINK_LIBRARIES
        p101_error
        p101_env
        p101_c
        p101_posix
        p101_unix
        p101_fsm
        p101_convert
        http
        m
)

set(database-query_SOURCES
        src/database_query.c
        src/db_util.c
        src/signal_util.c
)

set(database-query_HEADERS
        include/errors.h
)

set(database-query_LINK_LIBRARIES
        gdbm
        gdbm_compat
        p101_error
        p101_env
        p101_c
        p101_posix
        p101_unix
        p101_fsm
        p101_convert
        http
        m
)
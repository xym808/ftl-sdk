cmake_minimum_required (VERSION 2.6)
enable_language(C)
project(libftl)

include_directories(libftl)

if (WIN32)
  set(CHARON_PLATFORM_FILES charon/win32/xgetopt.c
                            charon/win32/xgetopt.h
                            charon/win32/ctrlc_handler.c)
  set(CHARON_PLATFORM_LIBS kernel32 user32 gdi32 advapi32)
  set(FTL_PLATFORM_FILES libftl/win32/socket.c)
else()
  set(CHARON_PLATFORM_FILES charon/posix/ctrlc_handler.c)
  set(FTL_PLATFORM_FILES libftl/posix/socket.c)
endif()

add_library(ftl SHARED libftl/hmac/hmac.c
                       libftl/hmac/hmac.h
                       libftl/hmac/sha2.c
                       libftl/hmac/sha2.h
                       libftl/activate.c
                       libftl/charon_protocol.c
                       libftl/components.c
                       libftl/deactivate.c
                       libftl/init.c
                       libftl/logging.c
                       libftl/stream_configuration.c
                       libftl/ftl.h
                       libftl/ftl_private.h
             ${FTL_PLATFORM_FILES})
set_target_properties(ftl PROPERTIES VERSION "0.2.3")
set_target_properties(ftl PROPERTIES SOVERSION 0)

if(WIN32)
  target_link_libraries(ftl ws2_32)
endif()

add_executable(charon
               charon/main.c
               charon/charon.h
               ${CHARON_PLATFORM_FILES})
target_link_libraries(charon ftl ${CHARON_PLATFORM_LIBS})
target_include_directories(charon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/charon)

# Install rules
install(TARGETS ftl DESTINATION lib)
install(FILES libftl/ftl.h DESTINATION "include/ftl")

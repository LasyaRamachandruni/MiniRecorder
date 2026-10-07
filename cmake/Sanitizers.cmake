# MR_SANITIZE accepts a comma separated list understood by -fsanitize=,
# e.g. "address,undefined" or "thread". Applied globally so that every
# target (library, tests, tools) is instrumented consistently.

if(MR_SANITIZE)
  message(STATUS "MiniRecorder: building with -fsanitize=${MR_SANITIZE}")
  add_compile_options(-fsanitize=${MR_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${MR_SANITIZE})
  if(MR_SANITIZE MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()

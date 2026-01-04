if (NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED VAR)
  message(FATAL_ERROR "INPUT, OUTPUT, and VAR must be set")
endif()

file(READ "${INPUT}" CONTENT)
string(REPLACE "\r\n" "\n" CONTENT "${CONTENT}")
string(REPLACE "\r" "\n" CONTENT "${CONTENT}")

string(REPLACE "\\" "\\\\" CONTENT "${CONTENT}")
string(REPLACE "\"" "\\\"" CONTENT "${CONTENT}")
string(REPLACE "\n" "\\n\"\n\"" CONTENT "${CONTENT}")

set(HEADER "// Generated from ${INPUT}. Do not edit.\n")
string(APPEND HEADER "#pragma once\n\n")
string(APPEND HEADER "static const char ${VAR}[] =\n")
string(APPEND HEADER "\"${CONTENT}\\n\";\n")

file(WRITE "${OUTPUT}" "${HEADER}")

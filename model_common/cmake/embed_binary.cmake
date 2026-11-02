# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# cmake -DIN=<file> -DOUT=<header> -DVAR=<identifier> -P embed_binary.cmake
#
# Writes <header> declaring `static const unsigned char <VAR>[]` with the
# bytes of <file> (plus `<VAR>_size`). Used for the rigged hand mesh, which is
# compiled into the binary so no installer, app bundle or APK has to ship it.
file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _bytes "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _body "${_hex}")
# Break the single line every 32 bytes so compilers and editors stay happy.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _body "${_body}")
file(WRITE "${OUT}"
"// Generated from ${IN} by embed_binary.cmake. Do not edit.\n#pragma once\n#include <cstddef>\nstatic const unsigned char ${VAR}[] = {\n${_body}\n};\nstatic const size_t ${VAR}_size = ${_bytes};\n")

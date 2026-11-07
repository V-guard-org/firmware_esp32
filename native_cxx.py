# Faz o ambiente native imitar o ESP32 em C++: sem exceções e sem RTTI (proposta §4.8).
# -fno-rtti só vale para C++; em build_flags ele também iria para os .c (Unity) e, com
# -Werror, quebraria o build. Por isso entra só em CXXFLAGS.
# Origem: native_cxx.py do firmware_telemetria (origin/main).
Import("env")  # noqa: F821 (injetado pelo SCons do PlatformIO)

env.Append(CXXFLAGS=["-fno-exceptions", "-fno-rtti"])  # noqa: F821

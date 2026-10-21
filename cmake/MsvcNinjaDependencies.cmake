# Ninja parses /showIncludes output using an exact byte prefix. Localized
# diagnostics can be decoded differently during compiler detection and during
# compilation (ninja-build/ninja#1766), silently dropping header dependencies.
# Translate MSVC's language-independent JSON dependencies into Ninja's ASCII
# protocol. VSLANG alone cannot help installations without an English pack.
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_C_CL_SHOWINCLUDES_PREFIX}")
    list(PREPEND CMAKE_C_COMPILER_LAUNCHER
        "${CMAKE_COMMAND}" "-P" "${CMAKE_CURRENT_LIST_DIR}/MsvcNinjaCompile.cmake" "--")
endif()

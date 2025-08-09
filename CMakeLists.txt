# Add our source directory to the list of public include directories
# This will be used by the parent CMakeLists.txt to configure the 'modules' target
list(APPEND PUBLIC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)

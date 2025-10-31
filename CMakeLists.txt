# 获取当前源文件所在目录的绝对路径
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录下所有源文件添加到 SRC_LIST 变量中
aux_source_directory(${SRC_DIR} SRC_LIST)

# 查找pthread库
find_package(Threads REQUIRED)

add_library(fiber_lib ${SRC_LIST})
target_link_libraries(fiber_lib -ldl Threads::Threads)
target_include_directories(fiber_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
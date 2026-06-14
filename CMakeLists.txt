# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(WIN32)
    set(_git_patch_dir "C:/Program Files/Git/usr/bin")
    if(EXISTS "${_git_patch_dir}/patch.exe")
        file(TO_CMAKE_PATH "$ENV{PATH}" _env_path)
        list(FIND _env_path "${_git_patch_dir}" _git_patch_dir_index)
        if(_git_patch_dir_index EQUAL -1)
            set(ENV{PATH} "${_git_patch_dir};$ENV{PATH}")
        endif()
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_world)

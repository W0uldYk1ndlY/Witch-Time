# Source only; the submodule commit is also checked so upstream cannot float.
include(FetchContent)
FetchContent_Declare(wt_adrenotools
    GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
    GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
    GIT_SUBMODULES lib/linkernsbypass
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_GetProperties(wt_adrenotools)
if(NOT wt_adrenotools_POPULATED)
    FetchContent_Populate(wt_adrenotools)
endif()
find_package(Git REQUIRED)
foreach(entry IN ITEMS ".|8fae8ce254dfc1344527e05301e43f37dea2df80" "lib/linkernsbypass|aa3975893d83ef1bc84c321ec60c65fbf1287887")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 dir)
    list(GET parts 1 revision)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${wt_adrenotools_SOURCE_DIR}/${dir}" rev-parse HEAD
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
    if(NOT result EQUAL 0 OR NOT actual STREQUAL revision)
        message(FATAL_ERROR "GPU driver dependency revision mismatch: ${dir}")
    endif()
endforeach()
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${wt_adrenotools_SOURCE_DIR}" "${wt_adrenotools_BINARY_DIR}")
foreach(target hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endforeach()

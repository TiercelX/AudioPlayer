if(NOT DEFINED WINDEPLOYQT_EXECUTABLE OR NOT EXISTS "${WINDEPLOYQT_EXECUTABLE}")
    message(FATAL_ERROR "WINDEPLOYQT_EXECUTABLE is not available.")
endif()

if(NOT DEFINED SOURCE_EXE OR NOT EXISTS "${SOURCE_EXE}")
    message(FATAL_ERROR "SOURCE_EXE is not available.")
endif()

if(NOT DEFINED PLAYABLE_ROOT)
    message(FATAL_ERROR "PLAYABLE_ROOT is not set.")
endif()

string(TIMESTAMP deploy_timestamp "%Y%m%d-%H%M%S")
set(deploy_dir "${PLAYABLE_ROOT}/${deploy_timestamp}")
file(MAKE_DIRECTORY "${deploy_dir}")

get_filename_component(exe_name "${SOURCE_EXE}" NAME)
set(playable_exe "${deploy_dir}/${exe_name}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_EXE}" "${playable_exe}"
    RESULT_VARIABLE copy_result
)
if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to copy executable into playable directory.")
endif()

set(windeployqt_args)
if(DEFINED DEBUG_FLAG AND NOT DEBUG_FLAG STREQUAL "")
    list(APPEND windeployqt_args "${DEBUG_FLAG}")
endif()
if(DEFINED RELEASE_FLAG AND NOT RELEASE_FLAG STREQUAL "")
    list(APPEND windeployqt_args "${RELEASE_FLAG}")
endif()
list(APPEND windeployqt_args
    --no-translations
    --dir "${deploy_dir}"
    "${playable_exe}"
)

execute_process(
    COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${windeployqt_args}
    RESULT_VARIABLE deploy_result
)
if(NOT deploy_result EQUAL 0)
    message(FATAL_ERROR "windeployqt failed for ${playable_exe}")
endif()

function(copy_tool_if_present tool_path tool_name)
    if(NOT "${tool_path}" STREQUAL "" AND EXISTS "${tool_path}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${tool_path}" "${deploy_dir}/${tool_name}"
            RESULT_VARIABLE tool_copy_result
        )
        if(NOT tool_copy_result EQUAL 0)
            message(FATAL_ERROR "Failed to copy ${tool_name} into playable directory.")
        endif()
    endif()
endfunction()

if(NOT DEFINED FFMPEG_DEPLOY_NAME OR FFMPEG_DEPLOY_NAME STREQUAL "")
    set(FFMPEG_DEPLOY_NAME "ffmpeg.exe")
endif()
if(NOT DEFINED FFPROBE_DEPLOY_NAME OR FFPROBE_DEPLOY_NAME STREQUAL "")
    set(FFPROBE_DEPLOY_NAME "ffprobe.exe")
endif()

copy_tool_if_present("${FFMPEG_EXECUTABLE}" "${FFMPEG_DEPLOY_NAME}")
copy_tool_if_present("${FFPROBE_EXECUTABLE}" "${FFPROBE_DEPLOY_NAME}")

function(remove_path_if_present candidate_path)
    if(EXISTS "${candidate_path}")
        file(REMOVE_RECURSE "${candidate_path}")
    endif()
endfunction()

if(PRUNE_QT_FFMPEG_RUNTIME)
    remove_path_if_present("${deploy_dir}/multimedia/ffmpegmediaplugin.dll")
    remove_path_if_present("${deploy_dir}/multimedia/ffmpegmediaplugind.dll")
    file(GLOB _qt_av_dlls "${deploy_dir}/avcodec-*.dll"
                           "${deploy_dir}/avformat-*.dll"
                           "${deploy_dir}/avutil-*.dll"
                           "${deploy_dir}/swresample-*.dll"
                           "${deploy_dir}/swscale-*.dll")
    foreach(_dll IN LISTS _qt_av_dlls)
        file(REMOVE "${_dll}")
    endforeach()
endif()

file(WRITE "${PLAYABLE_ROOT}/LATEST.txt" "${deploy_dir}\n")

file(GLOB playable_entries RELATIVE "${PLAYABLE_ROOT}" "${PLAYABLE_ROOT}/*")
set(playable_dirs)
foreach(entry IN LISTS playable_entries)
    if(IS_DIRECTORY "${PLAYABLE_ROOT}/${entry}")
        list(APPEND playable_dirs "${entry}")
    endif()
endforeach()

list(SORT playable_dirs)
list(LENGTH playable_dirs playable_dir_count)
if(playable_dir_count GREATER 3)
    math(EXPR stale_count "${playable_dir_count} - 3")
    math(EXPR stale_last_index "${stale_count} - 1")
    foreach(index RANGE 0 ${stale_last_index})
        list(GET playable_dirs ${index} stale_dir)
        file(REMOVE_RECURSE "${PLAYABLE_ROOT}/${stale_dir}")
    endforeach()
endif()

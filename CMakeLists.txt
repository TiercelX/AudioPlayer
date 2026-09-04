cmake_minimum_required(VERSION 3.19)
project(AudioPlayer LANGUAGES CXX)

set(AUDIOPLAYER_FFMPEG_AUDIO_CORE_VERSION "9.0.1")
set(AUDIOPLAYER_DEPLOY_FFMPEG_EXECUTABLE "" CACHE STRING
    "Path to the self-built audio-core ffmpeg executable bundled into playable builds. Leave empty to use AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT/bin/ffmpeg.exe.")
set(AUDIOPLAYER_DEPLOY_FFPROBE_EXECUTABLE "" CACHE STRING
    "Path to the self-built audio-core ffprobe executable bundled into playable builds. Leave empty to use AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT/bin/ffprobe.exe.")
option(AUDIOPLAYER_PRUNE_QT_FFMPEG_RUNTIME
    "Remove Qt Multimedia's FFmpeg backend files from deployed playable bundles."
    ON)
option(AUDIOPLAYER_ENABLE_MSVC_RELEASE_LTCG
    "Enable MSVC whole-program and link-time optimization for Release-like builds."
    ON)
option(AUDIOPLAYER_ENABLE_AVX2
    "Build app sources with /arch:AVX2. Off by default to keep x64 bundles portable."
    OFF)
if(WIN32)
    option(AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE
        "Fail configure unless the self-built FFmpeg audio-core runtime tools are available."
        ON)
else()
    option(AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE
        "Fail configure unless the self-built FFmpeg audio-core runtime tools are available."
        OFF)
endif()
option(AUDIOPLAYER_REQUIRE_LIBAV_DECODER
    "Fail configure when the in-process libav decoder cannot be enabled."
    ${AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE})

if(WIN32 AND NOT Qt6_DIR AND NOT CMAKE_PREFIX_PATH)
    foreach(qt_root "D:/Qt" "C:/Qt")
        if(NOT EXISTS "${qt_root}")
            continue()
        endif()

        file(GLOB qt_msvc_prefixes LIST_DIRECTORIES true "${qt_root}/*/msvc*_64")
        list(SORT qt_msvc_prefixes COMPARE NATURAL ORDER DESCENDING)
        foreach(qt_msvc_prefix IN LISTS qt_msvc_prefixes)
            if(EXISTS "${qt_msvc_prefix}/lib/cmake/Qt6/Qt6Config.cmake")
                list(APPEND CMAKE_PREFIX_PATH "${qt_msvc_prefix}")
                message(STATUS "Using auto-detected Qt MSVC prefix: ${qt_msvc_prefix}")
                break()
            endif()
        endforeach()

        if(CMAKE_PREFIX_PATH)
            break()
        endif()
    endforeach()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Multimedia Concurrent)

# Linux ALSA 依赖
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
endif()

qt_standard_project_setup()

qt_add_executable(AudioPlayer
    WIN32 MACOSX_BUNDLE
    src/core/androidnativeaudioplayer.cpp
    src/core/androidnativeaudioplayer.h
    src/core/applenativeaudioplayer.cpp
    src/core/applenativeaudioplayer.h
    src/core/audioplayerbackend.cpp
    src/core/audioplayerbackend.h
    src/core/audioplayerfactory.cpp
    src/core/audioplayerfactory.h
    src/core/nativeaudioplayerstubbase.cpp
    src/core/nativeaudioplayerstubbase.h
    src/core/playbacksourceservice_prepare.cpp
    src/core/playbacksourceservice_probe.cpp
    src/core/playbacksourceservice.cpp
    src/core/playbacksourceservice.h
    src/core/playbacksourceserviceinternal.h
    src/backends/ffmpeg/ffmpegpcmshared.cpp
    src/backends/ffmpeg/ffmpegpcmshared.h
    src/backends/ffmpeg/audiooutputworker.cpp
    src/backends/ffmpeg/audiooutputworker.h
    src/backends/ffmpeg/ffmpegaudioplayer.cpp
    src/backends/ffmpeg/ffmpegaudioplayer.h
    src/backends/ffmpeg/ffmpegaudioplayer_output.cpp
    src/backends/ffmpeg/ffmpegaudioplayer_state.cpp
    src/backends/ffmpeg/dolbydownmixprocessor.cpp
    src/backends/ffmpeg/dolbydownmixprocessor.h
    src/backends/ffmpeg/libavseekdecoderworker.cpp
    src/backends/ffmpeg/libavseekdecoderworker.h
    src/backends/ffmpeg/pcmseekcache.cpp
    src/backends/ffmpeg/pcmseekcache.h
    src/backends/shared/toollocator.h
    src/backends/shared/toollocator.cpp
    src/diagnostics/audioartifactmonitor.cpp
    src/diagnostics/audioartifactmonitor.h
    src/diagnostics/diagnosticsservice.cpp
    src/diagnostics/diagnosticsservice.h
    src/diagnostics/playerlogger.cpp
    src/diagnostics/playerlogger.h
    src/ui/main.cpp
    src/ui/automationoptions.cpp
    src/ui/automationoptions.h
    src/ui/diagnosticreportbuilder.cpp
    src/ui/diagnosticreportbuilder.h
    src/ui/diagnosticsdashboard.cpp
    src/ui/diagnosticsdashboard.h
    src/ui/mainwindow_helpers.cpp
    src/ui/mainwindow_helpers.h
    src/ui/mediainfodialog.cpp
    src/ui/mediainfodialog.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.ui
    src/ui/mainwindow_output.cpp
    src/ui/mainwindow_automation.cpp
    src/ui/mainwindow_media.cpp
    src/ui/mainwindow_cache.cpp
)

target_include_directories(AudioPlayer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/ffmpeg
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/wasapi
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/shared
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/asio
        ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

# Linux ALSA 后端源文件
if(UNIX AND NOT APPLE)
    target_sources(AudioPlayer PRIVATE
        src/backends/alsa/linuxalsaaudioplayer.h
        src/backends/alsa/linuxalsaaudioplayer.cpp
        src/backends/alsa/linuxalsaaudioplayer_output.cpp
        src/backends/alsa/linuxalsaaudioplayer_state.cpp
        src/backends/alsa/alsaoutputworker.h
        src/backends/alsa/alsaoutputworker.cpp
        src/backends/alsa/alsaformatnegotiator.h
        src/backends/alsa/alsaformatnegotiator.cpp
        src/backends/alsa/alsalogic.h
        src/backends/alsa/alsalogic.cpp
    )
    target_include_directories(AudioPlayer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/alsa
    )
endif()

target_link_libraries(AudioPlayer
    PRIVATE
        Qt::Core
        Qt::Concurrent
        Qt::Widgets
        Qt::Multimedia
)

# Linux ALSA 链接
if(UNIX AND NOT APPLE)
    target_link_libraries(AudioPlayer PRIVATE ${ALSA_LIBRARIES})
    target_include_directories(AudioPlayer PRIVATE ${ALSA_INCLUDE_DIRS})
endif()

set(AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-audio-core/runtime-with-ffprobe-msvc"
    CACHE PATH
    "Self-built FFmpeg audio-core runtime used for bundled tools and in-process decoding.")
set(AUDIOPLAYER_LIBAV_DECODER_AVAILABLE OFF)
set(AUDIOPLAYER_AUDIO_CORE_REQUIRED_TOOLS
    "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/bin/ffmpeg.exe"
    "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/bin/ffprobe.exe"
)
set(AUDIOPLAYER_AUDIO_CORE_MISSING_TOOLS "")
foreach(audio_core_required_file IN LISTS AUDIOPLAYER_AUDIO_CORE_REQUIRED_TOOLS)
    if(NOT EXISTS "${audio_core_required_file}")
        list(APPEND AUDIOPLAYER_AUDIO_CORE_MISSING_TOOLS "${audio_core_required_file}")
    endif()
endforeach()
list(LENGTH AUDIOPLAYER_AUDIO_CORE_MISSING_TOOLS AUDIOPLAYER_AUDIO_CORE_MISSING_TOOL_COUNT)
if(AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE AND AUDIOPLAYER_AUDIO_CORE_MISSING_TOOL_COUNT GREATER 0)
    string(REPLACE ";" "\n  " AUDIOPLAYER_AUDIO_CORE_MISSING_TEXT "${AUDIOPLAYER_AUDIO_CORE_MISSING_TOOLS}")
    message(FATAL_ERROR
        "AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE is ON, but the self-built FFmpeg audio-core tools are incomplete under "
        "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}:\n  ${AUDIOPLAYER_AUDIO_CORE_MISSING_TEXT}\n"
        "Run scripts/build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild, "
        "or set AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT to a complete self-built runtime root.")
endif()

set(AUDIOPLAYER_LIBAV_DECODER_LIBRARIES)
set(AUDIOPLAYER_LIBAV_DECODER_MISSING)

function(resolve_audio_core_library out_var library_name)
    set(candidates
        "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/lib/${library_name}.lib"
        "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/lib/lib${library_name}.a"
    )
    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

foreach(required_header IN ITEMS
        libavformat/avformat.h
        libavcodec/avcodec.h
        libavutil/avutil.h
        libswresample/swresample.h)
    if(NOT EXISTS "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/include/${required_header}")
        list(APPEND AUDIOPLAYER_LIBAV_DECODER_MISSING
            "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/include/${required_header}")
    endif()
endforeach()

foreach(required_library IN ITEMS avformat avcodec swresample avutil)
    resolve_audio_core_library(resolved_library "${required_library}")
    if(resolved_library)
        list(APPEND AUDIOPLAYER_LIBAV_DECODER_LIBRARIES "${resolved_library}")
    else()
        list(APPEND AUDIOPLAYER_LIBAV_DECODER_MISSING
            "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/lib/${required_library}.lib or ${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/lib/lib${required_library}.a")
    endif()
endforeach()

if(AUDIOPLAYER_LIBAV_DECODER_MISSING)
    if(AUDIOPLAYER_REQUIRE_LIBAV_DECODER)
        string(REPLACE ";" "\n  " AUDIOPLAYER_LIBAV_DECODER_MISSING_TEXT "${AUDIOPLAYER_LIBAV_DECODER_MISSING}")
        message(FATAL_ERROR
            "AUDIOPLAYER_REQUIRE_LIBAV_DECODER is ON, but the self-built FFmpeg audio-core libav decoder is incomplete under "
            "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}:\n  ${AUDIOPLAYER_LIBAV_DECODER_MISSING_TEXT}\n"
            "Run scripts/build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild, "
            "or configure with -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=OFF to allow the ffmpeg CLI fallback.")
    else()
        message(WARNING "In-process libav decoder disabled because the self-built FFmpeg audio-core libav files are incomplete.")
    endif()
else()
    set(AUDIOPLAYER_LIBAV_DECODER_AVAILABLE ON)
    target_compile_definitions(AudioPlayer PRIVATE AUDIOPLAYER_LIBAV_DECODER=1)
    target_include_directories(AudioPlayer PRIVATE "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/include")
    target_link_libraries(AudioPlayer
        PRIVATE
            ${AUDIOPLAYER_LIBAV_DECODER_LIBRARIES}
    )
    if(WIN32)
        target_link_libraries(AudioPlayer PRIVATE bcrypt)
    endif()
    if(MSVC)
        # The slim FFmpeg static libs are built with MSVC's static CRT default.
        # Keep the app on Qt's dynamic CRT and resolve FFmpeg CRT calls there.
        target_link_options(AudioPlayer PRIVATE /NODEFAULTLIB:LIBCMT)
    endif()
    message(STATUS "Using self-built FFmpeg audio-core runtime: ${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}")
endif()

if(WIN32)
    target_sources(AudioPlayer PRIVATE
        src/backends/wasapi/windowswasapiaudioplayer.cpp
        src/backends/wasapi/windowswasapiaudioplayer.h
        src/backends/wasapi/windowswasapiaudioplayer_output.cpp
        src/backends/wasapi/windowswasapiaudioplayer_state.cpp
        src/backends/wasapi/windowswasapiaudioplayer_worker_artifact.cpp
        src/backends/wasapi/windowswasapiaudioplayer_worker_format.cpp
        src/backends/wasapi/windowswasapiaudioplayer_worker_output.cpp
        src/backends/wasapi/windowswasapiaudioplayer_worker_spatial.cpp
        src/backends/wasapi/windowswasapiaudioplayer_worker.h
        src/backends/wasapi/windowswasapiaudioplayer_worker_helpers.h
        src/backends/asio/windowsasioaudioplayer.cpp
        src/backends/asio/windowsasioaudioplayer.h
        src/backends/asio/windowsasioaudioplayer_worker.cpp
        src/backends/asio/windowsasioaudioplayer_worker.h
        src/backends/asio/windowsasioaudioplayer_discovery.cpp
        src/backends/asio/windowsasioaudioplayer_discovery.h
        src/backends/asio/windowsasioaudioplayer_formats.cpp
        src/backends/asio/windowsasioaudioplayer_formats.h
        src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp
        src/backends/asio/windowsasioaudioplayer_sessionprobe.h
        src/backends/asio/windowsasioaudioplayer_utils.cpp
        src/backends/asio/windowsasioaudioplayer_utils.h
        src/backends/asio/asio_interface.h
    )

    target_link_libraries(AudioPlayer
        PRIVATE
            ole32
            uuid
    )

    add_executable(WasapiLoopbackCapture
        tools/wasapi-loopback-capture/main.cpp
    )
    set_target_properties(WasapiLoopbackCapture PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(WasapiLoopbackCapture PRIVATE cxx_std_17)
    target_link_libraries(WasapiLoopbackCapture
        PRIVATE
            avrt
            ole32
            propsys
            uuid
    )

    # Diagnostic-only spatial PCM probe. It deliberately generates synthetic
    # 5.1.2 object-bed PCM so the Windows Dolby/Spatial Audio render route can
    # be verified independently from any compressed-codec decoder.
    add_executable(SpatialBedProbe
        tools/spatial-bed-probe/main.cpp
    )
    set_target_properties(SpatialBedProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(SpatialBedProbe PRIVATE cxx_std_17)
    target_link_libraries(SpatialBedProbe
        PRIVATE
            avrt
            ole32
            propsys
            uuid
    )

    # Diagnostic-only Windows Spatial Audio object lifecycle probe. It uses
    # generated mono PCM and synthetic positions, independent of playback and
    # decoder code, to exercise a 15-object dynamic stream plus static LFE.
    add_executable(SpatialDynamicProbe
        tools/spatial-dynamic-probe/main.cpp
    )
    set_target_properties(SpatialDynamicProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(SpatialDynamicProbe PRIVATE cxx_std_17)
    target_link_libraries(SpatialDynamicProbe
        PRIVATE
            avrt
            ole32
            propsys
            uuid
    )

    # Diagnostic-only first stage of the self-written E-AC-3/JOC decoder. It
    # parses ETSI E-AC-3 syncframes and stream/substream topology without
    # linking FFmpeg or another decoder implementation.
    add_executable(AtmosJocProbe
        tools/atmos-joc-probe/main.cpp
    )
    set_target_properties(AtmosJocProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(AtmosJocProbe PRIVATE cxx_std_17)

    # Gate 8N-1a diagnostic-only native E-AC-3 syntax parser and six-block
    # access-unit assembler.  This target intentionally has no FFmpeg link.
    add_executable(Eac3NativeCoreProbe
        tools/atmos-joc-probe/native-eac3-core-probe.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
    )
    set_target_properties(Eac3NativeCoreProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeCoreProbe PRIVATE cxx_std_17)

    # Gate 8N-1b diagnostic-only bounded E-AC-3 bsi() parser.  It consumes
    # the metadata syntax through the audfrm boundary without FFmpeg.
    add_executable(Eac3NativeBsiProbe
        tools/atmos-joc-probe/native-eac3-bsi-probe.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
    )
    set_target_properties(Eac3NativeBsiProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeBsiProbe PRIVATE cxx_std_17)

    # TS 102 366 H.3.7 diagnostic: bounded headphone-rendering payload
    # header/presence, serialized BRIR reassembly, and parity validation.
    add_executable(Eac3AnnexHHeadphoneProbe
        tools/atmos-joc-probe/annex-h-headphone-probe.cpp
        tools/atmos-joc-probe/annex-h-headphone.cpp
    )
    set_target_properties(Eac3AnnexHHeadphoneProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3AnnexHHeadphoneProbe PRIVATE cxx_std_17)

    # Gate 8N-4a diagnostic-only ordered substream inventory.  It reuses the
    # bounded core/BSI parser and AU boundary state machine; no renderer,
    # mixer, FFmpeg/libav, or new codec syntax is introduced here.
    add_executable(Eac3NativeSubstreamProbe
        tools/atmos-joc-probe/native-eac3-substream-probe.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
    )
    set_target_properties(Eac3NativeSubstreamProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeSubstreamProbe PRIVATE cxx_std_17)

    # Gate 8N-5c: supplied config-4 Legacy AC-3 SID0 plus immediately
    # following dependent E-AC-3 SID0 acceptance.  This native adapter emits
    # stable substream/channel identity and an explicit no-PCM boundary; it
    # has no FFmpeg/libav, renderer, or playback dependency.
    add_executable(Eac3NativeConfig4CoreProbe
        tools/atmos-joc-probe/native-eac3-config4-core-probe.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
    )
    set_target_properties(Eac3NativeConfig4CoreProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4CoreProbe PRIVATE cxx_std_17)

    # Gate 8N-5f: supplied Legacy AC-3 six-block diagnostic PCM.  This probe
    # materializes coefficients and reuses native IMDCT/overlap; it remains
    # diagnostic-only and separate from production playback/assembly.
    add_executable(Eac3NativeLegacyAc3FrontendProbe
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    set_target_properties(Eac3NativeLegacyAc3FrontendProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeLegacyAc3FrontendProbe PRIVATE cxx_std_17)

    # Gate 8N-5g: supplied config-4 dependent E-AC-3 SID0 diagnostic PCM.
    # This adapter reuses the native audblk/coefficient/transform primitives
    # for four stable dependent channels; it does not assemble the base and
    # dependent substreams and has no JOC, renderer, or playback dependency.
    add_executable(Eac3NativeDependentPcmProbe
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    set_target_properties(Eac3NativeDependentPcmProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeDependentPcmProbe PRIVATE cxx_std_17)

    # Gate 8N-5h: probe-local config-4 base/dependent PCM assembly.  The
    # assembly target links the two accepted diagnostic implementations as
    # separate translation units through their probe-local contracts.
    add_executable(Eac3NativeConfig4PcmAssemblyProbe
        tools/atmos-joc-probe/native-eac3-config4-pcm.cpp
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    target_compile_definitions(Eac3NativeConfig4PcmAssemblyProbe PRIVATE
        EAC3_CONFIG4_PCM_COMPOSITION_TU
    )
    set_target_properties(Eac3NativeConfig4PcmAssemblyProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4PcmAssemblyProbe PRIVATE cxx_std_17)

    # J0A5 config-4: map the actual N5J ten-channel owned vectors into the
    # normative seven-slot JOC input plus a separate bypass LFE.  This target
    # stops before the existing config-3-only native session bridge.
    add_executable(Eac3NativeConfig4MappingProbe
        tools/atmos-joc-probe/native-eac3-config4-mapping-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-pcm.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    target_compile_definitions(Eac3NativeConfig4MappingProbe PRIVATE
        EAC3_CONFIG4_PCM_COMPOSITION_TU
        EAC3_CONFIG4_PCM_NO_MAIN
        EAC3_CONFIG4_JOC_BRIDGE_NO_MAIN
    )
    set_target_properties(Eac3NativeConfig4MappingProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4MappingProbe PRIVATE cxx_std_17)

    # J0A6: probe-only config-4 entry into the existing native JOC/OAMD
    # session.  It consumes the linked J0A4 qualification and J0A5 owned
    # mapping contracts; no renderer, playback, DRC, or FFmpeg dependency.
    add_executable(Eac3NativeConfig4JocSessionProbe
        tools/atmos-joc-probe/native-eac3-config4-joc-session-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-mapping-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-pcm.cpp
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-session-bridge.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder-seam.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    target_compile_definitions(Eac3NativeConfig4JocSessionProbe PRIVATE
        EAC3_CONFIG4_PCM_COMPOSITION_TU
        EAC3_CONFIG4_MAPPING_NO_MAIN
        EAC3_CONFIG4_JOC_BRIDGE_NO_MAIN
        EAC3_CONFIG4_PCM_NO_MAIN
    )
    set_target_properties(Eac3NativeConfig4JocSessionProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4JocSessionProbe PRIVATE cxx_std_17)

    # J0A7: probe-only prepared-scene entry.  It retains owned Gate6C PCM
    # while exercising the existing metadata-to-gain-frame contract; no PCM
    # speaker-bus renderer or playback path is entered.
    add_executable(Eac3NativeConfig4SceneProbe
        tools/atmos-render/native-eac3-config4-scene-probe.cpp
        tools/atmos-render/scene-adapter.cpp
        tools/atmos-render/scene-model.cpp
        tools/atmos-render/scene-object-properties.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/bs2127-system-h-configured-panner.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-system-h-cartesian-panner.cpp
        tools/atmos-render/bs2127-triplet.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-session-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-mapping-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-pcm.cpp
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-session-bridge.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder-seam.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    target_compile_definitions(Eac3NativeConfig4SceneProbe PRIVATE
        EAC3_CONFIG4_PCM_COMPOSITION_TU
        EAC3_CONFIG4_JOC_SESSION_NO_MAIN
        EAC3_CONFIG4_MAPPING_NO_MAIN
        EAC3_CONFIG4_JOC_BRIDGE_NO_MAIN
        EAC3_CONFIG4_PCM_NO_MAIN
    )
    set_target_properties(Eac3NativeConfig4SceneProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4SceneProbe PRIVATE cxx_std_17)

    # R2C: diagnostic sample-accurate System H planar mixer over J0A7.
    # LFE remains a separate sideband and bed input is explicitly absent.
    add_executable(Eac3NativeConfig4R2CProbe
        tools/atmos-render/native-eac3-config4-r2c-probe.cpp
        tools/atmos-render/i0p-offline-output.cpp
        tools/atmos-render/brir-convolver.cpp
        tools/atmos-render/radix2-fft.cpp
        tools/atmos-render/sofa-brir-cache.cpp
        tools/atmos-render/system-h-planar-mixer.cpp
        tools/atmos-render/native-eac3-config4-scene-probe.cpp
        tools/atmos-render/scene-adapter.cpp
        tools/atmos-render/scene-model.cpp
        tools/atmos-render/scene-object-properties.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/bs2127-system-h-configured-panner.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-system-h-cartesian-panner.cpp
        tools/atmos-render/bs2127-triplet.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-session-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-mapping-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-config4-pcm.cpp
        tools/atmos-joc-probe/native-eac3-legacy-ac3-frontend-probe.cpp
        tools/atmos-joc-probe/native-eac3-dependent-pcm-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-session-bridge.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder-seam.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    target_compile_definitions(Eac3NativeConfig4R2CProbe PRIVATE
        EAC3_CONFIG4_PCM_COMPOSITION_TU
        EAC3_CONFIG4_JOC_SESSION_NO_MAIN
        EAC3_CONFIG4_MAPPING_NO_MAIN
        EAC3_CONFIG4_JOC_BRIDGE_NO_MAIN
        EAC3_CONFIG4_PCM_NO_MAIN
        EAC3_CONFIG4_SCENE_NO_MAIN
    )
    set_target_properties(Eac3NativeConfig4R2CProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4R2CProbe PRIVATE cxx_std_17)

    # Gate 8N-5a bounded native ordinary E-AC-3 core-decoder adapter.  It
    # accepts only the supplied config-3 type-0 SID0 topology and emits a
    # renderer-neutral AU plus a separate EOS tail; no JOC or playback link.
    add_executable(Eac3NativeCoreDecoderProbe
        tools/atmos-joc-probe/native-eac3-core-decoder-probe.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    set_target_properties(Eac3NativeCoreDecoderProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeCoreDecoderProbe PRIVATE cxx_std_17)

    # Gate 8N-5b: adapt the bounded ordinary E-AC-3 native decoder to the
    # existing callback seam without labeling ordinary audio as JOC.
    add_executable(Eac3NativeCoreDecoderSeamProbe
        tools/atmos-joc-probe/native-eac3-core-decoder-seam-probe.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder-seam.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
    )
    set_target_properties(Eac3NativeCoreDecoderSeamProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeCoreDecoderSeamProbe PRIVATE cxx_std_17)

    # Gate J0A1: native FFmpeg-free config-3 EMDF raw-payload extraction.
    add_executable(Eac3NativeEmdfProbe
        tools/atmos-joc-probe/native-eac3-emdf-probe.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
    )
    set_target_properties(Eac3NativeEmdfProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeEmdfProbe PRIVATE cxx_std_17)

    # Gate J0A2: native FFmpeg-free qualification using the existing Gate 5A
    # JOC and Gate 6B1 OAMD parsers.  It never qualifies unsupported configs.
    add_executable(Eac3NativeJocQualifierProbe
        tools/atmos-joc-probe/native-eac3-joc-qualifier-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
    )
    set_target_properties(Eac3NativeJocQualifierProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeJocQualifierProbe PRIVATE cxx_std_17)

    # Gate 8N-1c/N0 diagnostic-only bounded audfrm/audblk feature inventory.
    # It has no FFmpeg/libav or playback dependency and stops structurally at
    # active advanced coding tools.
    add_executable(Eac3NativeAudblkProbe
        tools/atmos-joc-probe/native-eac3-audblk-probe.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
    )
    set_target_properties(Eac3NativeAudblkProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeAudblkProbe PRIVATE cxx_std_17)

    # Gate 8N-1d/N1A state-only frame/block/channel snapshots and digest
    # probe.  This target has no FFmpeg/libav, PCM, IMDCT, or playback path.
    add_executable(Eac3NativeBlockStateProbe
        tools/atmos-joc-probe/native-eac3-block-state-probe.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
    )
    set_target_properties(Eac3NativeBlockStateProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeBlockStateProbe PRIVATE cxx_std_17)

    # Gate 8N-2a diagnostic-only native exponent strategy/decode primitives.
    # This target has no FFmpeg or Qt dependency and stops before bit
    # allocation, mantissas, coupling coordinates, and IMDCT.
    add_executable(Eac3NativeExponentsProbe
        tools/atmos-joc-probe/native-eac3-exponents-probe.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
    )
    set_target_properties(Eac3NativeExponentsProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeExponentsProbe PRIVATE cxx_std_17)

    # Gate 8N-2b-1 diagnostic-only native parametric bit allocation for one
    # uncoupled FBW channel.  This target has no FFmpeg or Qt dependency and
    # explicitly stops before mantissas and unsupported advanced tools.
    add_executable(Eac3NativeBitAllocationProbe
        tools/atmos-joc-probe/native-eac3-bit-allocation-probe.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
    )
    set_target_properties(Eac3NativeBitAllocationProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeBitAllocationProbe PRIVATE cxx_std_17)

    # Gate 8N-2b-3 diagnostic-only mantissa degroup/dequantization primitive.
    # Injected dither is used for bap=0; no FFmpeg or Qt dependency is linked.
    add_executable(Eac3NativeMantissasProbe
        tools/atmos-joc-probe/native-eac3-mantissas-probe.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
    )
    set_target_properties(Eac3NativeMantissasProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeMantissasProbe PRIVATE cxx_std_17)

    # Gate 8N-2b/N2A1 renderer-neutral spectral coefficient snapshots.  This
    # reuses the audfrm/audblk cursor and mantissa primitive, with no FFmpeg,
    # IMDCT, PCM, JOC, renderer, playback, or DRC application path.
    add_executable(Eac3NativeCoefficientProbe
        tools/atmos-joc-probe/native-eac3-coefficient-probe.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
    )
    set_target_properties(Eac3NativeCoefficientProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeCoefficientProbe PRIVATE cxx_std_17)

    # Gate 8N-3a standalone scalar reference IMDCT/TDAC transform.  It uses
    # the ETSI transform/window equations without connecting audblk samples,
    # playback, or any FFmpeg/libav implementation.
    add_executable(Eac3NativeTransformProbe
        tools/atmos-joc-probe/native-eac3-transform-probe.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
    )
    set_target_properties(Eac3NativeTransformProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeTransformProbe PRIVATE cxx_std_17)

    # Gate 8N-3b/N3B diagnostic PCM session.  This connects one audblk
    # coefficient parse to independent per-channel scalar transform state;
    # it has no playback, renderer, FFmpeg/libav, JOC, or DRC application.
    add_executable(Eac3NativePcmSessionProbe
        tools/atmos-joc-probe/native-eac3-pcm-session-probe.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
    )
    set_target_properties(Eac3NativePcmSessionProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativePcmSessionProbe PRIVATE cxx_std_17)

    # Gate 8B-1 diagnostic: renderer-neutral scene/timeline contract.  This
    # target deliberately stops before panning, convolution, and audio I/O.
    add_executable(Eac3SceneModelProbe
        tools/atmos-render/scene-model-probe.cpp
        tools/atmos-render/scene-model.cpp
    )
    set_target_properties(Eac3SceneModelProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneModelProbe PRIVATE cxx_std_17)

    # Gate 8B-2 diagnostic: dependency-free equal-power stereo panner and
    # gain-ramp math.  It is not the formal BS.2051/BS.2127/SOFA renderer.
    add_executable(Eac3StereoDiagnosticProbe
        tools/atmos-render/stereo-diagnostic-probe.cpp
        tools/atmos-render/stereo-diagnostic.cpp
    )
    set_target_properties(Eac3StereoDiagnosticProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3StereoDiagnosticProbe PRIVATE cxx_std_17)

    # Gate 8C-1 diagnostic: fixed BS.2051 System H catalog and offline BBC
    # SOFA emitter-position comparison.  No panning or HDF5 runtime is used.
    add_executable(Eac3Bs2051SystemHProbe
        tools/atmos-render/bs2051-layout-probe.cpp
        tools/atmos-render/bs2051-layout.cpp
    )
    set_target_properties(Eac3Bs2051SystemHProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2051SystemHProbe PRIVATE cxx_std_17)

    # Gate 8C-2 diagnostic: one BS.2127 §6.1.1 triplet gain solve primitive.
    # This target does not select triplets or implement a complete renderer.
    add_executable(Eac3Bs2127TripletProbe
        tools/atmos-render/bs2127-triplet-probe.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127TripletProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127TripletProbe PRIVATE cxx_std_17)

    # Gate 8C-3a bounded diagnostic: System H nominal convex-hull topology and
    # first-valid Triplet/Quad selection. VirtualHullFacet is a separate region.
    add_executable(Eac3Bs2127SelectorProbe
        tools/atmos-render/bs2127-selector-probe.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127SelectorProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127SelectorProbe PRIVATE cxx_std_17)

    # Gate 8C-3b bounded diagnostic: aggregate the lower virtual hull facets
    # into one BS.2127 VirtualNgon.
    add_executable(Eac3Bs2127VirtualNgonProbe
        tools/atmos-render/bs2127-virtual-ngon-probe.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127VirtualNgonProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127VirtualNgonProbe PRIVATE cxx_std_17)

    # R0B bounded diagnostic: one deterministic first-region-valid dispatcher
    # over System H identity, Triplet, Quad, and lower VirtualNgon regions.
    add_executable(Eac3Bs2127PointSourcePannerProbe
        tools/atmos-render/bs2127-point-source-panner-probe.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127PointSourcePannerProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127PointSourcePannerProbe PRIVATE cxx_std_17)

    # R0C bounded diagnostic: BS.2127-1 §6.1.3 nominal-to-real System H
    # configuration process.  This is renderer-neutral and label keyed; it
    # deliberately does not claim a generic layout loader.
    add_executable(Eac3Bs2127NominalToActualProbe
        tools/atmos-render/bs2127-nominal-to-actual-probe.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127NominalToActualProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127NominalToActualProbe PRIVATE cxx_std_17)

    # R0D bounded configured System H topology: the five direct-downmix
    # lower virtual speakers from BS.2127-1 §6.1.3.1.1.
    add_executable(Eac3Bs2127SystemHConfiguredPannerProbe
        tools/atmos-render/bs2127-system-h-configured-panner-probe.cpp
        tools/atmos-render/bs2127-system-h-configured-panner.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3Bs2127SystemHConfiguredPannerProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3Bs2127SystemHConfiguredPannerProbe PRIVATE cxx_std_17)

    # R1A bounded renderer-neutral scene adapter: map bed/object scene updates
    # to configured System H directional or allocentric Cartesian gains. No
    # PCM or sink is used.
    add_executable(Eac3SceneAdapterProbe
        tools/atmos-render/scene-adapter-probe.cpp
        tools/atmos-render/scene-adapter.cpp
        tools/atmos-render/scene-model.cpp
        tools/atmos-render/bs2127-system-h-cartesian-panner.cpp
        tools/atmos-render/bs2127-system-h-configured-panner.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3SceneAdapterProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneAdapterProbe PRIVATE cxx_std_17)

    # R1B1 bounded decoder-to-scene object-property contract.  It records
    # OAMD position/size/gain semantics and accepts only explicit R1A point
    # directions; non-zero extent remains typed pending without extent math.
    add_executable(Eac3SceneObjectPropertiesProbe
        tools/atmos-render/scene-object-properties-probe.cpp
        tools/atmos-render/scene-object-properties.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/scene-object-ramp-scheduler.cpp
    )
    set_target_properties(Eac3SceneObjectPropertiesProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneObjectPropertiesProbe PRIVATE cxx_std_17)

    add_executable(Eac3SceneObjectRampSchedulerProbe
        tools/atmos-render/scene-object-ramp-scheduler-probe.cpp
        tools/atmos-render/scene-object-ramp-scheduler.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/scene-object-properties.cpp
    )
    set_target_properties(Eac3SceneObjectRampSchedulerProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneObjectRampSchedulerProbe PRIVATE cxx_std_17)

    add_executable(Eac3SceneObjectStreamGrouperProbe
        tools/atmos-render/scene-object-stream-grouper-probe.cpp
        tools/atmos-render/scene-object-stream-grouper.cpp
        tools/atmos-render/scene-object-ramp-scheduler.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/scene-object-properties.cpp
    )
    set_target_properties(Eac3SceneObjectStreamGrouperProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneObjectStreamGrouperProbe PRIVATE cxx_std_17)

    add_executable(Eac3SceneObjectGainFrameProbe
        tools/atmos-render/scene-object-gain-frame-probe.cpp
        tools/atmos-render/scene-object-gain-frame.cpp
        tools/atmos-render/scene-object-stream-grouper.cpp
        tools/atmos-render/scene-object-ramp-scheduler.cpp
        tools/atmos-render/scene-object-bridge.cpp
        tools/atmos-render/scene-object-properties.cpp
        tools/atmos-render/bs2127-system-h-cartesian-panner.cpp
        tools/atmos-render/bs2127-system-h-configured-panner.cpp
        tools/atmos-render/bs2127-nominal-to-actual.cpp
        tools/atmos-render/bs2127-point-source-panner.cpp
        tools/atmos-render/bs2127-virtual-ngon.cpp
        tools/atmos-render/bs2127-selector.cpp
        tools/atmos-render/bs2127-quad.cpp
        tools/atmos-render/bs2051-layout.cpp
        tools/atmos-render/bs2127-triplet.cpp
    )
    set_target_properties(Eac3SceneObjectGainFrameProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SceneObjectGainFrameProbe PRIVATE cxx_std_17)

    add_executable(Eac3SofaBrirCacheProbe
        tools/atmos-render/sofa-brir-cache-probe.cpp
        tools/atmos-render/sofa-brir-cache.cpp
    )
    set_target_properties(Eac3SofaBrirCacheProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SofaBrirCacheProbe PRIVATE cxx_std_17)

    add_executable(Eac3BrirConvolverProbe
        tools/atmos-render/brir-convolver-probe.cpp
        tools/atmos-render/brir-convolver.cpp
        tools/atmos-render/radix2-fft.cpp
        tools/atmos-render/sofa-brir-cache.cpp
    )
    set_target_properties(Eac3BrirConvolverProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3BrirConvolverProbe PRIVATE cxx_std_17)

    # Bounded offline adapter: consume a validated 22-channel System H f32
    # bus and apply the checked-in BBC BRIR cache. ADM authoring/rendering is
    # intentionally kept in the separate EAR wrapper script.
    add_executable(Eac3SystemHBrirOffline
        tools/atmos-render/system-h-brir-offline.cpp
        tools/atmos-render/brir-convolver.cpp
        tools/atmos-render/radix2-fft.cpp
        tools/atmos-render/sofa-brir-cache.cpp
    )
    set_target_properties(Eac3SystemHBrirOffline PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SystemHBrirOffline PRIVATE cxx_std_17)

    # Gate 4 diagnostic: standalone 64-band/640-tap QMF analysis and
    # synthesis. The normative coefficient table is supplied at runtime.
    add_executable(Eac3QmfProbe
        tools/atmos-joc-probe/qmf-probe.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    set_target_properties(Eac3QmfProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3QmfProbe PRIVATE cxx_std_17)

    # Gate 5C Phase 1 diagnostic: synthetic object-QMF reconstruction only.
    add_executable(Eac3JocQmfProbe
        tools/atmos-joc-probe/joc-qmf-probe.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    set_target_properties(Eac3JocQmfProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3JocQmfProbe PRIVATE cxx_std_17)

    # Gate 6A diagnostic: object-QMF synthesis only.
    add_executable(Eac3JocSynthesisProbe
        tools/atmos-joc-probe/joc-synthesis-probe.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    set_target_properties(Eac3JocSynthesisProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3JocSynthesisProbe PRIVATE cxx_std_17)

    # Gate 6B1 diagnostic: bounded OAMD framing and element inventory only.
    add_executable(Eac3OamdB1Probe
        tools/atmos-joc-probe/oamd-b1-probe.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
    )
    set_target_properties(Eac3OamdB1Probe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3OamdB1Probe PRIVATE cxx_std_17)

    # Gate 6B2A diagnostic: raw object-update syntax inside ID-1 elements.
    add_executable(Eac3OamdB2aProbe
        tools/atmos-joc-probe/oamd-b2a-probe.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
    )
    set_target_properties(Eac3OamdB2aProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3OamdB2aProbe PRIVATE cxx_std_17)

    # Gate 6B2B diagnostic: transactional object-property state application.
    add_executable(Eac3OamdB2bProbe
        tools/atmos-joc-probe/oamd-b2b-probe.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
    )
    set_target_properties(Eac3OamdB2bProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3OamdB2bProbe PRIVATE cxx_std_17)

    # Gate 6B2C diagnostic: normative trim/divergence/high-precision OAMD
    # element parser and transactional B2B materialization.
    add_executable(Eac3OamdAdditionalProbe
        tools/atmos-joc-probe/oamd-additional-probe.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
    )
    set_target_properties(Eac3OamdAdditionalProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3OamdAdditionalProbe PRIVATE cxx_std_17)

    # Bundle metadata contract probe.  It verifies renderer-neutral JSON
    # carriage while keeping the version-2 PCM binary layout unchanged.
    add_executable(Eac3BearExportProbe
        tools/atmos-joc-probe/bear-export-probe.cpp
        tools/atmos-joc-probe/bear-export.cpp
    )
    set_target_properties(Eac3BearExportProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3BearExportProbe PRIVATE cxx_std_17)
    add_test(NAME Eac3BearExportProbe
        COMMAND Eac3BearExportProbe --self-test)

    # Gate 6C diagnostic: renderer-neutral object/LFE/OAMD timeline assembly.
    add_executable(Eac3JocGate6cProbe
        tools/atmos-joc-probe/joc-gate6c-probe.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    set_target_properties(Eac3JocGate6cProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3JocGate6cProbe PRIVATE cxx_std_17)

    # Gate 8A-1 diagnostic: injectable core-decoder contract and reusable
    # Gate 5B -> QMF -> Gate 6C session seam.  No libav or playback dependency.
    add_executable(Eac3JocSessionProbe
        tools/atmos-joc-probe/joc-session-probe.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
    )
    set_target_properties(Eac3JocSessionProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3JocSessionProbe PRIVATE cxx_std_17)

    # Gate J0A3: bounded config-3 bridge joining native core frames and the
    # qualified JOC/OAMD session.  This remains renderer- and FFmpeg-free.
    add_executable(Eac3NativeJocSessionBridgeProbe
        tools/atmos-joc-probe/native-eac3-joc-session-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-session-bridge.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder-seam.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-core-decoder.cpp
        tools/atmos-joc-probe/native-eac3-pcm-session.cpp
        tools/atmos-joc-probe/native-eac3-audblk.cpp
        tools/atmos-joc-probe/native-eac3-block-state.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/native-eac3-exponents.cpp
        tools/atmos-joc-probe/native-eac3-bit-allocation.cpp
        tools/atmos-joc-probe/native-eac3-mantissas.cpp
        tools/atmos-joc-probe/native-eac3-transform.cpp
        tools/atmos-joc-probe/joc-session.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/joc-gate5b.cpp
        tools/atmos-joc-probe/joc-qmf.cpp
        tools/atmos-joc-probe/joc-gate6c.cpp
        tools/atmos-joc-probe/joc-synthesis.cpp
        tools/atmos-joc-probe/qmf-bank.cpp
    )
    set_target_properties(Eac3NativeJocSessionBridgeProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeJocSessionBridgeProbe PRIVATE cxx_std_17)

    # J0A4 config-4: probe-only base/dependent EMDF and JOC qualification
    # association. It deliberately stops before the config-3-only core/session
    # contract and reports an explicit blocked result.
    add_executable(Eac3NativeConfig4JocBridgeProbe
        tools/atmos-joc-probe/native-eac3-config4-joc-bridge-probe.cpp
        tools/atmos-joc-probe/native-eac3-joc-qualifier.cpp
        tools/atmos-joc-probe/native-eac3-emdf.cpp
        tools/atmos-joc-probe/native-eac3-bsi.cpp
        tools/atmos-joc-probe/native-eac3-core.cpp
        tools/atmos-joc-probe/joc-gate5a.cpp
        tools/atmos-joc-probe/oamd-b1.cpp
    )
    set_target_properties(Eac3NativeConfig4JocBridgeProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3NativeConfig4JocBridgeProbe PRIVATE cxx_std_17)

    # Gate 7B diagnostic: renderer-neutral OAMD-to-Windows property and ramp
    # adapter.  It deliberately has no Windows Spatial Audio dependency.
    add_executable(Eac3SpatialPropertyProbe
        tools/atmos-joc-probe/spatial-property-adapter-probe.cpp
        tools/atmos-joc-probe/spatial-property-adapter.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
    )
    set_target_properties(Eac3SpatialPropertyProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SpatialPropertyProbe PRIVATE cxx_std_17)

    # Gate 7C1 diagnostic: bounded renderer-neutral SPSC bridge and
    # render-quantum scheduler.  No Windows or renderer API dependency.
    add_executable(Eac3SpatialBridgeCoreProbe
        tools/atmos-joc-probe/spatial-bridge-core-probe.cpp
        tools/atmos-joc-probe/spatial-bridge-core.cpp
        tools/atmos-joc-probe/spatial-property-adapter.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
    )
    set_target_properties(Eac3SpatialBridgeCoreProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SpatialBridgeCoreProbe PRIVATE cxx_std_17)

    # Gate 7C2 diagnostic: Windows consumer runner.  The endpoint-free probe
    # exercises option, buffer-shape, outcome-text, and terminal contracts.
    add_executable(Eac3SpatialBridgeRendererProbe
        tools/atmos-joc-probe/spatial-bridge-renderer-probe.cpp
        tools/atmos-joc-probe/spatial-bridge-renderer.cpp
        tools/atmos-joc-probe/spatial-bridge-core.cpp
        tools/atmos-joc-probe/spatial-property-adapter.cpp
        tools/atmos-joc-probe/oamd-b2b.cpp
        tools/atmos-joc-probe/oamd-additional.cpp
        tools/atmos-joc-probe/oamd-b2a.cpp
    )
    set_target_properties(Eac3SpatialBridgeRendererProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(Eac3SpatialBridgeRendererProbe PRIVATE cxx_std_17)
    target_link_libraries(Eac3SpatialBridgeRendererProbe PRIVATE
        avrt
        ole32
        propsys
        uuid
    )

    # Gate 0 diagnostic: correlate compressed E-AC-3 packet topology with
    # native libavcodec AVFrame output. It does not touch production playback.
    if(AUDIOPLAYER_LIBAV_DECODER_AVAILABLE)
        add_executable(Eac3Gate0Probe
            tools/atmos-joc-probe/gate0.cpp
        )
        set_target_properties(Eac3Gate0Probe PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
        )
        target_compile_features(Eac3Gate0Probe PRIVATE cxx_std_17)
        target_compile_definitions(Eac3Gate0Probe PRIVATE AUDIOPLAYER_LIBAV_DECODER=1)
        target_include_directories(Eac3Gate0Probe PRIVATE
            "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/include"
        )
        target_link_libraries(Eac3Gate0Probe PRIVATE
            ${AUDIOPLAYER_LIBAV_DECODER_LIBRARIES}
        )
        if(WIN32)
            target_link_libraries(Eac3Gate0Probe PRIVATE bcrypt)
        endif()
        if(MSVC)
            target_link_options(Eac3Gate0Probe PRIVATE /NODEFAULTLIB:LIBCMT)
        endif()

        # Gate 1 diagnostic: assemble complete E-AC-3 access units, including
        # AC3_CONVERT core frames, without touching production playback.
        add_executable(Eac3AccessUnitProbe
            tools/atmos-joc-probe/access-unit.cpp
            tools/atmos-joc-probe/bear-export.cpp
            tools/atmos-joc-probe/annex-h-headphone.cpp
            tools/atmos-joc-probe/joc-gate5a.cpp
            tools/atmos-joc-probe/joc-gate5b.cpp
            tools/atmos-joc-probe/joc-matrix-trace.cpp
            tools/atmos-joc-probe/joc-qmf.cpp
            tools/atmos-joc-probe/oamd-b1.cpp
            tools/atmos-joc-probe/oamd-b2a.cpp
            tools/atmos-joc-probe/oamd-b2b.cpp
            tools/atmos-joc-probe/oamd-additional.cpp
            tools/atmos-joc-probe/joc-gate6c.cpp
            tools/atmos-joc-probe/joc-synthesis.cpp
            tools/atmos-joc-probe/qmf-bank.cpp
            tools/atmos-joc-probe/joc-session.cpp
            tools/atmos-joc-probe/spatial-property-adapter.cpp
            tools/atmos-joc-probe/spatial-bridge-renderer.cpp
            tools/atmos-joc-probe/spatial-bridge-core.cpp
        )
        set_target_properties(Eac3AccessUnitProbe PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
        )
        target_compile_features(Eac3AccessUnitProbe PRIVATE cxx_std_17)
        target_compile_definitions(Eac3AccessUnitProbe PRIVATE AUDIOPLAYER_LIBAV_DECODER=1)
        target_include_directories(Eac3AccessUnitProbe PRIVATE
            "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/include"
        )
        target_link_libraries(Eac3AccessUnitProbe PRIVATE
            ${AUDIOPLAYER_LIBAV_DECODER_LIBRARIES}
        )
        if(WIN32)
            target_link_libraries(Eac3AccessUnitProbe PRIVATE
                bcrypt
                avrt
                ole32
                propsys
                uuid
            )
        endif()
        if(MSVC)
            target_link_options(Eac3AccessUnitProbe PRIVATE /NODEFAULTLIB:LIBCMT)
        endif()
    endif()

    # Diagnostic-only Media Foundation probe. It deliberately has no Qt or
    # playback-backend dependency: it verifies whether the MKA sidecar made
    # from a raw Dolby stream is accepted by the Windows MF source reader
    # before any production routing is considered.
    add_executable(MfAtmosProbe
        tools/mf-atmos-probe/main.cpp
    )
    set_target_properties(MfAtmosProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(MfAtmosProbe PRIVATE cxx_std_17)
    target_link_libraries(MfAtmosProbe
        PRIVATE
            mfplat
            mfplay
            mfreadwrite
            mfuuid
            ole32
            propsys
            uuid
    )

    # Diagnostic-only modern Media Foundation engine probe. This is the
    # native engine used by the modern MediaPlayer stack and intentionally
    # remains separate from the production Qt playback route.
    add_executable(MfMediaEngineProbe
        tools/mf-media-engine-probe/main.cpp
    )
    set_target_properties(MfMediaEngineProbe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_compile_features(MfMediaEngineProbe PRIVATE cxx_std_17)
    target_link_libraries(MfMediaEngineProbe
        PRIVATE
            mfplat
            mfuuid
            ole32
            propsys
            shlwapi
            uuid
    )
endif()

if(AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE)
    string(REPLACE "." "[.]" audio_core_required_version_regex
        "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_VERSION}")
    foreach(audio_core_tool_name ffmpeg ffprobe)
        set(audio_core_tool_path "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/bin/${audio_core_tool_name}.exe")
        execute_process(
            COMMAND "${audio_core_tool_path}" -version
            OUTPUT_VARIABLE audio_core_version_output
            ERROR_VARIABLE audio_core_version_output
            RESULT_VARIABLE audio_core_version_result
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT audio_core_version_result EQUAL 0 OR
           NOT audio_core_version_output MATCHES "(ffmpeg|ffprobe)[ \\t]+version[ \\t]+n?${audio_core_required_version_regex}([ \\t-]|$)")
            message(FATAL_ERROR
                "Bundled self-built ${audio_core_tool_name}.exe must report FFmpeg ${AUDIOPLAYER_FFMPEG_AUDIO_CORE_VERSION}; "
                "got: ${audio_core_version_output}")
        endif()
    endforeach()
endif()

target_compile_definitions(AudioPlayer
    PRIVATE
        AUDIOPLAYER_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

if(MSVC)
    set(audio_player_release_like_config
        "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>")
    if(AUDIOPLAYER_ENABLE_MSVC_RELEASE_LTCG)
        target_compile_options(AudioPlayer PRIVATE
            "$<${audio_player_release_like_config}:/GL>"
        )
        target_link_options(AudioPlayer PRIVATE
            "$<${audio_player_release_like_config}:/LTCG>"
            "$<${audio_player_release_like_config}:/OPT:REF>"
            "$<${audio_player_release_like_config}:/OPT:ICF>"
        )
    endif()
    if(AUDIOPLAYER_ENABLE_AVX2)
        target_compile_options(AudioPlayer PRIVATE /arch:AVX2)
    endif()
endif()

if(WIN32)
    function(resolve_deploy_tool out_var configured_value fallback_name)
        if("${configured_value}" STREQUAL "")
            set(candidate_tool "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/bin/${fallback_name}.exe")
            if(EXISTS "${candidate_tool}")
                set(resolved_tool "${candidate_tool}")
            else()
                set(resolved_tool "")
            endif()
        elseif("${configured_value}" STREQUAL "OFF" OR "${configured_value}" STREQUAL "NONE")
            set(resolved_tool "")
        else()
            set(resolved_tool "${configured_value}")
        endif()

        set(${out_var} "${resolved_tool}" PARENT_SCOPE)
    endfunction()

    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    resolve_deploy_tool(FFMPEG_EXECUTABLE "${AUDIOPLAYER_DEPLOY_FFMPEG_EXECUTABLE}" ffmpeg)
    resolve_deploy_tool(FFPROBE_EXECUTABLE "${AUDIOPLAYER_DEPLOY_FFPROBE_EXECUTABLE}" ffprobe)
    if(AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE)
        foreach(audio_core_tool_name ffmpeg ffprobe)
            string(TOUPPER "${audio_core_tool_name}" audio_core_tool_var_prefix)
            set(audio_core_tool_path "${${audio_core_tool_var_prefix}_EXECUTABLE}")
            set(audio_core_expected_tool "${AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT}/bin/${audio_core_tool_name}.exe")
            if(NOT EXISTS "${audio_core_tool_path}")
                message(FATAL_ERROR "Required bundled ${audio_core_tool_name}.exe is missing: ${audio_core_tool_path}")
            endif()
            get_filename_component(audio_core_tool_path_real "${audio_core_tool_path}" REALPATH)
            get_filename_component(audio_core_expected_tool_real "${audio_core_expected_tool}" REALPATH)
            if(NOT audio_core_tool_path_real STREQUAL audio_core_expected_tool_real)
                message(FATAL_ERROR
                    "Default builds must bundle the self-built audio-core ${audio_core_tool_name}.exe from "
                    "${audio_core_expected_tool}; got ${audio_core_tool_path}.")
            endif()
        endforeach()
    endif()
    set(EFFECTIVE_PRUNE_QT_FFMPEG_RUNTIME "${AUDIOPLAYER_PRUNE_QT_FFMPEG_RUNTIME}")

    if(FFMPEG_EXECUTABLE)
        add_custom_command(TARGET AudioPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_EXECUTABLE}"
                "$<TARGET_FILE_DIR:AudioPlayer>/ffmpeg.exe"
            COMMENT "Copying bundled ffmpeg next to AudioPlayer"
            VERBATIM
        )
    endif()

    if(FFPROBE_EXECUTABLE)
        add_custom_command(TARGET AudioPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFPROBE_EXECUTABLE}"
                "$<TARGET_FILE_DIR:AudioPlayer>/ffprobe.exe"
            COMMENT "Copying bundled ffprobe next to AudioPlayer"
            VERBATIM
        )
    endif()

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET AudioPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DWINDEPLOYQT_EXECUTABLE=${WINDEPLOYQT_EXECUTABLE}
                -DFFMPEG_EXECUTABLE=${FFMPEG_EXECUTABLE}
                -DFFPROBE_EXECUTABLE=${FFPROBE_EXECUTABLE}
                -DFFMPEG_DEPLOY_NAME=ffmpeg.exe
                -DFFPROBE_DEPLOY_NAME=ffprobe.exe
                -DPRUNE_QT_FFMPEG_RUNTIME=${EFFECTIVE_PRUNE_QT_FFMPEG_RUNTIME}
                -DSOURCE_EXE=$<TARGET_FILE:AudioPlayer>
                -DPLAYABLE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/playable/$<CONFIG>
                -DCONFIG_NAME=$<CONFIG>
                -DDEBUG_FLAG=$<$<CONFIG:Debug>:--debug>
                -DRELEASE_FLAG=$<$<NOT:$<CONFIG:Debug>>:--release>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/deploy_playable.cmake
            COMMENT "Deploying runnable AudioPlayer bundle"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endif()

install(TARGETS AudioPlayer
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET AudioPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# 单元测试
enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(AudioPlayerTests
    tests/test_main.cpp
    tests/test_pcmstreamformat.cpp
    tests/test_pcmutils.cpp
    tests/test_audioutils.cpp
    tests/test_volumecontrol.cpp
    tests/test_audiobuffer.cpp
    tests/test_audioplayerfactory.cpp
    tests/test_wasapi_states.cpp
    tests/test_alsa_logic.cpp
    tests/test_asioformats.cpp
    tests/test_integration.cpp
    src/backends/ffmpeg/ffmpegpcmshared.cpp
    src/core/audioplayerbackend.cpp
    src/diagnostics/playerlogger.cpp
    src/backends/alsa/alsalogic.cpp
    src/backends/asio/windowsasioaudioplayer_formats.cpp
)

set_target_properties(AudioPlayerTests PROPERTIES AUTOMOC ON)

target_include_directories(AudioPlayerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/backends/shared
    ${CMAKE_SOURCE_DIR}/src/backends/ffmpeg
    ${CMAKE_SOURCE_DIR}/src/backends/alsa
    ${CMAKE_SOURCE_DIR}/src/backends/asio
    ${CMAKE_SOURCE_DIR}/src/diagnostics
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(AudioPlayerTests PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    Qt6::Multimedia
)

target_compile_definitions(AudioPlayerTests PRIVATE
    AUDIOPLAYER_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME AudioPlayerTests COMMAND AudioPlayerTests)

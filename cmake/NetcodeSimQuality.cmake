include_guard(GLOBAL)

option(NETCODESIM_ENABLE_WARNINGS "Enable compiler warnings for project targets" ON)
option(NETCODESIM_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(NETCODESIM_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(NETCODESIM_CLANG_TIDY_WARNINGS_AS_ERRORS "Treat clang-tidy warnings as errors" OFF)
option(NETCODESIM_ENABLE_SANITIZERS "Build project targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NETCODESIM_ENABLE_CLANG_TIDY)
    find_program(NETCODESIM_CLANG_TIDY_EXE NAMES clang-tidy)
    if(NOT NETCODESIM_CLANG_TIDY_EXE)
        message(FATAL_ERROR "NETCODESIM_ENABLE_CLANG_TIDY requires clang-tidy on PATH")
    endif()

    set(NETCODESIM_CLANG_TIDY_COMMAND "${NETCODESIM_CLANG_TIDY_EXE}")
    list(APPEND NETCODESIM_CLANG_TIDY_COMMAND "--use-color")

    if(NETCODESIM_CLANG_TIDY_WARNINGS_AS_ERRORS)
        list(APPEND NETCODESIM_CLANG_TIDY_COMMAND "--warnings-as-errors=*")
    endif()

    set(CMAKE_CXX_CLANG_TIDY "${NETCODESIM_CLANG_TIDY_COMMAND}")
endif()

add_library(NetcodeSimProjectWarnings INTERFACE)

if(NETCODESIM_ENABLE_WARNINGS)
    target_compile_options(NetcodeSimProjectWarnings
        INTERFACE
            $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
    )

    if(NETCODESIM_WARNINGS_AS_ERRORS)
        target_compile_options(NetcodeSimProjectWarnings
            INTERFACE
                $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror>
                $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>
        )
    endif()
endif()

add_library(NetcodeSimProjectOptions INTERFACE)

if(NETCODESIM_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "NETCODESIM_ENABLE_SANITIZERS is only supported with Clang, AppleClang, or GCC")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(NetcodeSimProjectOptions
            INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
        target_link_options(NetcodeSimProjectOptions
            INTERFACE
                -fsanitize=address,undefined
        )
    else()
        message(FATAL_ERROR "NETCODESIM_ENABLE_SANITIZERS is not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

function(netcodesim_apply_project_quality target_name)
    target_link_libraries(${target_name}
        PRIVATE
            NetcodeSimProjectOptions
            NetcodeSimProjectWarnings
    )
endfunction()

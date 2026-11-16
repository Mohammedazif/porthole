# Warning flags for Porthole's own targets (third-party code keeps its own flags).
function(porthole_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(PORTHOLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wshadow -Wformat=2 -Wundef
            -Wstrict-prototypes -Wmissing-prototypes)
        if(PORTHOLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

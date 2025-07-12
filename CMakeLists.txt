cmake_minimum_required(VERSION 3.11)

project(KillSwitch)

add_prx_module(${PROJECT_NAME}
    killswitch.c
    exports.exp
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    # If the debug configuration pass the DEBUG define to the compiler
    $<$<CONFIG:Debug>:DEBUG>
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pspdebug
    pspdisplay
    psppower
    pspctrl
    pspge
)

project(KillSwitchHold)

add_prx_module(${PROJECT_NAME}
    killswitch_hold.c
    exports.exp
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    # If the debug configuration pass the DEBUG define to the compiler
    $<$<CONFIG:Debug>:DEBUG>
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    psppower
    pspctrl
    pspge
)

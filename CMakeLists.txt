cmake_minimum_required(VERSION 3.11)

project(KillSwitch)

add_prx_module(${PROJECT_NAME} killswitch.c)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pspdebug
    pspdisplay
    psppower
    pspctrl
    pspge
)

project(KillSwitchHold)

add_prx_module(${PROJECT_NAME} killswitch_hold.c)

target_link_libraries(${PROJECT_NAME} PRIVATE
    psppower
    pspctrl
    pspge
)
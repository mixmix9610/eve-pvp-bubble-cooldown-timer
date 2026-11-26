cmake_minimum_required(VERSION 3.10)
project(CooldownTimerCpp LANGUAGES CXX)

find_package(OpenCV REQUIRED)

add_executable(cooldown_timer_cpp cooldown_timer.cpp)

target_include_directories(cooldown_timer_cpp PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cooldown_timer_cpp PRIVATE ${OpenCV_LIBS} user32 gdi32 gdiplus shell32)
set_target_properties(cooldown_timer_cpp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)

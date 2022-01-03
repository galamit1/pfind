cmake_minimum_required(VERSION 3.17)
project(pfind C)

set(CMAKE_C_FLAGS "-O3 -pthread -Wall -D_POSIX_C_SOURCE=200809 -std=c11")
add_executable(pfind1 pfind.c)
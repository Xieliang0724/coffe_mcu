cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 固件版本：/api/status 的 version 字段、开机日志 “App version:” 均取此值。
# 每次发布前同步更新 CHANGELOG.md 与 sdkconfig.defaults（如有）。
project(coffe_mcu VERSION 1.3.2)

cmake_minimum_required(VERSION 3.16)
project(Quant_Sev VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QUANT_SEV_WITH_CTP "Link official CTP Md/Td libraries" OFF)

set(QUANT_SEV_LIB_SOURCES
  src/config/app_config.cpp
  src/logging/logger.cpp
  src/util/mask.cpp
  src/util/path_util.cpp
  src/util/http_get.cpp
  src/util/time_shanghai.cpp
  src/util/charset.cpp
  src/md/md_gateway.cpp
  src/td/td_gateway.cpp
  src/store/instrument_store.cpp
  src/store/session_calendar.cpp
  src/store/dominant_selector.cpp
  src/store/dominant_table.cpp
  src/store/liquidity.cpp
  src/store/chip_profile.cpp
  src/store/option_screener.cpp
  src/market/bar_builder.cpp
  src/market/parquet_bar_store.cpp
  src/market/tick_day_store.cpp
  src/market/tick_bus.cpp
  src/market/tick_snapshot_book.cpp
  src/market/quote_tape.cpp
  src/md/subscription_manager.cpp
  src/strategy/modules/modules.cpp
  src/strategy/ai_open/ai_open.cpp
  src/strategy/manual_open/manual_open.cpp
  src/strategy/fixed/fixed.cpp
  src/strategy/pyramid.cpp
  src/strategy/selftest.cpp
  src/strategy/engine.cpp
  src/strategy/live_runner.cpp
  src/strategy/ai_open/signal.cpp
  src/oms/oms.cpp
  src/oms/oms_session.cpp
  src/risk/risk_guard.cpp
)

add_executable(quant_sev src/main.cpp ${QUANT_SEV_LIB_SOURCES})
add_executable(quant_strategy_live src/strategy_live_main.cpp ${QUANT_SEV_LIB_SOURCES})

if(WIN32)
  set(QUANT_SEV_LIVE_ICO "${CMAKE_SOURCE_DIR}/res/quant_sev_live.ico")
  file(TO_CMAKE_PATH "${QUANT_SEV_LIVE_ICO}" QUANT_SEV_LIVE_ICO)
  configure_file(
    "${CMAKE_SOURCE_DIR}/res/quant_strategy_live.rc.in"
    "${CMAKE_BINARY_DIR}/quant_strategy_live.rc"
    @ONLY
  )
  target_sources(quant_strategy_live PRIVATE "${CMAKE_BINARY_DIR}/quant_strategy_live.rc")
endif()

function(quant_sev_setup_target tgt)
  target_include_directories(${tgt} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp/include
  )
  target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(QUANT_SEV_WITH_CTP)
    target_compile_definitions(${tgt} PRIVATE QUANT_SEV_WITH_CTP=1)
    target_link_libraries(${tgt} PRIVATE ${CTP_MD_LIB} ${CTP_TD_LIB})
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CTP_BIN_DIR}/thostmduserapi_se.dll"
        "$<TARGET_FILE_DIR:${tgt}>/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CTP_BIN_DIR}/thosttraderapi_se.dll"
        "$<TARGET_FILE_DIR:${tgt}>/"
    )
  endif()
endfunction()

if(QUANT_SEV_WITH_CTP)
  set(CTP_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp/lib)
  set(CTP_BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp/bin)
  find_library(CTP_MD_LIB NAMES thostmduserapi_se thostmduserapi PATHS ${CTP_LIB_DIR} NO_DEFAULT_PATH)
  find_library(CTP_TD_LIB NAMES thosttraderapi_se thosttraderapi PATHS ${CTP_LIB_DIR} NO_DEFAULT_PATH)
  if(NOT CTP_MD_LIB OR NOT CTP_TD_LIB)
    message(FATAL_ERROR "QUANT_SEV_WITH_CTP=ON but CTP libs not found in ${CTP_LIB_DIR}")
  endif()
  message(STATUS "CTP MD lib: ${CTP_MD_LIB}")
  message(STATUS "CTP TD lib: ${CTP_TD_LIB}")
else()
  message(STATUS "Building Quant_Sev skeleton without linking CTP (QUANT_SEV_WITH_CTP=OFF)")
endif()

quant_sev_setup_target(quant_sev)
quant_sev_setup_target(quant_strategy_live)

# Drop a double-clickable exe at the repo root (C:\Quant_Sev\Quant_Sev_Live.exe).
if(QUANT_SEV_WITH_CTP)
  add_custom_command(TARGET quant_strategy_live POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:quant_strategy_live>"
      "${CMAKE_SOURCE_DIR}/Quant_Sev_Live.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CTP_BIN_DIR}/thostmduserapi_se.dll"
      "${CMAKE_SOURCE_DIR}/thostmduserapi_se.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CTP_BIN_DIR}/thosttraderapi_se.dll"
      "${CMAKE_SOURCE_DIR}/thosttraderapi_se.dll"
  )
endif()

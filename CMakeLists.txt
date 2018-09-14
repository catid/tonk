cmake_minimum_required(VERSION 3.5)
project(tonk)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 11)

set(ZSTD_SOURCE_FILES
    thirdparty/zstd/bitstream.h
    thirdparty/zstd/compiler.h
    thirdparty/zstd/cpu.h
    thirdparty/zstd/entropy_common.c
    thirdparty/zstd/error_private.c
    thirdparty/zstd/error_private.h
    thirdparty/zstd/fse.h
    thirdparty/zstd/fse_compress.c
    thirdparty/zstd/fse_decompress.c
    thirdparty/zstd/huf.h
    thirdparty/zstd/huf_compress.c
    thirdparty/zstd/huf_decompress.c
    thirdparty/zstd/mem.h
    thirdparty/zstd/xxhash.c
    thirdparty/zstd/xxhash.h
    thirdparty/zstd/zstd.h
    thirdparty/zstd/zstd_common.c
    thirdparty/zstd/zstd_compress.c
    thirdparty/zstd/zstd_compress_internal.h
    thirdparty/zstd/zstd_decompress.c
    thirdparty/zstd/zstd_double_fast.c
    thirdparty/zstd/zstd_double_fast.h
    thirdparty/zstd/zstd_errors.h
    thirdparty/zstd/zstd_fast.c
    thirdparty/zstd/zstd_fast.h
    thirdparty/zstd/zstd_internal.h
    thirdparty/zstd/zstd_lazy.c
    thirdparty/zstd/zstd_lazy.h
    thirdparty/zstd/zstd_ldm.c
    thirdparty/zstd/zstd_ldm.h
    thirdparty/zstd/zstd_opt.c
    thirdparty/zstd/zstd_opt.h
)

set(LIB_SOURCE_FILES
    tonk.cpp
    tonk.h
    tonk_file_transfer.cpp
    tonk_file_transfer.h
    MappedFile.cpp
    MappedFile.h
    TonkineseBandwidth.cpp
    TonkineseBandwidth.h
    TonkineseConnection.cpp
    TonkineseConnection.h
    SimpleCipher.cpp
    SimpleCipher.h
    StrikeRegister.cpp
    StrikeRegister.h
    PacketCompression.cpp
    PacketCompression.h
    TonkineseFirewall.cpp
    TonkineseFirewall.h
    TonkineseFlood.cpp
    TonkineseFlood.h
    TonkineseIncoming.cpp
    TonkineseIncoming.h
    TonkineseInterfaces.h
    TonkineseMaps.cpp
    TonkineseMaps.h
    TonkineseOutgoing.cpp
    TonkineseOutgoing.h
    TonkineseProtocol.cpp
    TonkineseProtocol.h
    TonkineseSession.cpp
    TonkineseSession.h
    TonkineseTools.cpp
    TonkineseTools.h
    TonkineseUDP.cpp
    TonkineseUDP.h
    TonkineseNAT.cpp
    TonkineseNAT.h
    WLANOptimizer.cpp
    WLANOptimizer.h
    TimeSync.cpp
    TimeSync.h
    gf256.cpp
    gf256.h
    siamese.cpp
    siamese.h
    SiameseCommon.cpp
    SiameseCommon.h
    SiameseDecoder.cpp
    SiameseDecoder.h
    SiameseEncoder.cpp
    SiameseEncoder.h
    SiameseSerializers.h
    SiameseTools.cpp
    SiameseTools.h
    Counter.h
    Logger.cpp
    Logger.h
    PacketAllocator.cpp
    PacketAllocator.h
    cymric.cpp
    cymric.h
    thirdparty/asio.hpp
    thirdparty/blake2.h
    thirdparty/blake2b-ref.c
    thirdparty/blake2-impl.h
    thirdparty/chacha.c
    thirdparty/chacha.h
    thirdparty/chacha_blocks_ref.c
    thirdparty/IncludeAsio.h
    thirdparty/t1ha.c
    thirdparty/t1ha.h
)

set(LIB_TONK_CPP_SDK_SOURCE_FILES
		TonkCppSDK.cpp
		TonkCppSDK.h
)

set(TEST_SERVER_SOURCE_FILES
    tests/TonkServerTest.cpp
    tests/TonkTestTools.cpp
    tests/TonkTestTools.h
    Logger.cpp
    Logger.h
)

set(TEST_P2P_CLIENT_SOURCE_FILES
    tests/TonkP2PClientTest.cpp
    tests/TonkTestTools.cpp
    tests/TonkTestTools.h
    Logger.cpp
    Logger.h
)

set(UNIT_TEST_SOURCE_FILES
    tests/TonkUnitTest.cpp
    tests/TonkTestTools.cpp
    tests/TonkTestTools.h
    Logger.cpp
    Logger.h
    TimeSync.cpp
    TimeSync.h
)

set(FILE_SENDER_SOURCE_FILES
    tests/FileSender.cpp
    Logger.cpp
    Logger.h
)

set(FILE_RECEIVER_SOURCE_FILES
    tests/FileReceiver.cpp
    Logger.cpp
    Logger.h
)

set(P2P_FILE_SENDER_SOURCE_FILES
    tests/P2PFileSender.cpp
    Logger.cpp
    Logger.h
)

set(P2P_FILE_RECEIVER_SOURCE_FILES
    tests/P2PFileReceiver.cpp
    Logger.cpp
    Logger.h
)

set(P2P_RENDEZVOUS_SERVER_SOURCE_FILES
    tests/P2PRendezvousServer.cpp
    Logger.cpp
    Logger.h
)

set(LIB_MAU_SOURCE_FILES
    tests/mau/mau.cpp
    tests/mau/mau.h
    tests/mau/MauProxy.cpp
    tests/mau/MauProxy.h
    tests/mau/MauTools.cpp
    tests/mau/MauTools.h
)

set(UNIT_TEST_SOURCE_FILES
    tests/TonkUnitTest.cpp
    tests/BandwidthControlTest.cpp
    tests/BandwidthControlTest.h
    tests/TonkTestTools.cpp
    tests/TonkTestTools.h
    Logger.cpp
    Logger.h
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
else()
    set(CMAKE_CXX_FLAGS "-Wall -Wextra")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -march=native")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
endif()

include_directories(thirdparty .)

add_library(tonk SHARED ${LIB_SOURCE_FILES} ${ZSTD_SOURCE_FILES})
target_compile_definitions(tonk PRIVATE LOGGER_DISABLE_ATEXIT=1 SIAMESE_BUILDING=1 TONK_BUILDING=1 TONK_DLL=1)

add_library(tonk_static STATIC ${LIB_SOURCE_FILES} ${ZSTD_SOURCE_FILES} ${LIB_TONK_CPP_SDK_SOURCE_FILES})
target_compile_definitions(tonk_static PRIVATE SIAMESE_BUILDING=1 TONK_BUILDING=1 TONK_DISABLE_SHIM=1)

add_library(tonkcppsdk STATIC ${LIB_TONK_CPP_SDK_SOURCE_FILES})
target_link_libraries(tonkcppsdk ${CMAKE_DL_LIBS})

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(test_server ${TEST_SERVER_SOURCE_FILES})
target_link_libraries(test_server tonkcppsdk Threads::Threads)

add_executable(test_p2p_client ${TEST_P2P_CLIENT_SOURCE_FILES})
target_link_libraries(test_p2p_client tonkcppsdk Threads::Threads)

add_executable(FileSender ${FILE_SENDER_SOURCE_FILES})
target_link_libraries(FileSender tonkcppsdk Threads::Threads)

add_executable(FileReceiver ${FILE_RECEIVER_SOURCE_FILES})
target_link_libraries(FileReceiver tonkcppsdk Threads::Threads)

add_executable(P2PFileSender ${P2P_FILE_SENDER_SOURCE_FILES})
target_link_libraries(P2PFileSender tonkcppsdk Threads::Threads)

add_executable(P2PFileReceiver ${P2P_FILE_RECEIVER_SOURCE_FILES})
target_link_libraries(P2PFileReceiver tonkcppsdk Threads::Threads)

add_executable(P2PFileRendezvousServer ${P2P_RENDEZVOUS_SERVER_SOURCE_FILES})
target_link_libraries(P2PFileRendezvousServer tonkcppsdk Threads::Threads)

add_library(mau STATIC ${LIB_MAU_SOURCE_FILES})

add_executable(unit_tests ${UNIT_TEST_SOURCE_FILES})
target_link_libraries(unit_tests tonk_static mau tonkcppsdk Threads::Threads)

if(MSVC)
else()
    # FIXME: Remove -Wno-unused-local-typedef after updating Asio code
    set_target_properties(tonk PROPERTIES COMPILE_FLAGS -Wno-unused-variable)
endif()
